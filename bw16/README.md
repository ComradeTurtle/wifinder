# BW16 (RTL8720DN) Slave Node Firmware

This firmware lets a BW16 act as a wired slave scanner for the ESP32-C6 master using the existing node UART framing protocol.

## What It Does

- Receives master commands over UART: `HELLO`, `CONFIG`, `PING`
- Sends node replies: `HELLO_ACK`, periodic `STATUS`, `PONG`, `SIGHTING`
- Scans APs and forwards 2.4 GHz + 5 GHz sightings to the master
- Keeps packet payload format compatible with the current master parser (`14 + ssid_len` bytes for `SIGHTING`)

## Build / Flash

Use Arduino IDE with the Realtek/Ameba board package for BW16.

1. Open `bw16/src/main.cpp` as your sketch source.
2. Select the BW16 board target.
3. Flash over USB (`/dev/ttyUSB0` on Linux).

## Wiring (C6 <-> BW16)

- `BW16 LOG_TX (PA7, Serial TX)` -> `ESP32-C6 GPIO4` (node RX)
- `BW16 LOG_RX (PA8, Serial RX)` <- `ESP32-C6 GPIO5` (node TX)
- GND <-> GND

Optional:

- Flash over USB, then run from VIN/3V3 for node-link use. `Serial` on PA7/PA8
  shares pins with log UART, so keeping USB-UART attached during runtime can
  interfere with the node link.

## Notes

- Node UART baud: `115200`
- The existing 16-bit channel mask controls 2.4 GHz channels (`1..13`) only.
- 5 GHz sightings are always forwarded by BW16 for compatibility with the current protocol.
