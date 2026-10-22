#!/usr/bin/env python3
"""Build the fastboot-boot-only TB128FU log reader: the tablet's own Android boot image (header v2; kernel and device tree section
reused byte for byte) with its ramdisk replaced by an initramfs holding init (init.c next to this file, static and libc-free), a FAT
image with LOG.BIN (32 MiB) and STATUS.TXT, and /log.offsets, where those two files' data lie inside the image. The reader offers
the image to the host as a read-only USB disk and restarts into fastboot when the host ejects it (scripts/read-tb128fu-log.py).
With --vendor-file, the image also has FIRMWARE.BIN (2 MiB) and the initramfs /vendor.list: init copies those files of the stock
vendor partition (read only, through the super partition's metadata and a loop device) into it.

usage: build-log-reader.py --boot boot_a.img --out reader.img [--vendor-partition vendor_a] [--vendor-file firmware/NAME ...]"""
import argparse
import gzip
import hashlib
import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
LLVM = Path('/opt/homebrew/opt/llvm/bin')
LOG_BYTES = 8192 * 4096
STATUS_BYTES = 4096
FIRMWARE_BYTES = 2 * 1024 * 1024
IMAGE_SECTORS = 34 * 2048          # 34 MiB: LOG.BIN, STATUS.TXT and the FAT16 metadata (2 MiB more with FIRMWARE.BIN)
VOLUME = 'UNIXNULOG'


def tool(name):
    # LLVM's own clang first: Xcode's links only Mach-O, and the Linux target needs ld.lld.
    found = (LLVM / name if (LLVM / name).exists() else None) or shutil.which(name)
    if not found:
        raise SystemExit(f'{name} not found')
    return str(found)


def build_init(work):
    out = work / 'init'
    subprocess.run([tool('clang'), '--target=aarch64-linux-gnu', '-O2', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                    '-fno-pic', '-static', '-nostdlib', '-fuse-ld=lld', '-Wl,--no-pie', '-Wl,-e,start', '-Wall', '-Werror',
                    '-o', out, HERE / 'init.c'], check=True)
    header = subprocess.run([tool('llvm-readelf'), '-h', out], capture_output=True, text=True, check=True).stdout
    if 'EXEC (Executable file)' not in header or 'AArch64' not in header:
        raise SystemExit('init is not a static AArch64 executable')
    if subprocess.run([tool('llvm-readelf'), '-d', out], capture_output=True, text=True).stdout.strip() not in ('', 'There is no dynamic section in this file.'):
        raise SystemExit('init has a dynamic section')
    return out.read_bytes()


def contiguous_file(image, name83):
    """Byte offset and size of a root-directory file of a FAT16 image whose clusters are contiguous."""
    bps, spc, reserved, fats, root_entries, _, _, fat_sectors = struct.unpack_from('<HBHBHHBH', image, 11)
    if image[54:62] != b'FAT16   ':
        raise SystemExit('expected a FAT16 image')
    fat = reserved * bps
    root = fat + fats * fat_sectors * bps
    data = root + root_entries * 32
    cluster_bytes = spc * bps
    for at in range(root, data, 32):
        if image[at:at + 11] == name83:
            first, size = struct.unpack_from('<HI', image, at + 26)
            cluster, count = first, 0
            while cluster < 0xFFF8:
                nxt = struct.unpack_from('<H', image, fat + 2 * cluster)[0]
                count += 1
                if nxt < 0xFFF8 and nxt != cluster + 1:
                    raise SystemExit(f'{name83!r} is fragmented')
                cluster = nxt
            if count * cluster_bytes < size:
                raise SystemExit(f'{name83!r} chain is short')
            return data + (first - 2) * cluster_bytes, size
    raise SystemExit(f'{name83!r} not in the root directory')


def build_image(work, firmware):
    image = work / 'log.img'
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
    sectors = IMAGE_SECTORS + (FIRMWARE_BYTES // 512 if firmware else 0)
    image.write_bytes(b'\0' * sectors * 512)
    subprocess.run(['mformat', '-i', image, '-T', str(sectors), '-h', '64', '-s', '32', '-c', '8', '-v', VOLUME, '::'],
                   env=env, check=True)
    (work / 'LOG.BIN').write_bytes(b'\0' * LOG_BYTES)
    (work / 'STATUS.TXT').write_bytes(b' ' * (STATUS_BYTES - 1) + b'\n')
    names = ['LOG.BIN', 'STATUS.TXT']
    if firmware:
        (work / 'FIRMWARE.BIN').write_bytes(b'\0' * FIRMWARE_BYTES)
        names.append('FIRMWARE.BIN')
    for name in names:
        subprocess.run(['mcopy', '-i', image, work / name, '::' + name], env=env, check=True)
    data = image.read_bytes()
    log_off, log_size = contiguous_file(data, b'LOG     BIN')
    status_off, status_size = contiguous_file(data, b'STATUS  TXT')
    if log_size != LOG_BYTES or status_size != STATUS_BYTES:
        raise SystemExit('unexpected file sizes in the FAT image')
    offsets = f'{log_off} {log_size} {status_off} {status_size}'
    if firmware:
        fw_off, fw_size = contiguous_file(data, b'FIRMWAREBIN')
        if fw_size != FIRMWARE_BYTES:
            raise SystemExit('unexpected FIRMWARE.BIN size in the FAT image')
        offsets += f' {fw_off} {fw_size}'
    return data, (offsets + '\n').encode()


def cpio(entries):
    """newc archive; entries are (path, mode, rdev major, rdev minor, data) with directories first."""
    out = bytearray()
    for ino, (name, mode, major, minor, data) in enumerate(entries + [('TRAILER!!!', 0, 0, 0, b'')], 1):
        path = name.encode() + b'\0'
        fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(path), 0)
        out += b'070701' + b''.join(b'%08x' % v for v in fields) + path
        out += b'\0' * (-len(out) % 4) + data
        out += b'\0' * (-len(out) % 4)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--boot', required=True, type=Path, help="the tablet's own boot image (header v2)")
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--vendor-partition', default='vendor_a', help='the logical partition of super to read files from')
    ap.add_argument('--vendor-file', action='append', default=[], help='a path inside that partition, e.g. firmware/Himax_firmware.bin')
    a = ap.parse_args()
    for path in a.vendor_file:
        if path.startswith('/') or '..' in path.split('/') or len(path) > 47 or '\n' in path:
            raise SystemExit(f'--vendor-file {path!r}: a relative path of at most 47 characters')
    boot = a.boot.read_bytes()
    if boot[:8] != b'ANDROID!':
        raise SystemExit('not an Android boot image')
    (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size, second_addr, tags, page, version,
     os_version) = struct.unpack_from('<10I', boot, 8)
    if version != 2 or second_size:
        raise SystemExit('expected a header v2 image without a second stage')
    recovery_dtbo_size, recovery_dtbo_offset, header_size, dtb_size, dtb_addr = struct.unpack_from('<IQIIQ', boot, 1632)
    if recovery_dtbo_size or not dtb_size:
        raise SystemExit('expected a device tree section and no recovery dtbo')
    pages = lambda n: (n + page - 1) // page * page
    kernel = boot[page:page + kernel_size]
    dtb_at = page + pages(kernel_size) + pages(ramdisk_size)
    dtb = boot[dtb_at:dtb_at + dtb_size]
    if len(kernel) != kernel_size or len(dtb) != dtb_size:
        raise SystemExit('truncated boot image')

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        init = build_init(work)
        image, offsets = build_image(work, bool(a.vendor_file))
    entries = [(d, 0o40755, 0, 0, b'') for d in ('dev', 'sys', 'proc')]
    entries += [('dev/console', 0o20600, 5, 1, b''), ('init', 0o100755, 0, 0, init),
                ('log.img', 0o100644, 0, 0, image), ('log.offsets', 0o100644, 0, 0, offsets)]
    if a.vendor_file:
        entries.append(('vendor.list', 0o100644, 0, 0, '\n'.join([a.vendor_partition] + a.vendor_file).encode() + b'\n'))
    ramdisk = gzip.compress(cpio(entries), 9, mtime=0)

    header = bytearray(boot[:page])
    struct.pack_into('<I', header, 16, len(ramdisk))
    digest = hashlib.sha1()
    for blob in (kernel, ramdisk, b'', b'', dtb):      # kernel, ramdisk, second, recovery dtbo, dtb (mkbootimg's id for v2)
        digest.update(blob)
        digest.update(struct.pack('<I', len(blob)))
    header[576:608] = digest.digest().ljust(32, b'\0')
    pad = lambda b: b + b'\0' * (-len(b) % page)
    out = bytes(header) + pad(kernel) + pad(ramdisk) + pad(dtb)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(out)
    print(f'BUILT {a.out} ({len(out)} bytes) sha256 {hashlib.sha256(out).hexdigest()} ramdisk {len(ramdisk)} '
          f'init {len(init)} offsets {offsets.decode().strip()}')


if __name__ == '__main__':
    main()
