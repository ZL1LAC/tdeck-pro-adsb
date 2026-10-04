#!/usr/bin/env python3
"""Build the T-Deck Pro aircraft database.

Turns the public tar1090 aircraft database into data/aircraftdb.bin, which
`pio run -t uploadfs` flashes into the SPIFFS partition alongside the basemap.
The firmware loads it into PSRAM at boot and binary-searches it by ICAO address
for the registration, type designator, description, year and operator.

This exists because a local receiver's aircraft.json carries none of them:
readsb only serves them when started with a --db-file, and holding that
database in readsb costs a Raspberry Pi around 50 MB of RAM. Doing the lookup
on the device instead costs the Pi nothing and needs no network -- the mapping
is fixed, so there is nothing to keep fresh but a reflash.

The whole database is 615k records and 30 MB uncompressed, which does not fit
in SPIFFS or in the 8 MB of PSRAM. A regional slice is the right thing to
flash; the worldwide table belongs on the SD card, where the firmware seeks it
on demand.

Source: https://github.com/wiedehopf/tar1090-db (ODbL), fetched on demand and
cached in tools/.dbcache. Only the Python standard library is used.

    python tools/build_db.py                       # New Zealand (C8)
    python tools/build_db.py --blocks C8,7C        # + Australia
    python tools/build_db.py --blocks all          # worldwide, for the SD card
    python tools/build_db.py --blocks C8,7C --no-operators
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
VERSION = 2

# 3-byte ICAO + 8-byte registration + 4-byte type + description index +
# operator index + year + 3 pad = 24. The two text fields are indices into a
# string table rather than inline text: the 20,475 aircraft in C8+7C share just
# 1,336 distinct descriptions between them, so inlining would cost 3.9 MB and
# not fit, where pooling costs 800 KB and does.
REG_LEN, TYPE_LEN = 8, 4
RECORD_LEN = 24
HEADER_LEN = 32
NO_STRING = 0xFFFF

# Covers every description in the source (the longest is 47) and keeps an
# operator name to something a 40-column panel can show in two lines.
MAX_TEXT = 47

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


def records(path, blocks, want_operators):
    """Yield (icao, reg, type, desc, year, operator) for the wanted blocks.

    The file is semicolon-delimited and unquoted:
        hex;registration;type;flags;description;year;owner

    Rows carrying nothing usable are dropped. A lookup that returns only empty
    strings is worse than a miss, because the firmware treats a hit as final
    and would stop an aggregator ever filling the fields in.
    """
    kept = skipped = 0
    with gzip.open(path, 'rt', encoding='utf-8', errors='replace') as f:
        for line in f:
            parts = line.rstrip('\n').split(';')
            if len(parts) < 3:
                continue
            hexid, reg, typ = parts[0].strip(), parts[1].strip(), parts[2].strip()
            desc = parts[4].strip() if len(parts) > 4 else ''
            year = parts[5].strip() if len(parts) > 5 else ''
            oper = parts[6].strip() if len(parts) > 6 and want_operators else ''
            if len(hexid) != 6:
                continue
            if blocks is not None and hexid[:2].upper() not in blocks:
                continue
            try:
                icao = int(hexid, 16)
            except ValueError:
                continue
            if not reg and not typ and not desc:
                skipped += 1
                continue
            if len(reg) > REG_LEN or len(typ) > TYPE_LEN:
                # Truncating a registration would produce a wrong one, which is
                # worse than not having it. Long ones are rare enough to drop.
                skipped += 1
                continue
            try:
                yr = int(year)
                if not 1900 <= yr <= 2100:
                    yr = 0
            except ValueError:
                yr = 0
            kept += 1
            yield icao, reg, typ, desc[:MAX_TEXT], yr, oper[:MAX_TEXT]
    print(f'  kept {kept}, skipped {skipped} (empty or over-long)')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--blocks', default='C8',
                    help='comma-separated leading ICAO hex byte(s) to include, '
                         'e.g. "C8" for New Zealand, "C8,7C" to add Australia, '
                         'or "all" (default: C8)')
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--no-operators', action='store_true',
                    help='drop the owner/operator column. Mostly private owners '
                         'rather than airlines, and the bulk of the string pool, '
                         'so this roughly halves the file')
    args = ap.parse_args()

    blocks = None if args.blocks.lower() == 'all' else {
        b.strip().upper() for b in args.blocks.split(',') if b.strip()}

    print('Aircraft database')
    src = fetch(DB_URL, 'aircraft.csv.gz')
    print(f'  blocks: {"all" if blocks is None else ",".join(sorted(blocks))}')

    rows = sorted(records(src, blocks, not args.no_operators), key=lambda r: r[0])
    if not rows:
        sys.exit('no records matched -- check --blocks')

    # The firmware binary-searches, so a duplicate key would make which record
    # it finds depend on the search path. Last write wins, as the CSV intends.
    deduped = {}
    for icao, reg, typ, desc, yr, oper in rows:
        deduped[icao] = (reg, typ, desc, yr, oper)
    rows = [(k,) + v for k, v in sorted(deduped.items())]

    # One entry per distinct string, so the thousand-odd aircraft that are each
    # a "CESSNA 172 Skyhawk" share a single copy of the words.
    strings, seen = [], {}

    def intern(text):
        if not text:
            return NO_STRING
        i = seen.get(text)
        if i is None:
            i = seen[text] = len(strings)
            strings.append(text)
        return i

    interned = [(icao, reg, typ, intern(desc), intern(oper), yr)
                for icao, reg, typ, desc, yr, oper in rows]

    if len(strings) >= NO_STRING:
        sys.exit(f'{len(strings)} distinct strings will not fit a 16-bit index '
                 f'-- narrow --blocks, or pass --no-operators')

    blob = bytearray()
    offsets = []
    for text in strings:
        offsets.append(len(blob))
        blob += text.encode('ascii', 'replace') + b'\0'

    strtab_off = HEADER_LEN + len(interned) * RECORD_LEN
    pool_off = strtab_off + len(strings) * 4
    size = pool_off + len(blob)

    os.makedirs(os.path.dirname(args.out) or '.', exist_ok=True)
    with open(args.out, 'wb') as f:
        header = (MAGIC
                  + struct.pack('<HH', VERSION, RECORD_LEN)
                  + struct.pack('<I', len(interned))
                  + struct.pack('<BBH', REG_LEN, TYPE_LEN, 0)
                  + struct.pack('<II', len(strings), strtab_off)
                  + struct.pack('<I', pool_off))
        f.write(header.ljust(HEADER_LEN, b'\0'))
        for icao, reg, typ, d, o, yr in interned:
            f.write(bytes(((icao >> 16) & 0xFF, (icao >> 8) & 0xFF, icao & 0xFF)))
            f.write(reg.encode('ascii', 'replace').ljust(REG_LEN, b'\0'))
            f.write(typ.encode('ascii', 'replace').ljust(TYPE_LEN, b'\0'))
            f.write(struct.pack('<HHH', d, o, yr))
            f.write(b'\0' * 3)
        for off in offsets:
            f.write(struct.pack('<I', off))
        f.write(blob)

    named = sum(1 for r in interned if r[3] != NO_STRING)
    print(f'\n  {len(interned)} records, {named} with a description, '
          f'{len(strings)} distinct strings ({len(blob)/1024:.0f} KB pooled)')
    print(f'  -> {args.out} ({size/1024:.0f} KB)')
    if size > SPIFFS_BUDGET:
        print(f'  {size/1e6:.1f} MB is too big for SPIFFS or PSRAM -- copy it '
              f'to the SD card as /aircraftdb.bin (see tools/pack_sd.py)')
    else:
        print(f'  {100.0*size/SPIFFS_BUDGET:.1f}% of the SPIFFS budget; '
              f'flash it with: pio run -t uploadfs')


if __name__ == '__main__':
    main()
