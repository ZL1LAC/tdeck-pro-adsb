#!/usr/bin/env python3
"""Read data/aircraftdb.bin back with an independent parser.

Deliberately does not import build_db.py. This is a second implementation of
the format, written from the layout rather than from the writer, so a wrong
stride, a byte-order slip or an unsorted table shows up as a failure here
instead of as a plausible-looking wrong registration on the glass.

    python tools/verify_db.py                     # structure + a sample
    python tools/verify_db.py --hex c82269 7c0c9a # look specific aircraft up
"""

import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), 'data', 'aircraftdb.bin')


def load(path):
    with open(path, 'rb') as f:
        blob = f.read()
    if len(blob) < 32:
        sys.exit('file is too short to hold a header')
    if blob[:8] != b'TDECKREG':
        sys.exit(f'bad magic {blob[:8]!r}')
    version, record_len = struct.unpack('<HH', blob[8:12])
    count, = struct.unpack('<I', blob[12:16])
    reg_len, type_len, _ = struct.unpack('<BBH', blob[16:20])
    print(f'version {version}  records {count}  record_len {record_len}  '
          f'reg_len {reg_len}  type_len {type_len}')

    want = 32 + count * record_len
    if len(blob) != want:
        sys.exit(f'expected {want} bytes for {count} records, file holds {len(blob)}')

    recs = []
    for i in range(count):
        off = 32 + i * record_len
        icao = (blob[off] << 16) | (blob[off + 1] << 8) | blob[off + 2]
        reg = blob[off + 3:off + 3 + reg_len].split(b'\0')[0].decode('ascii', 'replace')
        typ = blob[off + 3 + reg_len:off + 3 + reg_len + type_len].split(b'\0')[0].decode('ascii', 'replace')
        recs.append((icao, reg, typ))
    return recs


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--file', default=OUT)
    ap.add_argument('--hex', nargs='*', default=None,
                    help='ICAO addresses to look up, as the firmware would')
    args = ap.parse_args()

    recs = load(args.file)
    size = os.path.getsize(args.file)
    print(f'{size/1024:.0f} KB on SPIFFS\n')

    # The firmware binary-searches. Unsorted or duplicated keys would make it
    # return whichever record the search path happened to land on.
    bad = [i for i in range(1, len(recs)) if recs[i][0] <= recs[i - 1][0]]
    if bad:
        sys.exit(f'FAIL: {len(bad)} keys out of order or duplicated, '
                 f'first at index {bad[0]} ({recs[bad[0]][0]:06X})')
    print(f'PASS: {len(recs)} keys strictly ascending, '
          f'{recs[0][0]:06X}..{recs[-1][0]:06X}')

    empty = [r for r in recs if not r[1] and not r[2]]
    if empty:
        sys.exit(f'FAIL: {len(empty)} records carry neither registration nor type')
    print(f'PASS: every record carries a registration or a type')

    if args.hex:
        print()
        index = {r[0]: r for r in recs}
        for h in args.hex:
            try:
                key = int(h, 16)
            except ValueError:
                print(f'  {h:8} malformed')
                continue
            hit = index.get(key)
            print(f'  {h.lower():8} -> {hit[1]:10} {hit[2]}' if hit
                  else f'  {h.lower():8} -> not in this build')
    else:
        print('\nsample:')
        for r in recs[:3] + recs[len(recs) // 2:len(recs) // 2 + 2] + recs[-2:]:
            print(f'  {r[0]:06X}  {r[1]:10} {r[2]}')


if __name__ == '__main__':
    main()
