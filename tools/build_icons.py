#!/usr/bin/env python3
"""Build the T-Deck Pro aircraft silhouettes.

Turns tar1090's vector aircraft markers into data/icons.bin, which
`pio run -t uploadfs` flashes into SPIFFS. The firmware looks a silhouette up
by ICAO type designator and draws it on the detail page, so a selected target
shows what it looks like from above and not just what it is called.

Everything is done here rather than on the device: the SVG paths are parsed,
their curves flattened to polygons, and the result scan-converted to the 1-bit
bitmaps Adafruit_GFX::drawBitmap() wants. The firmware does no geometry at all.

Sources, both fetched on demand and cached in tools/.iconcache:

  shapes    wiedehopf/tar1090 html/markers.js -- 85 plan-view aircraft, plus
            the TypeDesignatorIcons and TypeDescriptionIcons tables that say
            which type designator uses which shape.
  classes   rikgale/ICAOList ICAOList.csv -- ICAO Doc 8643, giving the class
            and engine fitment of ~2,700 designators. This is what turns the
            368 exact entries into near-complete coverage: a C172 has no shape
            of its own, but Doc 8643 calls it a single-engine piston landplane
            and tar1090 has a shape for that.

  NOTE ON LICENCE: tar1090's LICENSE names an author and a dump1090 ancestry
  without naming a licence, and GitHub reads it as NOASSERTION. That is fine
  for a device you build for yourself and unresolved for anything you publish.
  The artwork is not committed to this repository for that reason -- this
  script fetches it, and data/icons.bin is gitignored with the other blobs.

Only the Python standard library is used, so this runs wherever the toolchain
does.

    python tools/build_icons.py
    python tools/build_icons.py --width 72 --height 88
"""

import argparse
import csv
import io
import math
import os
import re
import struct
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CACHE = os.path.join(HERE, '.iconcache')
OUT = os.path.join(ROOT, 'data', 'icons.bin')

MARKERS_URL = 'https://raw.githubusercontent.com/wiedehopf/tar1090/master/html/markers.js'
ICAOLIST_URL = 'https://raw.githubusercontent.com/rikgale/ICAOList/main/ICAOList.csv'

MAGIC = b'TDECKICO'
VERSION = 1
HEADER_LEN = 32
DESIGNATOR_LEN = 4

# Curves are flattened to this many segments each. They are being rasterised
# into something under a hundred pixels wide, so this is already far finer than
# the output can express.
CURVE_STEPS = 16

# The rasteriser samples this many times per axis per pixel. Wings and tailplanes
# come out one or two pixels thick at this size, and point sampling drops them
# entirely; coverage plus a low threshold keeps them.
SUPERSAMPLE = 4
COVERAGE_THRESHOLD = 0.35

# ICAO Doc 8643 class and engine letters, assembled into the description code
# ('L2J', 'H1T') that TypeDescriptionIcons is keyed on.
CLASS_LETTER = {
    'landplane': 'L', 'seaplane': 'S', 'amphibian': 'A', 'helicopter': 'H',
    'gyrocopter': 'G', 'tiltrotor': 'T', 'tilt-rotor': 'T', 'tiltwing': 'T',
}
ENGINE_LETTER = {
    'piston': 'P', 'jet': 'J', 'turboprop': 'T', 'turboshaft': 'T',
    'turboprop/turboshaft': 'T', 'electric': 'E', 'rocket': 'J',
}

# TypeDescriptionIcons distinguishes twin jets only once a wake category is
# known -- it has L2J-L, L2J-M and L2J-H but no bare L2J -- and Doc 8643 does
# not carry one. Every twin jet therefore falls back to the generic airliner
# unless it is named here, which would draw a 787 as a narrowbody. These are
# the widebodies common enough to be worth correcting by hand; the ones already
# in tar1090's own table (A332, A359, A380, B77W and so on) are not repeated.
WIDEBODY = {
    'B762', 'B763', 'B764', 'B772', 'B773', 'B77L', 'B778', 'B779',
    'B788', 'B789', 'B78X', 'A30B', 'A310', 'A337', 'A342', 'A343',
    'A345', 'A346', 'A35K', 'IL86', 'IL96', 'A124', 'C5M',
}


def fetch(url, name):
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, name)
    if os.path.exists(path) and os.path.getsize(path) > 0:
        print(f'  cached  {name} ({os.path.getsize(path)/1024:.0f} KB)')
        return path
    print(f'  fetching {name} ...', end='', flush=True)
    req = urllib.request.Request(url, headers={'User-Agent': 'tdeckpro-adsb-iconbuild'})
    with urllib.request.urlopen(req, timeout=180) as r, open(path, 'wb') as f:
        f.write(r.read())
    print(f' {os.path.getsize(path)/1024:.0f} KB')
    return path


# ------------------------------------------------------------ svg paths ------
NUMBER = re.compile(r'[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?')
SEPARATORS = ',' + ' \t\r\n'


class Scanner:
    """Positional reader for SVG path data.

    Positional rather than a token list because of the arc flags: in "a1.5 1.5
    0 011.5 0" the large-arc and sweep flags are the bare characters "0" and
    "1", and the 1.5 that follows them is a coordinate. Tokenising by number
    would read "011.5" as one value and desynchronise everything after it.
    """

    def __init__(self, d):
        self.d = d
        self.i = 0

    def _skip(self):
        while self.i < len(self.d) and self.d[self.i] in SEPARATORS:
            self.i += 1

    def command(self):
        self._skip()
        if self.i < len(self.d) and self.d[self.i].isalpha():
            c = self.d[self.i]
            self.i += 1
            return c
        return None

    def more_numbers(self):
        self._skip()
        return self.i < len(self.d) and not self.d[self.i].isalpha()

    def number(self):
        self._skip()
        m = NUMBER.match(self.d, self.i)
        if not m or m.group() in ('', '+', '-', '.'):
            raise ValueError(f'expected a number at offset {self.i}')
        self.i = m.end()
        return float(m.group())

    def flag(self):
        self._skip()
        c = self.d[self.i]
        self.i += 1
        return c == '1'


def bezier3(p0, p1, p2, p3, steps):
    for i in range(1, steps + 1):
        t = i / steps
        u = 1.0 - t
        yield (u*u*u*p0[0] + 3*u*u*t*p1[0] + 3*u*t*t*p2[0] + t*t*t*p3[0],
               u*u*u*p0[1] + 3*u*u*t*p1[1] + 3*u*t*t*p2[1] + t*t*t*p3[1])


def bezier2(p0, p1, p2, steps):
    for i in range(1, steps + 1):
        t = i / steps
        u = 1.0 - t
        yield (u*u*p0[0] + 2*u*t*p1[0] + t*t*p2[0],
               u*u*p0[1] + 2*u*t*p1[1] + t*t*p2[1])


def arc(p0, rx, ry, rot, large, sweep, p1, steps):
    """Endpoint to centre parameterisation, SVG 1.1 appendix F.6.5."""
    if rx == 0 or ry == 0 or p0 == p1:
        yield p1
        return
    rx, ry = abs(rx), abs(ry)
    phi = math.radians(rot)
    cos_p, sin_p = math.cos(phi), math.sin(phi)

    dx2, dy2 = (p0[0] - p1[0]) / 2.0, (p0[1] - p1[1]) / 2.0
    x1 = cos_p * dx2 + sin_p * dy2
    y1 = -sin_p * dx2 + cos_p * dy2

    # Scale the radii up if they are too small to span the endpoints.
    lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry)
    if lam > 1:
        s = math.sqrt(lam)
        rx, ry = rx * s, ry * s

    num = rx*rx*ry*ry - rx*rx*y1*y1 - ry*ry*x1*x1
    den = rx*rx*y1*y1 + ry*ry*x1*x1
    coef = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        coef = -coef
    cx1 = coef * rx * y1 / ry
    cy1 = -coef * ry * x1 / rx

    cx = cos_p * cx1 - sin_p * cy1 + (p0[0] + p1[0]) / 2.0
    cy = sin_p * cx1 + cos_p * cy1 + (p0[1] + p1[1]) / 2.0

    def angle(ux, uy, vx, vy):
        dot = ux * vx + uy * vy
        n = math.hypot(ux, uy) * math.hypot(vx, vy)
        if n == 0:
            return 0.0
        a = math.acos(max(-1.0, min(1.0, dot / n)))
        return -a if ux * vy - uy * vx < 0 else a

    theta = angle(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry)
    delta = angle((x1 - cx1) / rx, (y1 - cy1) / ry,
                  (-x1 - cx1) / rx, (-y1 - cy1) / ry)
    if not sweep and delta > 0:
        delta -= 2 * math.pi
    elif sweep and delta < 0:
        delta += 2 * math.pi

    for i in range(1, steps + 1):
        t = theta + delta * i / steps
        ct, st = math.cos(t), math.sin(t)
        yield (cos_p * rx * ct - sin_p * ry * st + cx,
               sin_p * rx * ct + cos_p * ry * st + cy)


def flatten(d):
    """SVG path data -> list of closed polygons, as lists of (x, y)."""
    sc = Scanner(d)
    polys, cur = [], []
    x = y = 0.0
    start = (0.0, 0.0)
    prev_c = prev_q = None   # last control point, for S/s and T/t
    cmd = None

    def close():
        nonlocal cur
        if len(cur) > 2:
            polys.append(cur)
        cur = []

    while True:
        c = sc.command()
        if c is not None:
            cmd = c
        elif cmd is None or not sc.more_numbers():
            break
        elif cmd in 'Mm':
            # Coordinates repeated after a moveto are implicit linetos.
            cmd = 'L' if cmd == 'M' else 'l'

        rel = cmd.islower()
        u = cmd.upper()

        if u == 'Z':
            close()
            x, y = start
            prev_c = prev_q = None
            continue

        if not sc.more_numbers():
            break

        if u == 'M':
            dx, dy = sc.number(), sc.number()
            x, y = (x + dx, y + dy) if rel else (dx, dy)
            close()
            cur = [(x, y)]
            start = (x, y)
            prev_c = prev_q = None
        elif u == 'L':
            dx, dy = sc.number(), sc.number()
            x, y = (x + dx, y + dy) if rel else (dx, dy)
            cur.append((x, y))
            prev_c = prev_q = None
        elif u == 'H':
            dx = sc.number()
            x = x + dx if rel else dx
            cur.append((x, y))
            prev_c = prev_q = None
        elif u == 'V':
            dy = sc.number()
            y = y + dy if rel else dy
            cur.append((x, y))
            prev_c = prev_q = None
        elif u in 'CS':
            if u == 'C':
                x1, y1 = sc.number(), sc.number()
                p1 = (x + x1, y + y1) if rel else (x1, y1)
            else:
                p1 = (2*x - prev_c[0], 2*y - prev_c[1]) if prev_c else (x, y)
            x2, y2 = sc.number(), sc.number()
            dx, dy = sc.number(), sc.number()
            p2 = (x + x2, y + y2) if rel else (x2, y2)
            p3 = (x + dx, y + dy) if rel else (dx, dy)
            cur.extend(bezier3((x, y), p1, p2, p3, CURVE_STEPS))
            prev_c, prev_q = p2, None
            x, y = p3
        elif u in 'QT':
            if u == 'Q':
                x1, y1 = sc.number(), sc.number()
                p1 = (x + x1, y + y1) if rel else (x1, y1)
            else:
                p1 = (2*x - prev_q[0], 2*y - prev_q[1]) if prev_q else (x, y)
            dx, dy = sc.number(), sc.number()
            p2 = (x + dx, y + dy) if rel else (dx, dy)
            cur.extend(bezier2((x, y), p1, p2, CURVE_STEPS))
            prev_q, prev_c = p1, None
            x, y = p2
        elif u == 'A':
            rx, ry, rot = sc.number(), sc.number(), sc.number()
            large, sweep = sc.flag(), sc.flag()
            dx, dy = sc.number(), sc.number()
            p1 = (x + dx, y + dy) if rel else (dx, dy)
            cur.extend(arc((x, y), rx, ry, rot, large, sweep, p1, CURVE_STEPS))
            prev_c = prev_q = None
            x, y = p1
        else:
            break

    close()
    return polys


# ----------------------------------------------------------- rasterising -----
def rasterise(polys, target_w, target_h):
    """Scan-convert polygons to a 1-bit bitmap, fitted to the target box.

    Nonzero winding, matching SVG's default fill rule, so the inner outlines
    tar1090 uses for cockpit windows and engine detail cut holes rather than
    filling them in.
    """
    xs = [p[0] for poly in polys for p in poly]
    ys = [p[1] for poly in polys for p in poly]
    if not xs:
        return 0, 0, b''
    minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)
    span_x, span_y = max(maxx - minx, 1e-6), max(maxy - miny, 1e-6)

    # Fit the geometry's own bounds rather than the viewBox, which carries
    # padding that differs shape to shape and would render them inconsistently.
    scale = min(target_w / span_x, target_h / span_y)
    w = max(1, min(target_w, int(round(span_x * scale))))
    h = max(1, min(target_h, int(round(span_y * scale))))

    ss = SUPERSAMPLE
    edges = []
    for poly in polys:
        n = len(poly)
        for k in range(n):
            x0, y0 = poly[k]
            x1, y1 = poly[(k + 1) % n]
            if y0 == y1:
                continue
            edges.append(((x0 - minx) * scale, (y0 - miny) * scale,
                          (x1 - minx) * scale, (y1 - miny) * scale))

    counts = [[0] * w for _ in range(h)]
    for sy in range(h * ss):
        yc = (sy + 0.5) / ss
        xs_hits = []
        for x0, y0, x1, y1 in edges:
            if (y0 <= yc < y1) or (y1 <= yc < y0):
                t = (yc - y0) / (y1 - y0)
                xs_hits.append((x0 + t * (x1 - x0), 1 if y1 > y0 else -1))
        if not xs_hits:
            continue
        xs_hits.sort()
        wind = 0
        spans = []
        for k in range(len(xs_hits) - 1):
            wind += xs_hits[k][1]
            if wind != 0:
                spans.append((xs_hits[k][0], xs_hits[k + 1][0]))
        if not spans:
            continue
        row = counts[sy // ss]
        for a, b in spans:
            for sx in range(max(0, int(a * ss)), min(w * ss, int(b * ss) + 1)):
                xc = (sx + 0.5) / ss
                if a <= xc < b:
                    row[sx // ss] += 1

    stride = (w + 7) // 8
    out = bytearray(stride * h)
    full = ss * ss
    for yy in range(h):
        for xx in range(w):
            if counts[yy][xx] / full >= COVERAGE_THRESHOLD:
                out[yy * stride + (xx >> 3)] |= 0x80 >> (xx & 7)
    return w, h, bytes(out)


# --------------------------------------------------------------- sources -----
def parse_markers(path):
    js = io.open(path, encoding='utf-8', errors='replace').read()
    shapes = {m.group(1): m.group(3) for m in re.finditer(
        r"'([a-z0-9_]+)':\s*\{[^{}]*?path:\s*(['\"])(.*?)\2", js, re.S)}

    def table(name):
        m = re.search(name + r'\s*=\s*\{(.*?)\n\};', js, re.S)
        if not m:
            return {}
        return dict(re.findall(r"'([A-Z0-9\-]+)'\s*:\s*\[?\s*'([a-z0-9_]+)'",
                               m.group(1)))

    return shapes, table('TypeDesignatorIcons'), table('TypeDescriptionIcons')


def parse_icaolist(path):
    """designator -> ICAO description code such as 'L2J'."""
    out = {}
    with io.open(path, encoding='utf-8', errors='replace', newline='') as f:
        for row in csv.reader(f):
            if len(row) < 3:
                continue
            t = row[0].strip().upper()
            cls = CLASS_LETTER.get(row[1].strip().lower())
            parts = row[2].strip().split('/')
            if not t or not cls or len(parts) < 2:
                continue
            eng = ENGINE_LETTER.get(parts[1].strip().lower())
            if not eng or not parts[0].strip().isdigit():
                continue
            out.setdefault(t, f'{cls}{parts[0].strip()}{eng}')
    return out


def resolve(designator, exact, codes, by_code, shapes):
    """Which shape a type designator should use, and how we decided."""
    s = exact.get(designator)
    if s and s in shapes:
        return s, 'exact'
    code = codes.get(designator)
    if not code:
        return None, None
    if code.startswith('L2J') or code.startswith('L3J'):
        want = 'L2J-H' if designator in WIDEBODY else 'L2J-M'
        s = by_code.get(want)
        if s and s in shapes:
            return s, 'widebody' if designator in WIDEBODY else 'twinjet'
    for key in (code, code[0]):
        s = by_code.get(key)
        if s and s in shapes:
            return s, 'by class'
    return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--width', type=int, default=88,
                    help='bitmap box width in pixels (default: 88)')
    ap.add_argument('--height', type=int, default=104,
                    help='bitmap box height in pixels (default: 104)')
    ap.add_argument('--out', default=OUT)
    args = ap.parse_args()

    print('Aircraft silhouettes')
    markers = fetch(MARKERS_URL, 'markers.js')
    icaolist = fetch(ICAOLIST_URL, 'ICAOList.csv')

    shapes, exact, by_code = parse_markers(markers)
    codes = parse_icaolist(icaolist)
    print(f'  {len(shapes)} shapes, {len(exact)} exact type entries, '
          f'{len(by_code)} class entries, {len(codes)} designators in Doc 8643')

    assignments, how = {}, {}
    for designator in set(codes) | set(exact):
        if not 2 <= len(designator) <= DESIGNATOR_LEN:
            continue
        shape, why = resolve(designator, exact, codes, by_code, shapes)
        if shape:
            assignments[designator] = shape
            how[why] = how.get(why, 0) + 1
    if not assignments:
        sys.exit('nothing resolved -- the upstream format has probably changed')

    used = sorted(set(assignments.values()))
    print(f'  {len(assignments)} designators resolved to {len(used)} shapes  '
          + ', '.join(f'{k} {v}' for k, v in sorted(how.items())))

    print('  rasterising ...', end='', flush=True)
    bitmaps, index = [], {}
    for name in used:
        polys = flatten(shapes[name])
        w, h, bits = rasterise(polys, args.width, args.height)
        if w == 0 or not any(bits):
            continue  # a path we could not make sense of; drop it rather than
                      # ship an empty box
        index[name] = len(bitmaps)
        bitmaps.append((w, h, bits))
    print(f' {len(bitmaps)} bitmaps')

    entries = sorted((d.encode('ascii').ljust(DESIGNATOR_LEN, b'\0'), index[s])
                     for d, s in assignments.items() if s in index)

    shape_tab_off = HEADER_LEN
    map_tab_off = shape_tab_off + len(bitmaps) * 8
    bitmap_off = map_tab_off + len(entries) * (DESIGNATOR_LEN + 2)

    blob = bytearray()
    shape_tab = bytearray()
    for w, h, bits in bitmaps:
        shape_tab += struct.pack('<HHI', w, h, len(blob))
        blob += bits

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'wb') as f:
        header = (MAGIC
                  + struct.pack('<HHHH', VERSION, len(bitmaps), len(entries), 0)
                  + struct.pack('<III', shape_tab_off, map_tab_off, bitmap_off))
        f.write(header.ljust(HEADER_LEN, b'\0'))
        f.write(shape_tab)
        for name, idx in entries:
            f.write(name + struct.pack('<H', idx))
        f.write(blob)

    size = bitmap_off + len(blob)
    print(f'\n  {len(entries)} type designators -> {args.out} '
          f'({size/1024:.0f} KB)')
    print('  flash it with: pio run -t uploadfs')


if __name__ == '__main__':
    main()
