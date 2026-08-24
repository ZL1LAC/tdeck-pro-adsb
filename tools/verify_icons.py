#!/usr/bin/env python3
"""Read data/icons.bin back with an independent parser and draw it as text.

Deliberately does not import build_icons.py. This is a second implementation of
the format, and more importantly it is the only way to see what was actually
rasterised: the e-paper cannot be screenshotted, and a silhouette that came out
as a blob, a hollow outline or an empty box would otherwise not be noticed
until it was on the glass.

    python tools/verify_icons.py                    # structure + a few shapes
    python tools/verify_icons.py --type B738 C172 R44
    python tools/verify_icons.py --all              # every distinct shape
"""

import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), 'data', 'icons.bin')

DESIGNATOR_LEN = 4


def load(path):
    with open(path, 'rb') as f:
        blob = f.read()
    if len(blob) < 32:
        sys.exit('file is too short to hold a header')
    if blob[:8] != b'TDECKICO':
        sys.exit(f'bad magic {blob[:8]!r}')

    version, shape_count, map_count, _ = struct.unpack('<HHHH', blob[8:16])
    shape_off, map_off, bitmap_off = struct.unpack('<III', blob[16:28])
    if version != 1:
        sys.exit(f'this tool reads version 1, file is version {version}')
    print(f'version {version}  shapes {shape_count}  designators {map_count}  '
          f'{len(blob)/1024:.0f} KB')

    if 32 + shape_count * 8 != map_off:
        sys.exit(f'shape table ends at {32 + shape_count * 8}, map claims {map_off}')
    if map_off + map_count * (DESIGNATOR_LEN + 2) != bitmap_off:
        sys.exit('map table size disagrees with the bitmap offset')

    shapes = []
    for i in range(shape_count):
        w, h, off = struct.unpack('<HHI', blob[shape_off + i*8:shape_off + i*8 + 8])
        stride = (w + 7) // 8
        start = bitmap_off + off
        end = start + stride * h
        if end > len(blob):
            sys.exit(f'shape {i} runs past the end of the file')
        shapes.append((w, h, blob[start:end]))

    mapping = {}
    for i in range(map_count):
        base = map_off + i * (DESIGNATOR_LEN + 2)
        raw = blob[base:base + DESIGNATOR_LEN]
        idx, = struct.unpack('<H', blob[base + DESIGNATOR_LEN:base + DESIGNATOR_LEN + 2])
        if idx >= shape_count:
            sys.exit(f'designator {raw!r} points at shape {idx} of {shape_count}')
        mapping[raw.split(b'\0')[0].decode('ascii', 'replace')] = idx
    return shapes, mapping, blob


def render(shape, cols=64):
    """Draw a bitmap as text, the way the panel would draw it in ink."""
    w, h, bits = shape
    stride = (w + 7) // 8

    def on(x, y):
        return bits[y * stride + (x >> 3)] & (0x80 >> (x & 7))

    # Two source rows per text row, so the aspect ratio survives a terminal
    # cell being about twice as tall as it is wide.
    step = max(1, w // cols)
    out = []
    for y in range(0, h - 1, 2 * step):
        line = []
        for x in range(0, w, step):
            top = on(x, y)
            bot = on(x, y + step) if y + step < h else 0
            # ASCII rather than half-block glyphs: this has to render on a
            # Windows console in its default code page too.
            line.append('#' if top and bot else
                        "'" if top else
                        '.' if bot else ' ')
        out.append(''.join(line).rstrip())
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--file', default=OUT)
    ap.add_argument('--type', nargs='*', default=None,
                    help='ICAO type designators to draw, as the firmware would')
    ap.add_argument('--all', action='store_true', help='draw every distinct shape')
    args = ap.parse_args()

    shapes, mapping, blob = load(args.file)

    # The firmware binary-searches the designator table on its NUL-padded
    # 4-byte key, so it has to be sorted that way here.
    keys = list(mapping)
    padded = [k.encode('ascii').ljust(DESIGNATOR_LEN, b'\0') for k in keys]
    if padded != sorted(padded):
        sys.exit('FAIL: designator table is not sorted -- a binary search '
                 'would miss entries')
    print(f'PASS: {len(keys)} designators sorted for binary search')

    empty = [i for i, (w, h, bits) in enumerate(shapes) if not any(bits)]
    if empty:
        sys.exit(f'FAIL: {len(empty)} shapes rasterised to nothing')
    print(f'PASS: all {len(shapes)} shapes carry ink')

    # A silhouette that fills its whole box is a rasteriser that has filled the
    # bounding box rather than the aircraft.
    solid = []
    for i, (w, h, bits) in enumerate(shapes):
        lit = sum(bin(b).count('1') for b in bits)
        if lit > 0.85 * w * h:
            solid.append(i)
    if solid:
        sys.exit(f'FAIL: shapes {solid} are almost solid -- winding rule wrong?')
    print('PASS: no shape is a solid block')

    if args.all:
        wanted = sorted({v: k for k, v in sorted(mapping.items())}.items())
        for idx, name in wanted:
            w, h, _ = shapes[idx]
            print(f'\n--- shape {idx}  {w}x{h}  (e.g. {name})')
            print('\n'.join(render(shapes[idx])))
        return

    targets = args.type or ['A320', 'B738', 'B789', 'C172', 'R44', 'DH8C']
    for t in targets:
        t = t.upper()
        if t not in mapping:
            print(f'\n--- {t}: no silhouette in this build')
            continue
        w, h, _ = shapes[mapping[t]]
        print(f'\n--- {t}  ->  shape {mapping[t]}  {w}x{h} px')
        print('\n'.join(render(shapes[mapping[t]])))


if __name__ == '__main__':
    main()
