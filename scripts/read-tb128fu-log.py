#!/usr/bin/env python3
"""Fetch the UniXNU diagnostic log window from a TB128FU in fastboot: boot the log reader (package/linux-log/build-log-reader.py) with
`fastboot boot`, wait for its read-only USB disk (volume UNIXNULOG), copy LOG.BIN and STATUS.TXT, eject the disk (the reader then
restarts into fastboot) and wait for fastboot again.

usage: read-tb128fu-log.py [--reader reader.img] [--out DIR] [--no-boot] [--wait 120]
Output: DIR/<time>/LOG.BIN (the last 32 MiB of userdata as the reader found it) and STATUS.TXT (what the reader did). A reader
built with --vendor-file also has FIRMWARE.BIN; its files go to DIR/<time>/vendor/<path>."""
import argparse
import glob
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path, PurePosixPath

UNIXNU = Path(__file__).resolve().parents[2] / 'UniXNU'
# The window has the sheng format (package/BootLog.c is the sheng writer); its decoder needs Python 3.12 or later.
DECODER = Path(__file__).resolve().parents[2] / 'UniXNU-sheng/scripts/read-sheng-log.py'
EXPORT_UNPACK = ('import importlib.util, pathlib, sys; spec = importlib.util.spec_from_file_location("decoder", sys.argv[1]); '
                 'decoder = importlib.util.module_from_spec(spec); spec.loader.exec_module(decoder); '
                 'decoder.unpack_export(pathlib.Path(sys.argv[2]).read_bytes(), pathlib.Path(sys.argv[3]))')
# The tablet's fastboot serial (TB128FU_SERIAL); without it, the only device in fastboot counts, and two or more are refused.
SERIAL = os.environ.get('TB128FU_SERIAL', '')


def fastboot_serials():
    out = subprocess.run(['fastboot', 'devices'], capture_output=True, text=True).stdout
    return [line.split()[0] for line in out.splitlines() if line.strip().endswith('fastboot')]


def in_fastboot():
    serials = fastboot_serials()
    return SERIAL in serials if SERIAL else len(serials) == 1


def fastboot(*args):
    return ['fastboot'] + (['-s', SERIAL] if SERIAL else []) + list(args)


def find_volume():
    """Only this reader's disk: another tablet's reader (pipa, 2026-10-22) offered a volume of the same name at the same time and
    each script copied and ejected the other one's. STATUS.TXT names the reader on its first line."""
    for volume in sorted(glob.glob('/Volumes/UNIXNULOG*')):
        status = os.path.join(volume, 'STATUS.TXT')
        if os.path.isfile(os.path.join(volume, 'LOG.BIN')) and os.path.isfile(status):
            with open(status, errors='replace') as f:
                if f.readline().strip() == 'tb128fu log reader':
                    return volume
    return None


def unpack_firmware(blob, out):
    """FIRMWARE.BIN (package/linux-log/init.c): 64-byte records ("UXFILE01", signed size or -errno, path), data padded to 8."""
    at = 0
    while at + 64 <= len(blob) and blob[at:at + 8] == b'UXFILE01':
        size = int.from_bytes(blob[at + 8:at + 16], 'little', signed=True)
        path = blob[at + 16:at + 64].split(b'\0')[0].decode()
        if size < 0:
            print(f'vendor/{path}: error {-size}')
            at += 64
            continue
        data = blob[at + 64:at + 64 + size]
        target = out / 'vendor' / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        print(f'vendor/{path}: {len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()}')
        at += 64 + (size + 7) // 8 * 8
    if blob[at:at + 8] != b'UXEND000':
        print('FIRMWARE.BIN: no end record (nothing copied, or the reader stopped early)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--reader', type=Path, default=UNIXNU / 'out/tb128fu/log-reader/reader.img')
    ap.add_argument('--out', type=Path, default=UNIXNU / 'out/tb128fu/device-logs')
    ap.add_argument('--no-boot', action='store_true', help='the reader is already running')
    ap.add_argument('--wait', type=int, default=120, help='seconds to wait for the disk')
    a = ap.parse_args()
    if find_volume():
        sys.exit('a UNIXNULOG volume is already mounted; eject it first')
    if not a.no_boot:
        if not SERIAL and len(fastboot_serials()) > 1:
            sys.exit('more than one device in fastboot; set TB128FU_SERIAL')
        if not in_fastboot():
            sys.exit('the tablet is not in fastboot')
        print(' '.join(fastboot('boot', str(a.reader))))
        subprocess.run(fastboot('boot', str(a.reader)), check=True)
    start = time.time()
    volume = None
    while time.time() - start < a.wait:
        volume = find_volume()
        if volume:
            break
        time.sleep(1)
    if not volume:
        sys.exit(f'no UNIXNULOG disk within {a.wait} s (the reader returns to fastboot by itself after 300 s)')
    print(f'disk {volume} after {time.time() - start:.0f} s')
    run = a.out / time.strftime('%Y%m%d-%H%M%S')
    run.mkdir(parents=True)
    names = ['STATUS.TXT', 'LOG.BIN'] + [name for name in ('EXPORT.BIN', 'FIRMWARE.BIN') if os.path.isfile(os.path.join(volume, name))]
    for name in names:
        shutil.copyfile(os.path.join(volume, name), run / name)
    log = (run / 'LOG.BIN').read_bytes()
    status = (run / 'STATUS.TXT').read_text(errors='replace')
    print(status.rstrip())
    print(f'LOG.BIN {len(log)} bytes, sha256 {hashlib.sha256(log).hexdigest()}, first bytes {log[:8]!r}')
    # EXPORT.BIN holds the export from its start; "export.copy = N" is how much of it the reader filled (0: none, negative: failed).
    copied = re.search(r'^export\.copy = (-?\d+)$', status, re.M)
    if 'EXPORT.BIN' in names and copied and int(copied[1]) > 0:
        export = run / 'EXPORT.BIN'
        export.write_bytes(export.read_bytes()[:int(copied[1])])
    elif 'EXPORT.BIN' in names:
        (run / 'EXPORT.BIN').unlink()
    if 'FIRMWARE.BIN' in names:
        unpack_firmware((run / 'FIRMWARE.BIN').read_bytes(), run)
    subprocess.run(['diskutil', 'eject', volume], check=True)
    start = time.time()
    while time.time() - start < 120 and not in_fastboot():
        time.sleep(1)
    print(f'saved {run}; ' + ('back in fastboot' if in_fastboot() else 'not back in fastboot within 120 s'))
    decode = ['uv', 'run', '--no-project', '--python', '3.13', str(DECODER), '--decode', str(run / 'LOG.BIN'), '--out', str(run / 'decoded')]
    if DECODER.is_file() and shutil.which('uv'):
        subprocess.run(decode)
        # The decoder only reports a version 3 export ("fetched separately"); its unpack_export writes export.bin and export/.
        if (run / 'EXPORT.BIN').is_file():
            unpacked = subprocess.run(['uv', 'run', '--no-project', '--python', '3.13', 'python', '-c', EXPORT_UNPACK, str(DECODER),
                                       str(run / 'EXPORT.BIN'), str(run)])
            if unpacked.returncode:
                salvage_export((run / 'EXPORT.BIN').read_bytes(), run / 'export' / 'files')
    else:
        print('decode with: ' + ' '.join(decode))


def salvage_export(blob, out):
    """The whole files of an export the reader copied only in part (its first 32 MiB; the plugin sends the cache directory's own
    files first, CacheExport.h): the "SHEXR001" stream's files in order, the one cut short as <name>.partial."""
    if blob[:8] != b'SHEXR001':
        return
    raw = blob[16:16 + struct.unpack_from('<I', blob, 8)[0]]
    at = 12 + struct.unpack_from('<I', raw, 8)[0]
    count = struct.unpack_from('<I', raw, at)[0]
    at += 4
    whole = 0
    for _ in range(count):
        if at + 24 > len(raw):
            break
        plen, _, _, _, size = struct.unpack_from('<IIIIQ', raw, at)
        rel = raw[at + 24:at + 24 + plen].decode(errors='replace')
        at += 24 + plen
        if rel.startswith('/') or '..' in PurePosixPath(rel).parts:
            break
        data = raw[at:at + size]
        at += size
        dest = out / (rel if len(data) == size else rel + '.partial')
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        whole += len(data) == size
    print(f'{out}: salvaged {whole} of {count} files from a cut export')


if __name__ == '__main__':
    main()
