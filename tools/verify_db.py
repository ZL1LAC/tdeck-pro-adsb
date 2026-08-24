#!/usr/bin/env python3
"""Read data/aircraftdb.bin back with an independent parser.

Deliberately does not import build_db.py. This is a second implementation of
the format, written from the layout rather than from the writer, so a wrong
stride, a byte-order slip, a dangling string index or an unsorted table shows
up as a failure here instead of as a plausible-looking wrong registration on
the glass.

    python tools/verify_db.py                     # structure + a sample
    python tools/verify_db.py --hex c82269 7c0c9a # look specific aircraft up
"""

import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), 'data', 'aircraftdb.bin')

NO_STRING = 0xFFFF


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
    str_count, strtab_off = struct.unpack('<II', blob[20:28])
    pool_off, = struct.unpack('<I', blob[28:32])
    if version != 2:
        sys.exit(f'this tool reads version 2, file is version {version}')
    print(f'version {version}  records {count}  record_len {record_len}  '
          f'reg_len {reg_len}  type_len {type_len}  strings {str_count}')

    if 32 + count * record_len != strtab_off:
        sys.exit(f'records end at {32 + count * record_len}, string table '
                 f'claims to start at {strtab_off}')
    if strtab_off + str_count * 4 != pool_off:
        sys.exit(f'string table ends at {strtab_off + str_count * 4}, pool '
                 f'claims to start at {pool_off}')

    # Resolve the pool independently of the writer's ordering.
    strings = []
    for i in range(str_count):
        off, = struct.unpack('<I', blob[strtab_off + i * 4:strtab_off + i * 4 + 4])
        end = blob.index(b'\0', pool_off + off)
        strings.append(blob[pool_off + off:end].decode('ascii', 'replace'))

    def text(idx):
        if idx == NO_STRING:
            return ''
        if idx >= str_count:
            sys.exit(f'string index {idx} is past the {str_count}-entry table')
        return strings[idx]

    recs = []
    for i in range(count):
        off = 32 + i * record_len
        icao = (blob[off] << 16) | (blob[off + 1] << 8) | blob[off + 2]
        reg = blob[off + 3:off + 3 + reg_len].split(b'\0')[0].decode('ascii', 'replace')
        typ = blob[off + 3 + reg_len:off + 3 + reg_len + type_len].split(b'\0')[0].decode('ascii', 'replace')
        d, o, yr = struct.unpack('<HHH', blob[off + 15:off + 21])
        recs.append((icao, reg, typ, text(d), text(o), yr))
    return recs, strings, len(blob)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--file', default=OUT)
    ap.add_argument('--hex', nargs='*', default=None,
                    help='ICAO addresses to look up, as the firmware would')
    args = ap.parse_args()

    recs, strings, size = load(args.file)
    print(f'{size/1024:.0f} KB on SPIFFS\n')

    # The firmware binary-searches. Unsorted or duplicated keys would make it
    # return whichever record the search path happened to land on.
    bad = [i for i in range(1, len(recs)) if recs[i][0] <= recs[i - 1][0]]
    if bad:
        sys.exit(f'FAIL: {len(bad)} keys out of order or duplicated, '
                 f'first at index {bad[0]} ({recs[bad[0]][0]:06X})')
    print(f'PASS: {len(recs)} keys strictly ascending, '
          f'{recs[0][0]:06X}..{recs[-1][0]:06X}')

    empty = [r for r in recs if not r[1] and not r[2] and not r[3]]
    if empty:
        sys.exit(f'FAIL: {len(empty)} records carry nothing at all')
    print('PASS: every record carries a registration, type or description')

    used = sum(1 for s in strings if s)
    if used != len(strings):
        sys.exit(f'FAIL: {len(strings) - used} empty strings in the pool')
    print(f'PASS: {len(strings)} pooled strings all resolve and are non-empty')

    dupes = len(strings) - len(set(strings))
    if dupes:
        sys.exit(f'FAIL: pool holds {dupes} duplicate strings -- interning broke')
    print('PASS: no duplicates in the pool')

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
            if not hit:
                print(f'  {h.lower():8} not in this build')
                continue
            print(f'  {h.lower():8} {hit[1]:9} {hit[2]:5} {hit[3]}')
            if hit[4] or hit[5]:
                print(f'           {"":9} {"":5} {hit[4]}'
                      f'{"" if not hit[5] else f" ({hit[5]})"}')
    else:
        print('\nsample:')
        for r in recs[:2] + recs[len(recs) // 2:len(recs) // 2 + 2] + recs[-2:]:
            print(f'  {r[0]:06X}  {r[1]:9} {r[2]:5} {r[3][:34]:34} {r[5] or "":4} {r[4][:24]}')


if __name__ == '__main__':
    main()
