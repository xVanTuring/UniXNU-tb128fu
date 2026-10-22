#!/usr/bin/env python3
"""Check the boot inputs of a TB128FU image before flashing.

Reads the SHNGROOT manifest and the SHNGPOL1 policy directory of the userdata image, verifies every
owner signature with the compiled-in 395-byte anchor (pure Python RSA, no host dylib), and checks that
the signed volume identities match the real APFS volumes and the Data seed. Also checks the kernel,
the boot image size and the optional trust-cache pin.

  check-boot-inputs.py [--out ../UniXNU/out/tb128fu] [--seed-report ../UniXNU/build/owner-data-fixture/seed.json]
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

OS_UUID = '04769D26-2CA0-4825-B443-E2E5F88300F2'
SYSTEMOS_UUID = '8846FE78-2910-4297-AF97-D59BB6F8FC16'
DATA_UUID = '73F1FA24-4189-446D-96D4-FB6324EBE955'
TRUST_PIN = '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7'
BOOT_LIMIT = 96 * 1024 * 1024
DIGEST_INFO = bytes.fromhex('3031300d060960864801650304020105000420')


def rsa3072_verify(anchor, signature, message):
    if len(anchor) != 395 or anchor[:8] != b'VROOTKEY' or anchor[392:] != b'\x01\x00\x01':
        return False
    modulus = int.from_bytes(anchor[8:392], 'big')
    if not modulus & (1 << 3071) or not modulus & 1:
        return False
    actual = pow(int.from_bytes(signature, 'big'), 65537, modulus).to_bytes(384, 'big')
    digest = hashlib.sha256(message).digest()
    expected = b'\x00\x01' + b'\xff' * (384 - 3 - len(DIGEST_INFO) - len(digest)) + b'\x00' + DIGEST_INFO + digest
    return actual == expected


def volume_uuid(raw):
    hexes = raw.hex().upper()
    return '-'.join((hexes[0:8], hexes[8:12], hexes[12:16], hexes[16:20], hexes[20:32]))


def fail(message):
    print(f'FAIL: {message}')
    sys.exit(1)


def check_userdata(image, anchor, expected):
    data = image.open('rb')
    head = data.read(4096)
    if head[:8] != b'SHNGROOT':
        fail('userdata has no SHNGROOT manifest')
    version, windows = struct.unpack_from('<II', head, 8)
    image_bytes = struct.unpack_from('<Q', head, 16)[0]
    if version != 1 or not 0 < windows <= 4:
        fail('SHNGROOT version/window count')
    if image_bytes != image.stat().st_size:
        fail(f'SHNGROOT image size {image_bytes} != file size {image.stat().st_size}')
    if hashlib.sha256(head[:4064]).digest() != head[4064:4096]:
        fail('SHNGROOT manifest hash')
    layout = {}
    for index in range(windows):
        name, offset, size, block, flags, digest = struct.unpack_from('<32sQQII32s', head, 24 + 128 * index)
        name = name.rstrip(b'\0').decode()
        if not name or block != 512 or flags != 1:
            fail(f'window {index} attributes')
        if (offset & 4095) or offset < 4096 or size > image_bytes - offset:
            fail(f'window {name} bounds')
        layout[name] = (offset, size, digest.hex())
        print(f'window {name}: {size} bytes at {offset:#x} sha256 {digest.hex()[:16]}…')

    data.seek(4096)
    policy = data.read(4096)
    if policy[:8] != b'SHNGPOL1':
        fail('userdata has no SHNGPOL1 policy directory')
    pversion, count, entry_bytes = struct.unpack_from('<III', policy, 8)
    if pversion != 1 or entry_bytes != 88 or not 0 < count <= 4:
        fail('SHNGPOL1 version/entry format')
    if hashlib.sha256(policy[:4064]).digest() != policy[4064:4096]:
        fail('SHNGPOL1 manifest hash')
    seen = set()
    for index in range(count):
        name, offset, size, block, flags, digest = struct.unpack_from('<32sQQII32s', policy, 24 + 88 * index)
        name = name.rstrip(b'\0').decode()
        if name in seen:
            fail(f'duplicate policy {name}')
        seen.add(name)
        if name not in layout:
            fail(f'policy {name} has no window')
        if (offset & 4095) or offset < 8192 or size < 544 or offset + ((size + 4095) & ~4095) > image_bytes:
            fail(f'policy {name} bounds')
        if block != 4096 or flags != 1:
            fail(f'policy {name} attributes')
        data.seek(offset)
        manifest = data.read(size)
        if hashlib.sha256(manifest).digest() != digest:
            fail(f'policy {name} blob hash')
        if manifest[:8] != b'VROOT01\x00':
            fail(f'policy {name} magic')
        header, mblock = struct.unpack_from('<I', manifest, 12)[0], struct.unpack_from('<I', manifest, 16)[0]
        signed_bytes, blocks = struct.unpack_from('<Q', manifest, 24)[0], struct.unpack_from('<Q', manifest, 32)[0]
        window_offset, window_size, window_hash = layout[name]
        if header != 160 or mblock != 4096 or len(manifest) != 544 + blocks * 32:
            fail(f'policy {name} header/table size')
        if signed_bytes != window_size:
            fail(f'policy {name} signs {signed_bytes} bytes, window is {window_size}')
        if blocks != (signed_bytes + 4095) // 4096:
            fail(f'policy {name} block count')
        if manifest[56:88].hex() != window_hash:
            fail(f'policy {name} image hash differs from the window hash')
        if hashlib.sha256(anchor).digest() != manifest[120:152]:
            fail(f'policy {name} key anchor')
        if not rsa3072_verify(anchor, manifest[160:544], manifest[:160]):
            fail(f'policy {name} RSA signature')
        table = manifest[544:]
        if hashlib.sha256(table).digest() != manifest[88:120]:
            fail(f'policy {name} hash-table digest')
        uuid = volume_uuid(manifest[40:56])
        if uuid != expected[name]:
            fail(f'policy {name} signs volume {uuid}, expected {expected[name]}')
        print(f'policy {name}: {blocks} blocks, volume UUID {uuid}, signature and table verified')
    missing = set(expected) - seen
    if missing:
        fail(f'policies missing for {sorted(missing)}')
    return layout


def check_payload(path):
    if not path.is_file():
        print(f'payload: {path} missing, args not checked')
        return
    try:
        text = subprocess.run(['mtype', '-i', str(path), '::/EFI/UniXNU/unixnu.toml'],
                              capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        print('payload: mtype unavailable or image unreadable, args not checked')
        return
    for needle in ('native-ufs-root=1', 'sheng-data-persist=2', 'boot-progress=2', 'Payloads/scratch-data.img'):
        if needle not in text:
            fail(f'payload lacks {needle}')
    print('payload: root, persistence and boot-progress arguments plus the Data initrd are present')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=Path('../UniXNU/out/tb128fu'), help='build output directory')
    parser.add_argument('--userdata', type=Path)
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--boot', type=Path)
    parser.add_argument('--payload', type=Path)
    parser.add_argument('--key', type=Path, help='trusted-key.bin of the policy build')
    parser.add_argument('--seed-report', type=Path, default=Path('../UniXNU/build/owner-data-fixture/seed.json'))
    parser.add_argument('--trust-cache', type=Path, help='bundle to check against the pin')
    args = parser.parse_args()
    out = args.out.resolve()
    userdata = args.userdata or out / 'userdata/userdata-root.img'
    kernel = args.kernel or out / 'kernel/kernel.macho'
    boot = args.boot or out / 'boot/boot.img'
    payload = args.payload or out / 'payload/payload.img'
    key = args.key or out / 'tree/build/verified-root-os/trusted-key.bin'
    anchor = key.read_bytes()
    if hashlib.sha256(anchor).hexdigest() != '70a5ea346d2d779a7ef43eb4ed8b17f54488e160d8c2a6d6420969664e3ae798':
        fail('the owner anchor is not the pinned key')
    check_userdata(userdata, anchor, {'OS': OS_UUID, 'SystemOS': SYSTEMOS_UUID})

    if not kernel.is_file() or not boot.is_file():
        fail('kernel or boot image missing')
    if boot.stat().st_size > BOOT_LIMIT:
        fail(f'boot image {boot.stat().st_size} exceeds {BOOT_LIMIT}')
    symbols = subprocess.run(['xcrun', 'nm', str(kernel)], capture_output=True, text=True)
    if symbols.returncode:
        fail('cannot read kernel symbols')
    for symbol in ('_native_verified_block_root_authorized', '_native_ram_block_volume_seed',
                   '_native_owner_data_stage', '_native_owner_cache_stage', '_boot_progress_init', '_vr_open'):
        if symbol not in symbols.stdout:
            fail(f'kernel lacks {symbol}')
    print(f'kernel: {kernel.stat().st_size} bytes, owner chain and boot-progress symbols present')
    print(f'boot image: {boot.stat().st_size} bytes')
    check_payload(payload)

    if args.trust_cache:
        digest = hashlib.sha256(args.trust_cache.read_bytes()).hexdigest()
        if digest != TRUST_PIN:
            fail(f'trust cache is {digest}, not the board pin')
        print('trust cache: matches the board pin')

    if args.seed_report.is_file():
        report = json.loads(args.seed_report.read_text())
        if report.get('volume_uuid', '').upper() != DATA_UUID:
            fail(f"Data seed volume {report.get('volume_uuid')} != {DATA_UUID}")
        print(f"Data seed: volume UUID {DATA_UUID} (base seed {report.get('bytes')} bytes)")
    else:
        print(f'Data seed: report {args.seed_report} not found, UUID not checked')
    print('PASS: boot inputs are internally consistent')


if __name__ == '__main__':
    main()
