#!/usr/bin/env python3
"""Build the T-Deck Pro basemap blob.

Turns public-domain geodata into data/map.bin, which `pio run -t uploadfs`
flashes into the SPIFFS partition. The firmware memory-maps one detail level at
a time into PSRAM and draws it under the radar plot.

Sources, all fetched on demand and cached in tools/.mapcache:

  coastline  Natural Earth 1:10m  (public domain)
  airports   OurAirports          (public domain)
  airspace   NOT downloadable -- openAIP needs an API key and soaringweb blocks
             scripted access. Pass --airspace <file.txt> with an OpenAir file
             you are licensed to use. Skipped when absent. For New Zealand,
             XCSoar's public repository indexes one at
             https://gliding.co.nz/wp-content/uploads/2023/01/2022_nz_airspace_v3.txt

Only the Python standard library is used, so this runs anywhere the toolchain
does.

    python tools/build_map.py                      # New Zealand, default
    python tools/build_map.py --airspace nz.txt
    python tools/build_map.py --bbox -48 165 -33 180
"""

import argparse
import csv
import io
import json
import math
import os
import struct
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CACHE = os.path.join(HERE, '.mapcache')
OUT = os.path.join(ROOT, 'data', 'map.bin')

COASTLINE_URL = ('https://raw.githubusercontent.com/nvkelso/natural-earth-vector/'
                 'master/geojson/ne_10m_coastline.geojson')
AIRPORTS_URL = 'https://davidmegginson.github.io/ourairports-data/airports.csv'
RUNWAYS_URL = 'https://davidmegginson.github.io/ourairports-data/runways.csv'

MAGIC = b'TDECKMAP'
VERSION = 1

KIND_COAST, KIND_AIRSPACE, KIND_RUNWAY, KIND_AIRPORT = 0, 1, 2, 3

# One entry per detail level, coarsest last. `max_range_nm` is the largest
# kRangeStepsNm value this level serves; `eps_nm` is the Douglas-Peucker
# tolerance, set to roughly one screen pixel at that range (RADAR_R is 110 px,
# so px_per_nm = 110 / range).
LEVELS = [
    dict(max_range_nm=20,  eps_nm=0.15, airport_types=('large_airport', 'medium_airport', 'small_airport'), runways=True),
    dict(max_range_nm=100, eps_nm=0.70, airport_types=('large_airport', 'medium_airport'), runways=True),
    dict(max_range_nm=250, eps_nm=2.00, airport_types=('large_airport',), runways=False),
]


# ----------------------------------------------------------------- fetching --
def fetch(url, name):
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, name)
    if os.path.exists(path) and os.path.getsize(path) > 0:
        print(f'  cached  {name} ({os.path.getsize(path)/1e6:.1f} MB)')
        return path
    print(f'  fetching {name} ...', end='', flush=True)
    req = urllib.request.Request(url, headers={'User-Agent': 'tdeckpro-adsb-mapbuild'})
    with urllib.request.urlopen(req, timeout=120) as r, open(path, 'wb') as f:
        f.write(r.read())
    print(f' {os.path.getsize(path)/1e6:.1f} MB')
    return path


# ---------------------------------------------------------------- geometry ---
def clip_runs(points, bbox):
    """Split a polyline into the runs that touch bbox.

    Segments are kept when either end is inside, so a coastline entering and
    leaving the box keeps its crossing segment. Runs are emitted separately so
    two distant pieces are never joined by a line straight across the view.
    """
    minlat, minlon, maxlat, maxlon = bbox

    def inside(p):
        return minlat <= p[0] <= maxlat and minlon <= p[1] <= maxlon

    runs, cur = [], []
    for a, b in zip(points, points[1:]):
        if inside(a) or inside(b):
            if not cur:
                cur = [a]
            cur.append(b)
        elif cur:
            runs.append(cur)
            cur = []
    if cur:
        runs.append(cur)
    return [r for r in runs if len(r) >= 2]


def simplify(points, eps_deg, lon_scale):
    """Douglas-Peucker, iterative so a long coastline cannot blow the stack.

    Longitude is scaled by cos(latitude) so the tolerance is isotropic on the
    ground rather than in raw degrees.
    """
    if len(points) < 3:
        return points

    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]

    while stack:
        lo, hi = stack.pop()
        if hi <= lo + 1:
            continue
        ay, ax = points[lo][0], points[lo][1] * lon_scale
        by, bx = points[hi][0], points[hi][1] * lon_scale
        dy, dx = by - ay, bx - ax
        norm = math.hypot(dx, dy)

        worst, worst_i = -1.0, -1
        for i in range(lo + 1, hi):
            py, px = points[i][0], points[i][1] * lon_scale
            if norm == 0.0:
                d = math.hypot(px - ax, py - ay)
            else:
                d = abs(dx * (ay - py) - (ax - px) * dy) / norm
            if d > worst:
                worst, worst_i = d, i

        if worst > eps_deg:
            keep[worst_i] = True
            stack.append((lo, worst_i))
            stack.append((worst_i, hi))

    return [p for p, k in zip(points, keep) if k]


# ------------------------------------------------------------------ layers ---
def load_coastline(bbox):
    path = fetch(COASTLINE_URL, 'ne_10m_coastline.geojson')
    with open(path, encoding='utf-8') as f:
        gj = json.load(f)

    runs = []
    for feat in gj['features']:
        geom = feat.get('geometry') or {}
        if geom.get('type') == 'LineString':
            parts = [geom['coordinates']]
        elif geom.get('type') == 'MultiLineString':
            parts = geom['coordinates']
        else:
            continue
        for coords in parts:
            # GeoJSON is (lon, lat); everything downstream is (lat, lon).
            pts = [(c[1], c[0]) for c in coords]
            runs.extend(clip_runs(pts, bbox))
    print(f'  coastline: {len(runs)} runs, {sum(len(r) for r in runs)} points in bbox')
    return runs


def load_airports(bbox):
    apath = fetch(AIRPORTS_URL, 'airports.csv')
    rpath = fetch(RUNWAYS_URL, 'runways.csv')
    minlat, minlon, maxlat, maxlon = bbox

    airports = {}
    with open(apath, encoding='utf-8', newline='') as f:
        for row in csv.DictReader(f):
            try:
                lat, lon = float(row['latitude_deg']), float(row['longitude_deg'])
            except (ValueError, KeyError):
                continue
            if not (minlat <= lat <= maxlat and minlon <= lon <= maxlon):
                continue
            if row['type'] not in ('large_airport', 'medium_airport', 'small_airport'):
                continue
            airports[row['id']] = dict(
                ident=row['ident'], type=row['type'], lat=lat, lon=lon, runways=[])

    with open(rpath, encoding='utf-8', newline='') as f:
        for row in csv.DictReader(f):
            ap = airports.get(row['airport_ref'])
            if ap is None or row.get('closed') == '1':
                continue
            try:
                le = (float(row['le_latitude_deg']), float(row['le_longitude_deg']))
                he = (float(row['he_latitude_deg']), float(row['he_longitude_deg']))
            except (ValueError, KeyError):
                continue
            ap['runways'].append((le, he))

    n_rwy = sum(len(a['runways']) for a in airports.values())
    print(f'  airports:  {len(airports)} in bbox, {n_rwy} runway centrelines')
    return list(airports.values())


def dest(clat, clon, bearing_deg, dist_nm):
    """Point `dist_nm` from (clat, clon) on a true bearing, flat-earth."""
    b = math.radians(bearing_deg)
    coslat = math.cos(math.radians(clat))
    dlon = (dist_nm * math.sin(b) / (60.0 * coslat)) if abs(coslat) > 1e-9 else 0.0
    return (clat + dist_nm * math.cos(b) / 60.0, clon + dlon)


def bearing_dist(clat, clon, lat, lon):
    north = (lat - clat) * 60.0
    east = (lon - clon) * 60.0 * math.cos(math.radians(clat))
    return (math.degrees(math.atan2(east, north)) % 360.0, math.hypot(east, north))


def arc_points(centre, radius_nm, start_deg, sweep_deg):
    """Tessellate an arc so the chord sags less than ~0.05 nm off the true curve.

    That is well under a pixel at every range the display offers, and it keeps
    small circles from being rendered as visible polygons.
    """
    if radius_nm <= 0.0:
        return []
    ratio = max(-1.0, min(1.0, 1.0 - 0.05 / radius_nm))
    step = math.degrees(2.0 * math.acos(ratio)) if radius_nm > 0.05 else 45.0
    n = int(abs(sweep_deg) / max(step, 1.0)) + 1
    n = max(4, min(n, 180))
    return [dest(centre[0], centre[1], start_deg + sweep_deg * i / n, radius_nm)
            for i in range(n + 1)]


def parse_openair_coord(text):
    """e.g. "36:52:00 S 174:46:00 E" or "36:52.0 S 174:46.0 E"."""
    parts = text.replace(',', ' ').split()
    if len(parts) < 4:
        return None
    try:
        vals = []
        for token, hemi in ((parts[0], parts[1]), (parts[2], parts[3])):
            bits = [float(b) for b in token.split(':')]
            deg = bits[0] + (bits[1] / 60 if len(bits) > 1 else 0) +                   (bits[2] / 3600 if len(bits) > 2 else 0)
            if hemi.upper() in ('S', 'W'):
                deg = -deg
            vals.append(deg)
        return (vals[0], vals[1])
    except (ValueError, IndexError):
        return None


def load_airspace(path, bbox, classes):
    """Parse OpenAir into closed rings.

    Handles the geometry records that actually appear in real files: DP points,
    DC circles, and DA/DB arcs about the V X= centre honouring V D= direction.
    Circles matter especially -- an airspace defined only by V X= plus DC has no
    DP records at all, so a DP-only parser drops it silently rather than merely
    approximating it.
    """
    out = []
    name = cls = ''
    pts = []
    centre = None
    clockwise = True
    skipped = 0

    def flush():
        nonlocal skipped
        if not pts:
            return
        if classes and cls.upper() not in classes:
            skipped += 1
            return
        if len(pts) < 3:
            return
        ring = pts + [pts[0]]
        for run in clip_runs(ring, bbox):
            out.append(dict(name=f'{cls} {name}'.strip()[:20], points=run))

    with open(path, encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith('*'):
                continue
            tag, _, rest = line.partition(' ')
            tag, rest = tag.upper(), rest.strip()

            if tag == 'AC':
                flush()
                cls, name, pts = rest, '', []
                centre, clockwise = None, True
            elif tag == 'AN':
                name = rest
            elif tag == 'V':
                key, _, val = rest.partition('=')
                key, val = key.strip().upper(), val.strip()
                if key == 'X':
                    centre = parse_openair_coord(val)
                elif key == 'D':
                    clockwise = not val.startswith('-')
            elif tag == 'DP':
                pt = parse_openair_coord(rest)
                if pt:
                    pts.append(pt)
            elif tag == 'DC' and centre:
                try:
                    radius = float(rest.split()[0])
                except (ValueError, IndexError):
                    continue
                # A circle replaces the ring outright rather than joining any
                # stray points before it.
                pts = arc_points(centre, radius, 0.0, 360.0)
            elif tag == 'DB' and centre:
                ends = [parse_openair_coord(t) for t in rest.split(',')]
                if len(ends) != 2 or not all(ends):
                    continue
                b1, r1 = bearing_dist(centre[0], centre[1], *ends[0])
                b2, r2 = bearing_dist(centre[0], centre[1], *ends[1])
                sweep = (b2 - b1) % 360.0
                if not clockwise:
                    sweep -= 360.0
                pts.extend(arc_points(centre, (r1 + r2) / 2.0, b1, sweep))
            elif tag == 'DA' and centre:
                try:
                    radius, a1, a2 = [float(t) for t in rest.split(',')[:3]]
                except ValueError:
                    continue
                sweep = (a2 - a1) % 360.0
                if not clockwise:
                    sweep -= 360.0
                pts.extend(arc_points(centre, radius, a1, sweep))
    flush()

    kept = len({r['name'] for r in out})
    print(f'  airspace:  {len(out)} boundary runs from {kept} areas '
          f'({skipped} filtered out by class) in {os.path.basename(path)}')
    return out


# ------------------------------------------------------------------ writing --
def e7(v):
    return int(round(v * 1e7))


def pack_feature(kind, name, points):
    """int32 bbox, then counts, then 4-byte-aligned coords, then padded name.

    Laid out so every int32 the firmware reads is 4-byte aligned -- Xtensa
    traps unaligned 32-bit loads, and taking that on a per-vertex basis would
    be ruinous.
    """
    lats = [p[0] for p in points]
    lons = [p[1] for p in points]
    nb = name.encode('ascii', 'ignore')[:31]
    pad = (-len(nb)) % 4

    return (struct.pack('<4i', e7(min(lats)), e7(min(lons)), e7(max(lats)), e7(max(lons)))
            + struct.pack('<HBB', len(points), kind, len(nb))
            + b''.join(struct.pack('<ii', e7(p[0]), e7(p[1])) for p in points)
            + nb + b'\0' * pad)


def build_level(spec, coast, airports, airspace, lon_scale):
    eps_deg = spec['eps_nm'] / 60.0
    feats = []

    for run in coast:
        simp = simplify(run, eps_deg, lon_scale)
        if len(simp) >= 2:
            feats.append(pack_feature(KIND_COAST, '', simp))

    for run in airspace:
        simp = simplify(run['points'], eps_deg, lon_scale)
        if len(simp) >= 2:
            feats.append(pack_feature(KIND_AIRSPACE, run['name'], simp))

    for ap in airports:
        if ap['type'] not in spec['airport_types']:
            continue
        feats.append(pack_feature(KIND_AIRPORT, ap['ident'], [(ap['lat'], ap['lon'])]))
        if spec['runways']:
            for le, he in ap['runways']:
                feats.append(pack_feature(KIND_RUNWAY, '', [le, he]))

    return b''.join(feats), len(feats)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bbox', nargs=4, type=float, metavar=('MINLAT', 'MINLON', 'MAXLAT', 'MAXLON'),
                    default=[-48.0, 165.0, -33.0, 180.0],
                    help='region to include (default: New Zealand)')
    ap.add_argument('--airspace', metavar='OPENAIR_FILE',
                    help='OpenAir airspace file; omitted = no airspace layer')
    ap.add_argument('--airspace-classes', default='CTR,P,R',
                    help='OpenAir classes to keep, comma separated, or "all". '
                         'Default CTR,P,R -- control zones plus prohibited and '
                         'restricted. Adding C,D blankets the plot with terminal '
                         'areas and buries the traffic; preview with '
                         'tools/verify_map.py --centre before committing to it.')
    ap.add_argument('--out', default=OUT)
    args = ap.parse_args()

    bbox = tuple(args.bbox)
    lon_scale = math.cos(math.radians((bbox[0] + bbox[2]) / 2))
    print(f'region: lat {bbox[0]}..{bbox[2]}, lon {bbox[1]}..{bbox[3]}')

    coast = load_coastline(bbox)
    airports = load_airports(bbox)
    if args.airspace:
        classes = set() if args.airspace_classes.lower() == 'all' else {
            c.strip().upper() for c in args.airspace_classes.split(',') if c.strip()}
        airspace = load_airspace(args.airspace, bbox, classes)
    else:
        airspace = []
        print('  airspace:  skipped (no --airspace file given)')

    header_len = 32 + 12 * len(LEVELS)
    blobs, counts = [], []
    for i, spec in enumerate(LEVELS):
        blob, n = build_level(spec, coast, airports, airspace, lon_scale)
        blobs.append(blob)
        counts.append(n)
        print(f'  level {i} (<= {spec["max_range_nm"]:3d} nm, eps {spec["eps_nm"]:.2f} nm): '
              f'{n:5d} features, {len(blob)/1024:7.1f} KB')

    out = io.BytesIO()
    out.write(MAGIC)
    out.write(struct.pack('<HH', VERSION, len(LEVELS)))
    out.write(struct.pack('<4i', e7(bbox[0]), e7(bbox[1]), e7(bbox[2]), e7(bbox[3])))
    out.write(struct.pack('<I', 0))  # reserved
    offset = header_len
    for blob, n, spec in zip(blobs, counts, LEVELS):
        out.write(struct.pack('<IIHH', offset, len(blob), n, spec['max_range_nm']))
        offset += len(blob)
    assert out.tell() == header_len, (out.tell(), header_len)
    for blob in blobs:
        out.write(blob)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'wb') as f:
        f.write(out.getvalue())
    total = out.tell()
    print(f'\nwrote {args.out}  ({total/1024:.1f} KB, {total/3538944*100:.1f}% of SPIFFS)')
    print('flash it with:  pio run -t uploadfs')


if __name__ == '__main__':
    sys.exit(main())
