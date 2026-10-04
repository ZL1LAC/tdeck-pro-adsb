#!/usr/bin/env python3
"""Read data/map.bin back with an independent parser and plot it as ASCII.

Cross-checks the writer: if latitude and longitude were ever swapped, or the
4-byte alignment padding is wrong, it shows up here as garbage instead of a
recognisable coastline.

    python tools/verify_map.py [--level N]
"""
import argparse
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KINDS = {0: 'coast', 1: 'airspace', 2: 'runway', 3: 'airport'}
GLYPH = {0: '#', 1: ':', 2: '=', 3: 'o'}
INT16_MIN, INT16_MAX = -32768, 32767
KIND_AIRSPACE = 1


def read_blob(path):
    data = open(path, 'rb').read()
    magic, version, levels = struct.unpack_from('<8sHH', data, 0)
    if magic != b'TDECKMAP':
        sys.exit(f'bad magic {magic!r}')
    bbox = [v / 1e7 for v in struct.unpack_from('<4i', data, 12)]
    dirs = []
    for i in range(levels):
        off, length, count, maxr = struct.unpack_from('<IIHH', data, 32 + 12 * i)
        dirs.append(dict(offset=off, length=length, count=count, max_range_nm=maxr))
    return data, version, bbox, dirs


def read_features(data, d, version):
    pos, end, feats = d['offset'], d['offset'] + d['length'], []
    hdr = 24 if version >= 2 else 20
    while pos < end:
        mnla, mnlo, mxla, mxlo = struct.unpack_from('<4i', data, pos)
        npts, kind, namelen = struct.unpack_from('<HBB', data, pos + 16)
        if version >= 2:
            floor_ft, ceil_ft = struct.unpack_from('<hh', data, pos + 20)
            cpos = pos + 24
        else:
            floor_ft, ceil_ft = INT16_MIN, INT16_MAX
            cpos = pos + 20
        pts = [(la / 1e7, lo / 1e7) for la, lo in
               struct.iter_unpack('<ii', data[cpos:cpos + 8 * npts])]
        npos = cpos + 8 * npts
        name = data[npos:npos + namelen].decode('ascii', 'replace')
        pos = npos + namelen + ((-namelen) % 4)
        if pos % 4:
            sys.exit(f'feature at {pos} lost 4-byte alignment')
        feats.append(dict(kind=kind, name=name, points=pts,
                          floor_ft=floor_ft, ceil_ft=ceil_ft,
                          bbox=(mnla / 1e7, mnlo / 1e7, mxla / 1e7, mxlo / 1e7)))
    if pos != end:
        sys.exit(f'level overran its length: {pos} != {end}')
    return feats


def airspace_visible(floor_ft, ceil_ft, traffic='all'):
    """Match filter::airspaceVisible() for the named traffic preset."""
    if traffic == 'low':
        max_floor = 10000
        min_ceil = 0
    elif traffic == 'high':
        max_floor = INT16_MAX
        min_ceil = 10000
    else:
        max_floor = 19500
        min_ceil = 0
    if max_floor != INT16_MAX and floor_ft != INT16_MIN and floor_ft >= max_floor:
        return False
    if min_ceil > 0 and ceil_ft not in (INT16_MAX, INT16_MIN) and ceil_ft < min_ceil:
        return False
    return True


def plot(feats, bbox, w=78, h=40):
    minlat, minlon, maxlat, maxlon = bbox
    grid = [[' '] * w for _ in range(h)]

    def put(lat, lon, ch):
        # North up: latitude increases upward, so row 0 is maxlat.
        x = int((lon - minlon) / (maxlon - minlon) * (w - 1))
        y = int((maxlat - lat) / (maxlat - minlat) * (h - 1))
        if 0 <= x < w and 0 <= y < h:
            if grid[y][x] in (' ', '#', ':') or ch in ('o', '='):
                grid[y][x] = ch

    for f in feats:
        ch = GLYPH.get(f['kind'], '?')
        pts = f['points']
        if len(pts) == 1:
            put(pts[0][0], pts[0][1], ch)
            continue
        for (la1, lo1), (la2, lo2) in zip(pts, pts[1:]):
            steps = max(2, int(max(abs(la2 - la1), abs(lo2 - lo1)) * 60))
            for i in range(steps + 1):
                t = i / steps
                put(la1 + (la2 - la1) * t, lo1 + (lo2 - lo1) * t, ch)

    print('+' + '-' * w + '+')
    for row in grid:
        print('|' + ''.join(row) + '|')
    print('+' + '-' * w + '+')



def scope(feats, clat, clon, range_nm, w=92, rows=34):
    """Render what the device would actually plot: a circle of `range_nm`
    around (clat, clon), using the same flat projection as geo::projectNm.

    This is the only preview available for a panel that cannot be screenshotted,
    so it is worth keeping honest: same projection, same circular clip, same
    level selection as basemap.cpp.
    """
    import math
    cx, cy = w // 2, rows // 2
    rx, ry = cx - 1, cy - 1          # chars are about twice as tall as wide
    coslat = math.cos(math.radians(clat))
    grid = [[' '] * w for _ in range(rows)]

    def cell(lat, lon):
        north = (lat - clat) * 60.0
        east = (lon - clon) * 60.0 * coslat
        return (cx + east / range_nm * rx, cy - north / range_nm * ry)

    def put(fx, fy, ch):
        x, y = int(round(fx)), int(round(fy))
        if not (0 <= x < w and 0 <= y < rows):
            return
        # Inside the plot circle only, matching clipToCircle().
        nx, ny = (x - cx) / rx, (y - cy) / ry
        if nx * nx + ny * ny > 1.0:
            return
        if ch in ('o', '=') or grid[y][x] == ' ':
            grid[y][x] = ch

    for f in feats:
        ch = GLYPH.get(f['kind'], '?')
        pts = f['points']
        if len(pts) == 1:
            put(*cell(*pts[0]), ch)
            continue
        for a, b in zip(pts, pts[1:]):
            x0, y0 = cell(*a)
            x1, y1 = cell(*b)
            steps = max(2, int(max(abs(x1 - x0), abs(y1 - y0))))
            if steps > 4000:
                continue
            for i in range(steps + 1):
                t = i / steps
                put(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, ch)

    # Own position, drawn like the firmware's ringed crosshair.
    for dx in (-1, 0, 1):
        if grid[cy][cx + dx] == ' ':
            grid[cy][cx + dx] = '-'
    grid[cy][cx] = '+'

    print('+' + '-' * w + '+')
    for row in grid:
        print('|' + ''.join(row) + '|')
    print('+' + '-' * w + '+')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--level', type=int, default=None)
    ap.add_argument('--file', default=os.path.join(ROOT, 'data', 'map.bin'))
    ap.add_argument('--centre', nargs=2, type=float, metavar=('LAT', 'LON'),
                    help='render the radar view the device would draw here')
    ap.add_argument('--range', type=float, default=40.0,
                    help='plotted range in nm for --centre (default 40)')
    ap.add_argument('--traffic', choices=('all', 'air', 'low', 'high'),
                    default='all',
                    help='airspace altitude skip, matching the device filter')
    args = ap.parse_args()

    data, version, bbox, dirs = read_blob(args.file)
    print(f'{args.file}: v{version}, {len(dirs)} levels, {len(data)/1024:.1f} KB')
    print(f'bbox lat {bbox[0]}..{bbox[2]}  lon {bbox[1]}..{bbox[3]}\n')

    for i, d in enumerate(dirs):
        feats = read_features(data, d, version)
        assert len(feats) == d['count'], (len(feats), d['count'])
        tally = {}
        for f in feats:
            tally[KINDS[f['kind']]] = tally.get(KINDS[f['kind']], 0) + 1
        pts = sum(len(f['points']) for f in feats)
        print(f'level {i}: <= {d["max_range_nm"]:3d} nm  {d["count"]:5d} features  '
              f'{pts:6d} points  {d["length"]/1024:7.1f} KB  {tally}')

    if args.centre:
        # Same rule as basemap.cpp levelFor().
        show = next((i for i, d in enumerate(dirs)
                     if args.range <= d['max_range_nm']), len(dirs) - 1)
        feats = read_features(data, dirs[show], version)
        skipped = 0
        kept = []
        for f in feats:
            if f['kind'] == KIND_AIRSPACE and not airspace_visible(
                    f['floor_ft'], f['ceil_ft'], args.traffic):
                skipped += 1
                continue
            kept.append(f)
        print(f'\nradar view: {args.range:.0f} nm around '
              f'{args.centre[0]:.4f}, {args.centre[1]:.4f}  -> level {show}')
        print(f'airspace altitude skip ({args.traffic}): {skipped} runs hidden')
        print('(# coast, o airport, = runway, : airspace, + own position)')
        scope(kept, args.centre[0], args.centre[1], args.range)
        return

    show = args.level if args.level is not None else len(dirs) - 1
    print(f'\nlevel {show}  (# coast, o airport, = runway, : airspace)')
    plot(read_features(data, dirs[show], version), bbox)


if __name__ == '__main__':
    main()
