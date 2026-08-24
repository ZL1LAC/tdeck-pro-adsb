#!/usr/bin/env python3
"""Build the T-Deck Pro aircraft registration database.

Turns the public tar1090 aircraft database into data/aircraftdb.bin, which
`pio run -t uploadfs` flashes into the SPIFFS partition alongside the basemap.
The firmware loads it into PSRAM at boot and binary-searches it by ICAO address
to fill in the registration and ICAO type designator.

This exists because a local receiver's aircraft.json carries neither: readsb
only serves them when it is started with a --db-file, and holding that database
in readsb costs a Raspberry Pi around 50 MB of RAM. Doing the lookup on the
device instead costs the Pi nothing.

The whole database is 615k records and 30 MB uncompressed, which does not fit
in a 3.4 MB SPIFFS partition and does not need to: an aircraft has to be within
radio range to appear on the plot, so only the ICAO address blocks you can
actually hear are worth carrying. New Zealand's C8 block is 4,898 records and
78 KB.

Source: https://github.com/wiedehopf/tar1090-db (ODbL), fetched on demand and
cached in tools/.dbcache. Only the Python standard library is used.

    python tools/build_db.py                       # New Zealand (C8)
    python tools/build_db.py --blocks C8,7C        # + Australia
    python tools/build_db.py --blocks all          # everything that fits
"""

import argparse
import gzip
import os
import struct
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CACHE = os.path.join(HERE, '.dbcache')
OUT = os.path.join(ROOT, 'data', 'aircraftdb.bin')

DB_URL = 'https://github.com/wiedehopf/tar1090-db/raw/csv/aircraft.csv.gz'

MAGIC = b'TDECKREG'
VERSION = 1

# 3-byte ICAO + 8-byte registration + 4-byte type + 1 pad. Sixteen rather than
# fifteen so that, with a 32-byte header, every record starts 16-byte aligned:
# the firmware indexes straight into a PSRAM buffer and the odd byte is worth
# less than the arithmetic it would cost.
REG_LEN, TYPE_LEN = 8, 4
RECORD_LEN = 16
HEADER_LEN = 32

# The partition is 3.4 MB and the basemap already lives there.
SPIFFS_BUDGET = 3 * 1024 * 1024


def fetch(url, name):
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, name)
    if os.path.exists(path) and os.path.getsize(path) > 0:
        print(f'  cached  {name} ({os.path.getsize(path)/1e6:.1f} MB)')
        return path
    print(f'  fetching {name} ...', end='', flush=True)
    req = urllib.request.Request(url, headers={'User-Agent': 'tdeckpro-adsb-dbbuild'})
    with urllib.request.urlopen(req, timeout=180) as r, open(path, 'wb') as f:
        f.write(r.read())
    print(f' {os.path.getsize(path)/1e6:.1f} MB')
    return path


def records(path, blocks):
    """Yield (icao24, reg, type) for rows in the wanted ICAO blocks.

    The file is semicolon-delimited and unquoted:
        hex;registration;type;flags;description;year;owner
    Rows carrying neither a registration nor a type are dropped -- they are
    placeholders, and a lookup that returns two empty strings is worse than a
    miss because it stops the aggregator ever filling the fields in.
    """
    kept = skipped = 0
    with gzip.open(path, 'rt', encoding='utf-8', errors='replace') as f:
        for line in f:
            parts = line.rstrip('\n').split(';')
            if len(parts) < 3:
                continue
            hexid, reg, typ = parts[0].strip(), parts[1].strip(), parts[2].strip()
            if len(hexid) != 6:
                continue
            if blocks is not None and hexid[:2].upper() not in blocks:
                continue
            try:
                icao = int(hexid, 16)
            except ValueError:
                continue
            if not reg and not typ:
                skipped += 1
                continue
            if len(reg) > REG_LEN or len(typ) > TYPE_LEN:
                # Truncating a registration would produce a wrong one, which is
                # worse than not having it. Long ones are rare enough to drop.
                skipped += 1
                continue
            kept += 1
            yield icao, reg, typ
    print(f'  kept {kept}, skipped {skipped} (empty or over-long)')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--blocks', default='C8',
                    help='comma-separated leading ICAO hex byte(s) to include, '
                         'e.g. "C8" for New Zealand, "C8,7C" to add Australia, '
                         'or "all" (default: C8)')
    ap.add_argument('--out', default=OUT)
    args = ap.parse_args()

    blocks = None if args.blocks.lower() == 'all' else {
        b.strip().upper() for b in args.blocks.split(',') if b.strip()}

    print('Aircraft database')
    src = fetch(DB_URL, 'aircraft.csv.gz')
    print(f'  blocks: {"all" if blocks is None else ",".join(sorted(blocks))}')

    rows = sorted(records(src, blocks), key=lambda r: r[0])
    if not rows:
        sys.exit('no records matched -- check --blocks')

    # The firmware binary-searches, so duplicates would make which one it finds
    # depend on the search path. Last write wins, matching the CSV's own order.
    deduped = {}
    for icao, reg, typ in rows:
        deduped[icao] = (reg, typ)
    rows = [(k, v[0], v[1]) for k, v in sorted(deduped.items())]

    size = HEADER_LEN + len(rows) * RECORD_LEN
    if size > SPIFFS_BUDGET:
        sys.exit(f'{size/1e6:.1f} MB exceeds the SPIFFS budget -- narrow --blocks')

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'wb') as f:
        header = (MAGIC
                  + struct.pack('<HH', VERSION, RECORD_LEN)
                  + struct.pack('<I', len(rows))
                  + struct.pack('<BBH', REG_LEN, TYPE_LEN, 0)
                  + struct.pack('<I', 0))
        f.write(header.ljust(HEADER_LEN, b'\0'))
        for icao, reg, typ in rows:
            f.write(bytes(((icao >> 16) & 0xFF, (icao >> 8) & 0xFF, icao & 0xFF)))
            f.write(reg.encode('ascii', 'replace').ljust(REG_LEN, b'\0'))
            f.write(typ.encode('ascii', 'replace').ljust(TYPE_LEN, b'\0'))
            f.write(b'\0')

    print(f'\n  {len(rows)} records -> {args.out} ({size/1024:.0f} KB, '
          f'{100.0*size/SPIFFS_BUDGET:.1f}% of the SPIFFS budget)')
    print('  flash it with: pio run -t uploadfs')


if __name__ == '__main__':
    main()
