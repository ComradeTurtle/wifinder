# wifinder (ESP32 scanner + Web/Android/Desktop clients)

This project runs a BLE-controlled Wi-Fi wardriving scanner stack centered on
an `ESP32-C6` master (optionally with an `ESP8266` or `BW16` wired slave node), with:
- a self-contained local web app
- an Android app that adds GPS ingestion and Wigle CSV logging on phone
- a desktop app (Python + Qt) for direct USB serial debugging/control

The end goal for WiFinder is an all-in-one wardriving PCB, featuring an ESP32-S3 acting as the master/controller node and 10 ESP32-C5 scan nodes, alongside a dual-band GNSS receiver and other extras. PCB design is currently WIP (soon™) and will be open-sourced once ready.

## What current firmware includes

- Wi-Fi management sniffing (beacon + probe response) on 2.4 GHz
- Selectable channel set (`1..13`) with configurable hop interval (`50..2000 ms`)
- Start/stop scan control over BLE
- BLE status + AP sighting notifications (binary framed protocol)
- WG protocol transport over USB serial (desktop client path)
- WG protocol v2 with session metadata on sightings/status
- SD-backed store-and-forward queue (SDSPI) with SPIFFS fallback, explicit replay control + host ACK cursor
- Live local web UI (Chromium) showing:
  - connection and scanner status
  - visible `(B)SSIDs`
  - detailed security labels (for example `WPA2-PSK-CCMP-128`)
  - channel occupancy + RSSI histogram + packet rate trend
- NVS persistence on ESP for:
  - hop interval
  - channel mask
  - boot mode (`manual` / `auto`)
- UART GPS ingestion (u-blox/NMEA)
- GPS nav-rate control:
  - dynamic `auto` mode (`1/2/4 Hz` based on speed)
  - forced modes for testing (`1 Hz`, `2 Hz`, `4 Hz`)

## BLE service

- Custom service UUID: `6ee2e690-d7fd-4a11-94a2-89528da43130`
- Characteristics:
  - Control: `...3131` (write / write-no-response)
  - Status: `...3132` (read / notify)
  - Sighting stream: `...3133` (notify)
  - Config: `...3134` (read / write)
  - Device info: `...3135` (read)
- Security: bonding + LE Secure Connections + MITM (fixed passkey `123456`)

## Firmware build and flash

Prerequisites:
- ESP-IDF activated in shell (`idf.py` available)
- Board connected at `/dev/ttyACM0`

```bash
idf.py set-target esp32c6
idf.py -p /dev/ttyACM0 flash monitor
```

If you intentionally build for a different target (for example legacy S3), set
the appropriate target first.

## Local web app

The web app is in `web/` and uses native Web Bluetooth (Chromium over
`localhost`/secure context).

```bash
npm --prefix web install
npm --prefix web run dev
```

Open the shown local URL in Chromium, click `Connect`, then use Start/Stop and
channel/hop controls.

## Desktop app (USB serial + Qt)

Desktop app lives in `desktop/` and talks to the master ESP over USB serial
using the same WG framed protocol as BLE.

```bash
python -m venv .venv-desktop
source .venv-desktop/bin/activate
pip install -r desktop/requirements.txt
python desktop/run.py
```

In the app:
- select `/dev/ttyACM*`
- connect
- use Start/Stop, hop/channels/boot controls, GPS nav-rate override, and CSV logging
- once the first WG command is received, firmware switches that USB stream to
  frame-only mode (ESP logs are suppressed on that stream to avoid protocol noise)

CSV output path on PC:
- `~/Downloads/wifinder/wigle-YYYYMMDD-HHMMSS.csv`

## Android app (BLE + GPS + CSV)

Android project lives in `android/`.

Features:
- BLE control and telemetry (same protocol as web app)
- Live visible `(B)SSID` table with timeout-based pruning
- GPS push fallback to ESP (`SET_GPS_FIX`) when phone fix exists
- ESP-originated GPS telemetry (`WG_MSG_GPS`) reception
- GPS nav-rate mode control (`Auto` / forced `1/2/4 Hz`) for field testing
- node-link telemetry from secondary scanner (ESP8266 or BW16)
- Wigle CSV logging generated on phone storage (prefers fresh ESP GPS, phone fallback)
- Explicit `Download ESP` backlog flow (`SET_REPLAY` on/off, blocked while scanner is active)

Build/test:

```bash
cat > android/local.properties <<'EOF'
sdk.dir=/path/to/Android/Sdk
EOF

GRADLE_USER_HOME=/tmp/gradle-home ./android/gradlew -p android test
```

Install debug build to connected phone:

```bash
GRADLE_USER_HOME=/tmp/gradle-home ./android/gradlew -p android installDebug
```

CSV output path on phone:
- public Downloads subfolder:
  - `Downloads/wifinder/wigle-YYYYMMDD-HHMMSS.csv`

## Wireshark protocol decoding

You can decode WIFINDER frames in BLE ATT traffic with the Lua post-dissector:

- Script path: `tools/wireshark/wifinder.lua`
- It parses WG frame headers and payloads for:
  - `STATUS`, `SIGHTING`, `ACK`, `ERROR`, `SNAPSHOT_END`, `CONFIG`, `GPS`, `REPLAY_ACK`, `NODE_TABLE`, `COMMAND`

Install:

```bash
mkdir -p ~/.local/lib/wireshark/plugins
cp tools/wireshark/wifinder.lua ~/.local/lib/wireshark/plugins/
```

Then restart Wireshark and filter as usual (for example `btatt`).  
Any ATT value beginning with WG magic (`57 47`) is decoded under a new
`WIFINDER Frame` tree and tagged in the packet Info column.

## Tests

Firmware host logic tests:

```bash
./scripts/run_host_tests.sh
```

Web protocol/state core tests:

```bash
npm --prefix web run test:core
```

Web production build:

```bash
npm --prefix web run build
```

Desktop protocol/state/CSV tests:

```bash
python -m unittest discover -s desktop/tests -v
```

## Notes

- ESP32 scanning in this project is 2.4 GHz only.
- 5 GHz scanning is available via an attached slave node (for example BW16).

## ESP8266 slave node firmware (for C6 master)

The `esp8266/` firmware is now a wired slave scanner for the ESP32-C6 master.

What it does:
- promiscuous Wi-Fi management scanning with channel hop
- UART framed protocol to C6:
  - RX: `HELLO`, `CONFIG`, `PING`
  - TX: `HELLO_ACK`, periodic `STATUS`, `PONG`, `SIGHTING`
- no onboard GPS, BLE, or flash logging (all logging remains on phone CSV side)

Build/upload:

```bash
cd esp8266
pio run
pio run -t upload --upload-port /dev/ttyUSB0
```

## BW16 slave node firmware (for C6 master)

The `bw16/` firmware provides the same node UART protocol but forwards
2.4 GHz + 5 GHz sightings from RTL8720DN/BW16 hardware.

Details and wiring notes:

```bash
cat bw16/README.md
```

## C6 + 8266 wiring

Current defaults in firmware:
- C6 node UART (LP UART):
  - TX: `GPIO5`
  - RX: `GPIO4`
  - baud: `460800`
- C6 GPS UART:
  - TX: `GPIO10`
  - RX: `GPIO11`
  - boot baud: `9600` (firmware probes and may switch module to higher runtime baud)
- C6 SD card (SDSPI):
  - CS: `GPIO20`
  - SCK: `GPIO21`
  - MOSI: `GPIO22`
  - MISO: `GPIO23`
- 8266 node UART:
  - TX0 (`GPIO1`) -> C6 RX (`GPIO4`)
  - RX0 (`GPIO3`) <- C6 TX (`GPIO5`)

Also connect common GND between boards.

## C6 + BW16 wiring

- C6 node UART (LP UART):
  - TX: `GPIO5`
  - RX: `GPIO4`
  - baud: `115200`
- BW16 node UART:
  - `LOG_TX (PA7, Serial TX)` -> C6 RX (`GPIO4`)
  - `LOG_RX (PA8, Serial RX)` <- C6 TX (`GPIO5`)

Also connect common GND between boards.

Storage backend behavior:
- firmware tries SD first at `/sdcard` (no auto-format)
- if SD mount fails or card is missing, firmware falls back to SPIFFS at `/spiffs`
