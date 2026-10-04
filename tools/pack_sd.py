#!/usr/bin/env python3
"""Stage files for a T-Deck Pro SD card.

Copies whatever generated blobs are in data/ onto a directory you then copy
to the card's root. The firmware prefers SD over SPIFFS, so a card can carry
a travel map or a worldwide aircraft database without a reflash.

    python tools/pack_sd.py              # writes ./sdcard/
    python tools/pack_sd.py --out E:/    # Windows: the card's drive letter

Layout the firmware looks for:

    /map.bin              vector basemap (or /maps/*.bin, picked in Settings)
    /aircraftdb.bin       ICAO registry (regional in PSRAM, worldwide by seek)
    /icons.bin            plan-view silhouettes
    /wifi.txt             optional override network (ssid=/pass=, or two lines)
    /home.txt             optional home position (lat=/lon=, or two numbers)
    /local.txt            optional local ADS-B feed URL (url=, or one line)
    /maps/*.bin           extra regions; cycle them from Settings → Map file
    /logs/adsb.csv        written when Track log is on
    /shots/*.pbm          1-bit screenshots, key `v`
"""

import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATA = os.path.join(ROOT, 'data')
DEFAULT_OUT = os.path.join(ROOT, 'sdcard')

BLOBS = ('map.bin', 'aircraftdb.bin', 'icons.bin')

WIFI_EXAMPLE = """# Rename to wifi.txt on the card. Applied at boot, not written to NVS.
# Either key=value:
ssid=YOUR_SSID
pass=YOUR_PASSWORD
# or two lines: SSID then password.
"""

HOME_EXAMPLE = """# Rename to home.txt on the card. Applied at boot, not written to NVS.
lat=-36.848460
lon=174.763332
"""

LOCAL_EXAMPLE = """# Rename to local.txt on the card. Applied at boot, not written to NVS.
# Your readsb/tar1090 aircraft.json URL (http or https).
url=http://192.168.1.10/tar1090/data/aircraft.json
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=DEFAULT_OUT,
                    help='directory to fill (default: ./sdcard)')
    args = ap.parse_args()
    dest = os.path.abspath(args.out)
    os.makedirs(dest, exist_ok=True)
    for name in ('maps', 'logs', 'shots'):
        os.makedirs(os.path.join(dest, name), exist_ok=True)

    copied = []
    missing = []
    for name in BLOBS:
        src = os.path.join(DATA, name)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(dest, name))
            copied.append(f'{name} ({os.path.getsize(src)/1024:.0f} KB)')
        else:
            missing.append(name)

    wifi_ex = os.path.join(dest, 'wifi.txt.example')
    home_ex = os.path.join(dest, 'home.txt.example')
    local_ex = os.path.join(dest, 'local.txt.example')
    if not os.path.exists(os.path.join(dest, 'wifi.txt')):
        with open(wifi_ex, 'w', encoding='ascii') as f:
            f.write(WIFI_EXAMPLE)
    if not os.path.exists(os.path.join(dest, 'home.txt')):
        with open(home_ex, 'w', encoding='ascii') as f:
            f.write(HOME_EXAMPLE)
    if not os.path.exists(os.path.join(dest, 'local.txt')):
        with open(local_ex, 'w', encoding='ascii') as f:
            f.write(LOCAL_EXAMPLE)

    print(f'SD card staging -> {dest}')
    for line in copied:
        print(f'  copied {line}')
    if missing:
        print('  not in data/ (build them, or skip): ' + ', '.join(missing))
    print('  maps/  extra *.bin files, picked from Settings')
    print('  logs/  traffic CSV appears here when Track log is on')
    print('  shots/ PBM dumps from the `v` key')
    print('Copy the contents of that folder to the card, not the folder itself.')
    return 0 if copied else 1


if __name__ == '__main__':
    sys.exit(main())
