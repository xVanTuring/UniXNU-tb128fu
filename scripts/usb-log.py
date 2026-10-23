#!/usr/bin/env python3
"""Live board log from the TB128FU over USB (package/UsbLog.cpp: a CDC ACM port, /dev/cu.usbmodem* on macOS).

Waits for the port, puts it in raw mode, appends everything it reads to out/tb128fu/usb-log/<time>.log (one file per connection)
and keeps the newest complete snapshot (from one "#slot" line to the next) in latest.txt beside it. Standard library only.

  python3 scripts/usb-log.py [--dir DIR] [--port /dev/cu.usbmodemXXXX] [--quiet]
"""
import argparse
import glob
import os
import sys
import termios
import time
from pathlib import Path

UNIXNU = Path(__file__).resolve().parents[2] / 'UniXNU'


def find_port(explicit):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    ports = sorted(glob.glob('/dev/cu.usbmodem*'))
    return ports[0] if ports else None


def raw(fd):
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0                                  # iflag
    attrs[1] = 0                                  # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0                                  # lflag: no echo, no canonical mode
    attrs[6][termios.VMIN] = 1
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dir', type=Path, default=UNIXNU / 'out/tb128fu/usb-log')
    ap.add_argument('--port')
    ap.add_argument('--quiet', action='store_true', help='do not echo to the terminal')
    args = ap.parse_args()
    args.dir.mkdir(parents=True, exist_ok=True)
    while True:
        port = find_port(args.port)
        if not port:
            time.sleep(1)
            continue
        try:
            fd = os.open(port, os.O_RDONLY | os.O_NOCTTY)
        except OSError:
            time.sleep(1)
            continue
        raw(fd)
        path = args.dir / time.strftime('%Y%m%d-%H%M%S.log')
        print(f'# {port} -> {path}', file=sys.stderr, flush=True)
        latest = bytearray()
        with open(path, 'ab') as log:
            while True:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                log.write(data)
                log.flush()
                if not args.quiet:
                    sys.stdout.write(data.decode('utf-8', 'replace'))
                    sys.stdout.flush()
                latest += data
                # A new "#slot" line ends the snapshot before it: that one is the newest complete snapshot.
                cut = latest.rfind(b'#slot ')
                if cut > 0:
                    start = latest.rfind(b'#slot ', 0, cut)
                    if start >= 0:
                        (args.dir / 'latest.txt').write_bytes(bytes(latest[start:cut]))
                    latest = latest[cut:]
                if len(latest) > (4 << 20):
                    latest = latest[-(1 << 20):]
        os.close(fd)
        print(f'# {port} closed', file=sys.stderr, flush=True)
        time.sleep(1)


if __name__ == '__main__':
    main()
