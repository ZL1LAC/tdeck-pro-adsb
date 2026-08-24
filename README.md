# T-Deck Pro ADS-B Tracker

Live aircraft on the LilyGO **T-Deck Pro v1.0**: a north-up radar scope and a
sortable traffic list on the 3.1" e-paper, driven from a public ADS-B feed over
Wi-Fi.

```
+----------------------------------+
|RDR 40nm|12ac OK|.ıl Gok*|23:45 87%|  status bar: view+range, feed, radios, time+power
+----------------------------------+
|                          +4 out  |
|[o]          N                    |   recentre (only when panned)
|         .---------.              |
|       /   ~~~\   \               |   coastline, airports and runways
|      |    ---+---   |            |   range rings, own position at the centre
|       \      |    ~~/~           |   from the basemap in SPIFFS
|         '---------'              |
|       >BAW117  >EZY83AF          |   heading triangles + leader lines
|        FL340    FL257            |
|[-]          S           [+]      |   zoom
+----------------------------------+
|BAW117   FL340  452kt             |   selection / key hints
|B77W      12.4nm 041 NE           |
+----------------------------------+
```

## Why it is a client and not a receiver

ADS-B is 1090 MHz at 1 Mbit/s PPM. The T-Deck Pro's SX1262 is a sub-GHz LoRa
transceiver and does not tune anywhere near that band, and even with a suitable
front end the ESP32-S3 could not sustain the ~2 MSPS demodulation. So the board
polls an aggregator instead. Its LoRa and 4G rails are held powered down.

## Hardware

| Part | Device | Notes |
| --- | --- | --- |
| MCU | ESP32-S3, 16 MB flash, 8 MB QSPI PSRAM | |
| Display | GDEQ031T10 e-paper, 240x320, UC8253 | ~1.1 s full refresh, ~0.7 s partial |
| Keyboard | TCA8418 matrix controller @ 0x34 | 4x10, BlackBerry-style |
| Touch | CST328 @ 0x1A | single finger, INT-driven; taps and drags |
| GNSS | u-blox MIA-M10Q on UART2 | optional; supplies the plot centre |
| Fuel gauge | BQ27220 @ 0x55 | battery percentage |
| Charger | BQ25896 @ 0x6B | I2C watchdog disabled at boot |

Pin assignments live in [board_pins.h](include/board_pins.h), transcribed from
LilyGO's `examples/factory/utilities.h` for hardware revision v1.0-241106.

## Getting it running

1. **Set your Wi-Fi and home position.** Copy the template and edit the copy:

   ```
   cp include/secrets.example.h include/secrets.h
   ```

   ```c
   static const WifiCredential kWifiNetworks[] = {
       {"YOUR_SSID", "YOUR_PASSWORD"},
   };

   #define HOME_LATITUDE  -36.848460
   #define HOME_LONGITUDE 174.763332
   ```

   The home position is where you are until the GNSS gets a fix — and
   permanently, if you set `GNSS_ENABLED` to `false`.

   `secrets.h` is gitignored so your password and your address stay on your
   machine; the build stops with a clear error if you skip this step.
   Everything else — provider, poll interval, units, range steps — is in
   [config.h](include/config.h).

2. **Build and flash.** PlatformIO picks up the bundled board definition from
   `boards/`:

   ```
   pio run -t upload
   pio device monitor
   ```

3. **Optional: build the basemap.** Coastlines, airports and runways for your
   region, flashed into SPIFFS separately from the firmware:

   ```
   python tools/build_map.py
   pio run -t uploadfs
   ```

   Defaults to New Zealand; pass `--bbox MINLAT MINLON MAXLAT MAXLON` for
   anywhere else. Skip it entirely and the radar just draws without a map.

4. **First boot** shows a splash, then the radar. The status bar tells you what
   is working; press `i` for the full diagnostics page.

## Controls

Keyboard:

| Key | Action |
| --- | --- |
| `w` `a` `s` `d` | Pan the plot (quarter of a screen per press) |
| `z` `x` | Zoom out / in (5 – 250 nm) |
| `c` | Recentre on your own position |
| `j` `k` | Next / previous target |
| `t` | Toggle radar / list |
| Enter | Open the detail page for the selected target |
| Del | Back, or clear the selection |
| `r` | Poll the feed now |
| `f` | Force a full e-paper refresh (clears ghosting) |
| `g` | Take your own position from GNSS vs. the configured home |
| `l` | Keyboard backlight |
| `m` | Basemap on / off |
| `p` | Swap feed: your own receiver / the aggregator |
| `i` | Diagnostics page |

In the list and detail views there is nothing to pan, so `w`/`s` move the
selection there instead.

Touch:

- **Drag** anywhere on the radar to pan. The panel needs ~0.7 s per refresh, so
  the plot does not track your finger — it redraws once, on release.
- **Tap** an aircraft to select it, a list row to select it, a selected row (or
  the footer) to open its detail page, or the status bar to swap views.
- **`+` / `−` buttons** in the bottom corners zoom. Once panned, a crosshair
  button appears top-left to recentre.

Panning moves the plot, not you: ranges and bearings in the list, footer and
detail page are always measured from your own position, which stays drawn as a
ringed crosshair wherever it falls on screen. The feed query follows the view —
radius becomes visible range plus pan distance — so dragging somewhere new
actually fetches traffic there rather than showing an empty circle.

> The five modifier keys report stand-in characters from LilyGO's keymap
> (`E` = Enter, `U` = Del, `S` = Shift, `$` = Sym, `2` = Alt). If a key on your
> unit does something unexpected, every press is logged to serial as
> `keypad: '<c>' (row,col)` — adjust `kKeymap` in
> [keypad.cpp](src/hw/keypad.cpp).

## Data feeds

Your own receiver, or one of three key-less public aggregators. Pick with
`ADSB_PROVIDER`.

| Provider | Endpoint | Notes |
| --- | --- | --- |
| `LOCAL` (default) | `ADSB_LOCAL_URL` in secrets.h | Your own receiver. Plain HTTP, sub-second positions |
| `ADSB_FI` | `opendata.adsb.fi` | Includes the `desc` type description |
| `ADSB_LOL` | `api.adsb.lol` | No published rate limit |
| `AIRPLANES_LIVE` | `api.airplanes.live` | Asks for ≤ 1 request/second |

All four serve the same readsb-shaped JSON, so one parser and one field filter
cover the lot; they differ only in URL layout and in whether the array is called
`aircraft` or `ac`.

`ADSB_PROVIDER_DEFAULT` picks what the firmware boots on and `p` swaps between
that and `ADSB_PROVIDER_REMOTE` at runtime, so you can walk out of Wi-Fi range
and keep seeing traffic. The radar footer names whichever is live. Two things
follow from the swap being a runtime choice rather than a compiled-in one:
both transports are linked, which costs about 120 KB of flash in mbedtls that a
local-only build would not pay; and there is no NVS yet, so the choice lasts
until the next reboot.

Switching leaves the old feed's targets to age out rather than clearing them.
They merge by ICAO hex and `mergeString()` only overwrites a field that arrives
with something in it, so a trip through the aggregator fills in the registration
and type that a local `aircraft.json` does not carry, and they survive the swap
back while positions return to being a fraction of a second old.

### Feeding from your own receiver

If you run dump1090, readsb or PiAware, the decoded state it already publishes
is a better source than any aggregator. Point `ADSB_LOCAL_URL` at its
`aircraft.json` — the usual paths are `/tar1090/data/aircraft.json`,
`/skyaware/data/aircraft.json`, or `:8080/data/aircraft.json` — and the firmware
polls that instead.

What it buys, against a public aggregator:

- **Positions under a second old** (`seen_pos` is typically 0.2–0.5 s) rather
  than several, refreshed at 1 Hz, so `ADSB_POLL_INTERVAL_MS` drops to 5 s.
- **A few kilobytes per poll** instead of a few hundred.
- **No TLS.** The handshake was the most expensive part of a poll — it dominated
  the 1.3–1.5 s an aggregator fetch took and churned tens of kilobytes of
  mbedtls heap every time. Skipping it also lets the linker drop mbedtls
  altogether, which is worth 121 KB of flash.
- **No rate limit, and no internet.** It keeps working when the WAN is down.

What it costs:

- **Coverage stops at your antenna's horizon.** The aggregators pool every
  receiver in the country; you see what you can hear.
- **No registration or type designator.** `aircraft.json` carries neither unless
  readsb is run with a `--db-file`. Rather than spend ~50 MB of the Pi's RAM on
  that, the device carries its own lookup table — see [Aircraft
  database](#aircraft-database). Note the trap in the JSON: there *is* a `type`
  field, but it holds `adsb_icao` — the message source, not the airframe.
- **It only exists on your own network.** Carry the board out of range and the
  feed goes quiet until you switch `ADSB_PROVIDER` back.

A local `aircraft.json` also has no radius parameter — it carries everything the
receiver hears — so `ADSB_QUERY_RADIUS_NM` is applied client-side after parsing
instead of by the server. Either way the radius follows whatever range the radar
is showing, so zooming out pulls in more traffic.

### Against an aggregator

Please keep `ADSB_POLL_INTERVAL_MS` sensible — these are volunteer-run feeds,
and the e-paper cannot repaint faster than about 1.5 Hz anyway.

TLS uses `setInsecure()`. The payload is public, read-only and unauthenticated,
and a pinned root baked into flash would silently expire the first time a
provider rotated certificates. If you would rather pin one, do it in
[adsb_source.cpp](src/net/adsb_source.cpp).

### Why not Beast?

Port 30005 carries raw Mode S frames, and decoding them on the ESP32 would mean
CRC-24 with address overlay, CPR even/odd position decoding, the 12-bit altitude
field with its Q-bit split, the 6-bit callsign charset and the velocity
subtypes — several hundred lines re-deriving what the Pi has already computed.
The deeper problem is that Beast is a stream of *events* where the display wants
*state*: you only learn about an aircraft when it next transmits, so a reconnect
leaves the radar empty and fills it over ~30 s, while one GET of `aircraft.json`
returns the receiver's whole accumulated picture. A persistent socket also wakes
the Wi-Fi modem on every frame, where polling lets it idle in between.

## Basemap

The radar draws over simplified vector coastlines, airports and runway
centrelines, held in the SPIFFS partition as `/map.bin`. Three detail levels
are stored and the plotted range picks one; `m` toggles the whole layer.

Vector rather than raster tiles, for three reasons. The panel is 1-bit, so
dithered imagery turns to mud underneath the aircraft markers. Every frame
redraws from scratch, so per-frame tile decode would be paid over and over
rather than once. And vectors need no network, which is exactly when a map
earns its keep. All of New Zealand costs 49 KB — 1.4% of the partition.

Drawing it adds no e-paper refreshes: the map is a pure function of the plot
centre and the range, both of which already feed the scene hash.

| Layer | Source | Licence |
| --- | --- | --- |
| Coastline | Natural Earth 1:10m | public domain |
| Airports, runways | OurAirports | public domain |
| Airspace | an OpenAir file you supply | varies — see below |

[`tools/build_map.py`](tools/build_map.py) fetches the first two on demand and
caches them under `tools/.mapcache`. Airspace cannot be fetched automatically —
openAIP needs an API key, and soaringweb blocks scripted access — so point it at
an OpenAir file yourself:

```
python tools/build_map.py --airspace mapsrc/nz-airspace.txt
pio run -t uploadfs
```

Keep the source file **out of `data/`** — that directory is the SPIFFS image, so
anything left there gets flashed to the device as dead weight. `mapsrc/` is a
good home for it.

### Where to get an OpenAir file

XCSoar publishes a machine-readable index of community airspace files at
<http://download.xcsoar.org/repository>; grep it for `type=airspace` plus your
country. The New Zealand entry points at Gliding New Zealand:

```
https://gliding.co.nz/wp-content/uploads/2023/01/2022_nz_airspace_v3.txt
```

openAIP also has worldwide coverage and exports OpenAir from the website once
you have a free account (the API needs a key; the site does not). Its data is
CC BY-NC-SA, so check that suits you.

> **This is not navigation data.** The Gliding NZ file above is derived from the
> 2021–22 Air Navigation Register — years stale, and airspace changes. It is
> useful for recognising roughly where traffic is operating and nothing more.
> The parser also drops everything except the classes you ask for, so what is
> drawn is deliberately incomplete.

The class filter matters more than it sounds. The default is `CTR,P,R` — control
zones plus prohibited and restricted areas. Adding `C,D` pulls in the terminal
area blankets, which on a 240×320 mono panel bury the traffic completely:

```
python tools/build_map.py --airspace mapsrc/nz-airspace.txt --airspace-classes all
```

Preview before you commit to it — see below.

[`tools/verify_map.py`](tools/verify_map.py) reads the blob back with an
independent parser — a second implementation of the format, so a swapped
coordinate or broken alignment shows up as garbage rather than passing quietly.
It plots the whole region, or renders the actual radar view for a position and
range, picking the detail level the same way the firmware does:

```
python tools/verify_map.py                                     # whole region
python tools/verify_map.py --centre -36.5938 174.6944 --range 40
```

The e-paper cannot be screenshotted, so that second form is the only way to see
what a build will look like before flashing it. Use it to judge clutter.

Only the Python standard library is used, so both scripts run wherever the
toolchain does.

## Aircraft database

`aircraft.json` gives you `hex` but not the registration or the type
designator. readsb will serve both if started with a `--db-file`, but holding
that database costs a Raspberry Pi around 50 MB of RAM. The mapping is fixed, so
carrying it on the device instead costs the Pi nothing and needs no network:

```
python tools/build_db.py --blocks C8,7C
pio run -t uploadfs
```

The full database is 615k records and 30 MB uncompressed, which neither fits a
3.4 MB SPIFFS partition nor needs to — an aircraft has to be within radio range
to appear on the plot, so only the ICAO address blocks you can actually hear are
worth carrying.

| Blocks | Records | On SPIFFS |
| --- | --- | --- |
| `C8` — New Zealand and Fiji | 4,898 | 77 KB |
| `C8,7C` — adding Australia | 22,807 | 356 KB |
| `all` | 615,897 | 9 MB — will not fit |

Pick by what actually turns up: a sample of one Auckland feed had five NZ and
two Australian aircraft airborne at once, so `C8,7C` is the sensible default
there. Missing data is not an error — an aircraft the table does not know keeps
empty fields, exactly as before, and a later poll through an aggregator can
still fill them in.

The source is [tar1090-db](https://github.com/wiedehopf/tar1090-db) (ODbL),
fetched on demand and cached in `tools/.dbcache`.
[`tools/verify_db.py`](tools/verify_db.py) reads the blob back with an
independent parser — the same second-implementation trick `verify_map.py` uses —
and checks that the keys really are strictly ascending, since the firmware
binary-searches them:

```
python tools/verify_db.py                      # structure + a sample
python tools/verify_db.py --hex c82347 7c561d  # resolve specific aircraft
```

## Settings that survive a reboot

Range, basemap on/off, GNSS centring, keyboard backlight and the chosen feed are
kept in NVS. Everything else — the pan offset, the selection, the view — is
deliberately not: restoring them would bring back a picture of traffic that flew
away hours ago.

Writes are deferred five seconds and skipped when the bytes have not actually
changed, so holding a zoom key through a dozen range steps costs one write
rather than twelve, and toggling something back the way it was costs none.

## How it is put together

```
src/
  main.cpp              orchestration: poll, gather context, render
  core/
    aircraft.h          one target, flat POD
    aircraftdb.{h,cpp}  ICAO -> registration/type, binary search in PSRAM
    settings.{h,cpp}    the handful of choices worth keeping in NVS
    tracker.{h,cpp}     merge snapshots, age out, sort by range, scene hash
    geo.{h,cpp}         haversine, bearing, flat-earth projection, units
  hw/
    power.{h,cpp}       I2C bus, rails, charger watchdog, fuel gauge
    keypad.{h,cpp}      TCA8418 matrix decode
    touch.{h,cpp}       CST328 press/release, interrupt-driven
    gnss.{h,cpp}        MIA-M10Q with UART baud probing
  net/
    net.{h,cpp}         Wi-Fi association with backoff across several SSIDs
    adsb_source.{h,cpp} HTTPS GET + filtered streaming JSON parse
  ui/
    display.{h,cpp}     GxEPD2 wrapper and the refresh policy
    ui.{h,cpp}          views, input handling, layout
```

Four design points worth knowing:

**Nothing repaints unless it changed.** `Tracker::sceneHash()` fingerprints
everything the renderer draws, quantised so GPS jitter does not count as change.
`display::render()` compares it against the frame already on the glass and
returns early on a match, and enforces a 4 s floor between refreshes on top of
that. Every 20th repaint (or after 10 minutes) is promoted from a partial to a
full refresh to clear ghosting.

Both halves of that matter, and the trap is easy to fall into twice. The first
build displayed raw RSSI in the status bar; `WiFi.RSSI()` wanders several dB
between reads, so the frame genuinely differed every loop and the panel
repainted about 1.4 times a second. Signal strength is now four bars with 3 dB
of hysteresis. The satellite count sat in the same hash and did the same thing
— an open-sky constellation gains and loses a satellite most seconds, which
was enough to drive the panel at the 4 s floor rather than once per poll — so
the bar now shows `Gok` / `Glo` with two satellites of hysteresis. Exact dBm
and the exact satellite count are both on the diagnostics page, where numbers
that precise are actually useful.

Only a view change and the `f` key ask for a full refresh. Panning, zooming
and recentring keep the same view and take the 651 ms partial rather than the
1016 ms flash; a burst of them still trips the every-20th-partial promotion,
so ghosting is cleared during the burst instead of on every press of it.

**The feed is parsed straight off the socket.** A busy 250 nm query is a couple
of hundred kilobytes of JSON. An ArduinoJson filter keeps only the fourteen
fields the UI uses, the parse tree is allocated from PSRAM through a custom
allocator so it cannot starve the Wi-Fi stack, and the response is never
buffered whole.

**Targets are merged, not replaced.** Aircraft drop in and out of a feed's
coverage between polls; wholesale replacement makes the plot flicker. Snapshots
merge into a fixed 96-slot store and entries retire after 90 s. Ageing also runs
when the link is down, so a dropped connection decays the plot instead of
freezing it.

**The loop has a fast half and a slow half.** Draining the GNSS UART and
answering the keyboard have to happen on a 10 ms cadence — the serial ring
holds 67 ms of NMEA, and a keypress you cannot feel land is a keypress you
press twice. Nothing else does. Reassembling the UI context calls into
`esp_wifi` and `esp_netif` and hashing the scene walks all 96 slots, and at a
hundred passes a second that was the largest single expense in the firmware,
spent on changes no e-paper could show. It now runs once a second, or
immediately when input or a completed poll makes it worth doing.

The clock policy sits on top of that. The prebuilt Arduino libraries are
compiled without `CONFIG_PM_ENABLE`, so there is no frequency scaling and no
tickless idle to lean on — the core runs flat out at 240 MHz whatever it is
doing. Two things genuinely are CPU-bound, the TLS fetch and drawing a frame,
so `power::Boost` raises the clock around those and everything else runs at
80 MHz. 80 is the floor that keeps the PLL-derived APB clock at 80 MHz, so the
UART, I2C and SPI peripherals need no re-tuning across the change.

## Current footprint

```
RAM:   21.3% (69,928 / 327,680 bytes)
Flash: 15.7% (1,026,645 / 6,553,600 bytes)
SPIFFS: 73 KB basemap + 356 KB aircraft database, of 3.4 MB
PSRAM:  356 KB, holding the aircraft database for the life of the run

Pinning ADSB_PROVIDER_DEFAULT to LOCAL and deleting the aggregator branch of
adsb::fetch() drops mbedtls and takes flash to 13.7% (898,381 bytes).
```

Measured on hardware, 651 ms partial / 1016 ms full panel refresh either way:

| Feed | Fetch and parse |
| --- | --- |
| Aggregator over HTTPS, ~29 aircraft | 1.3–1.5 s |
| Local receiver over HTTP, ~8 aircraft | **34 ms** |

Nearly all of that difference is the TLS handshake rather than the payload. It
is the single biggest change in the firmware's power profile: a poll that used
to hold the CPU boosted for a second and a half now finishes in the gap between
two panel scans.

Which is what lets the local feed be polled at 1 Hz. Measured over a minute on
hardware, five aircraft in view at the default 40 nm range:

| | Aggregator timings | Local timings |
| --- | --- | --- |
| Poll interval, median | 6.02 s | 1.51 s |
| Partial refreshes | one per ~4 s | one per 2.36 s |
| Panel duty cycle | ~13% | ~28% |

The panel, not the network, is now the whole constraint. A partial refresh takes
651 ms, so 1.5 Hz is the ceiling, and every refresh is 651 ms of powered panel
and one more cycle of wear — a bit over 36,000 a day at the rate above. If the
board lives on a desk rather than in a hand, raising
`EPD_MIN_REFRESH_INTERVAL_MS` back towards 4 s halves that and costs only
latency nobody is watching for.

## Not done yet

- Runtime settings (Wi-Fi, home position, provider) are compile-time constants;
  there is no on-device settings screen or NVS persistence.
- No sort options in the list view — it is always nearest-first, and it does
  not drag-scroll (the visible window is driven by the selection).
- No pinch-zoom. The CST328 reports multiple contacts, but at 0.7 s per refresh
  a pinch gesture would be unusable; the corner buttons do the job instead.
- Touch axis orientation is unverified on hardware. If taps land transposed or
  mirrored — or dragging pans the wrong way — flip `kSwapXY` / `kMirrorX` /
  `kMirrorY` in [touch.cpp](src/hw/touch.cpp).
- No sleep of any kind. The Arduino libraries ship without `CONFIG_PM_ENABLE`,
  so automatic light sleep and DFS are unavailable without rebuilding the
  framework, and manual `esp_light_sleep_start()` with the station associated
  risks losing beacons. The CPU therefore never idles below 80 MHz, which is
  the floor on what this can draw. Duty-cycling the GNSS — it is powered
  continuously for a position that barely moves — is the next thing worth
  doing.
- Airspace has no automatic data source; you have to supply an OpenAir file,
  and the only one indexed for New Zealand is several years stale.
- The basemap has no roads or terrain, and no altitude filtering — an airspace
  is drawn whether its floor is the surface or FL245, so `AH`/`AL` are parsed
  and then thrown away.
- Nothing uses the SD slot, the IMU, the light sensor, or the 4G modem.
