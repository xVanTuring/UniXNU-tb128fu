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
import shutil
import subprocess
import sys
import time
from pathlib import Path

UNIXNU = Path(__file__).resolve().parents[2] / 'UniXNU'
# The window has the sheng format (package/BootLog.c is the sheng writer); its decoder needs Python 3.12 or later.
DECODER = Path(__file__).resolve().parents[2] / 'UniXNU-sheng/scripts/read-sheng-log.py'
# The tablet's fastboot serial (TB128FU_SERIAL); without it, any device in fastboot counts.
SERIAL = os.environ.get('TB128FU_SERIAL', '')


def in_fastboot():
    out = subprocess.run(['fastboot', 'devices'], capture_output=True, text=True).stdout
    return any(line.split()[0] == SERIAL if SERIAL else line.strip().endswith('fastboot')
               for line in out.splitlines() if line.strip())


def find_volume():
    for volume in sorted(glob.glob('/Volumes/UNIXNULOG*')):
        if os.path.isfile(os.path.join(volume, 'LOG.BIN')) and os.path.isfile(os.path.join(volume, 'STATUS.TXT')):
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
        if not in_fastboot():
            sys.exit('the tablet is not in fastboot')
        print(f'fastboot boot {a.reader}')
        subprocess.run(['fastboot', 'boot', str(a.reader)], check=True)
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
    names = ['STATUS.TXT', 'LOG.BIN'] + (['FIRMWARE.BIN'] if os.path.isfile(os.path.join(volume, 'FIRMWARE.BIN')) else [])
    for name in names:
        shutil.copyfile(os.path.join(volume, name), run / name)
    log = (run / 'LOG.BIN').read_bytes()
    print((run / 'STATUS.TXT').read_text(errors='replace').rstrip())
    print(f'LOG.BIN {len(log)} bytes, sha256 {hashlib.sha256(log).hexdigest()}, first bytes {log[:8]!r}')
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
    else:
        print('decode with: ' + ' '.join(decode))


if __name__ == '__main__':
    main()
