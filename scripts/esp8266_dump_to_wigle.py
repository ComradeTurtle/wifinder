#!/usr/bin/env python3
"""Dump and convert ESP8266 standalone wardrive binary logs to WiGLE CSV."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import io
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, Optional

LOG_MAGIC = b"EWBIN01\x00"
LOG_VERSION = 1
LOG_HEADER_STRUCT = struct.Struct("<8sBB6sIH")

RECORD_FIXED_STRUCT = struct.Struct("<IIiiiH6sBbBBBBBB")
MAX_SSID_LEN = 32

WIGLE_HEADER = (
    "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,"
    "CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type"
)

WG_SEC_PROTO_WPA = 1 << 0
WG_SEC_PROTO_WPA2 = 1 << 1
WG_SEC_PROTO_WPA3 = 1 << 2

WG_SEC_AKM_EAP = 1 << 0
WG_SEC_AKM_PSK = 1 << 1
WG_SEC_AKM_SAE = 1 << 2
WG_SEC_AKM_OWE = 1 << 3

WG_SEC_CIPHER_TKIP = 1 << 0
WG_SEC_CIPHER_CCMP_128 = 1 << 1
WG_SEC_CIPHER_GCMP_128 = 1 << 2
WG_SEC_CIPHER_GCMP_256 = 1 << 3
WG_SEC_CIPHER_CCMP_256 = 1 << 4
WG_SEC_CIPHER_WEP = 1 << 5


@dataclass
class LogHeader:
    mac: bytes
    session_unix_s: int


@dataclass
class SightingRecord:
    monotonic_ms: int
    gps_unix_s: int
    lat_e7: int
    lon_e7: int
    alt_mm: int
    accuracy_cm: int
    bssid: bytes
    channel: int
    rssi: int
    auth_mode: int
    proto_flags: int
    akm_flags: int
    cipher_flags: int
    ssid: str
    flags: int


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def parse_header(blob: bytes) -> LogHeader:
    if len(blob) < LOG_HEADER_STRUCT.size:
        raise ValueError("log too small for header")
    magic, version, _reserved, mac, session_unix_s, crc = LOG_HEADER_STRUCT.unpack_from(blob, 0)
    if magic != LOG_MAGIC:
        raise ValueError("bad log magic")
    if version != LOG_VERSION:
        raise ValueError(f"unsupported log version: {version}")
    calc = crc16_ccitt(blob[: LOG_HEADER_STRUCT.size - 2])
    if crc != calc:
        raise ValueError("header CRC mismatch")
    return LogHeader(mac=mac, session_unix_s=session_unix_s)


def iter_records(blob: bytes) -> Iterator[SightingRecord]:
    pos = LOG_HEADER_STRUCT.size
    end = len(blob)

    while pos < end:
        if pos + RECORD_FIXED_STRUCT.size > end:
            break
        fixed = blob[pos : pos + RECORD_FIXED_STRUCT.size]
        pos += RECORD_FIXED_STRUCT.size
        (
            monotonic_ms,
            gps_unix_s,
            lat_e7,
            lon_e7,
            alt_mm,
            accuracy_cm,
            bssid,
            channel,
            rssi,
            auth_mode,
            proto_flags,
            akm_flags,
            cipher_flags,
            ssid_len,
            flags,
        ) = RECORD_FIXED_STRUCT.unpack(fixed)
        if ssid_len > MAX_SSID_LEN:
            raise ValueError(f"bad ssid_len={ssid_len} at pos={pos}")
        if pos + ssid_len + 2 > end:
            raise ValueError("truncated record")

        ssid_bytes = blob[pos : pos + ssid_len]
        pos += ssid_len
        (wire_crc,) = struct.unpack_from("<H", blob, pos)
        pos += 2

        record_without_crc = fixed + ssid_bytes
        calc_crc = crc16_ccitt(record_without_crc)
        if wire_crc != calc_crc:
            raise ValueError(f"record CRC mismatch at pos={pos}")

        ssid = ssid_bytes.decode("utf-8", errors="replace")
        yield SightingRecord(
            monotonic_ms=monotonic_ms,
            gps_unix_s=gps_unix_s,
            lat_e7=lat_e7,
            lon_e7=lon_e7,
            alt_mm=alt_mm,
            accuracy_cm=accuracy_cm,
            bssid=bssid,
            channel=channel,
            rssi=rssi,
            auth_mode=auth_mode,
            proto_flags=proto_flags,
            akm_flags=akm_flags,
            cipher_flags=cipher_flags,
            ssid=ssid,
            flags=flags,
        )


def bssid_to_text(bssid: bytes) -> str:
    return ":".join(f"{x:02X}" for x in bssid)


def channel_to_frequency(channel: int) -> int:
    if 1 <= channel <= 13:
        return 2407 + channel * 5
    if channel == 14:
        return 2484
    if 32 <= channel <= 177:
        return 5000 + channel * 5
    return 0


def build_security_label(proto_flags: int, akm_flags: int, cipher_flags: int, auth_mode: int) -> str:
    parts: list[str] = []
    if proto_flags & (WG_SEC_PROTO_WPA2 | WG_SEC_PROTO_WPA3) == (WG_SEC_PROTO_WPA2 | WG_SEC_PROTO_WPA3):
        parts.append("WPA2/WPA3")
    elif proto_flags & WG_SEC_PROTO_WPA3:
        parts.append("WPA3")
    elif proto_flags & WG_SEC_PROTO_WPA2:
        parts.append("WPA2")
    elif proto_flags & WG_SEC_PROTO_WPA:
        parts.append("WPA")
    elif auth_mode == 2 or (cipher_flags & WG_SEC_CIPHER_WEP):
        parts.append("WEP")
    elif auth_mode == 1:
        parts.append("OPEN")
    else:
        parts.append("UNKNOWN")

    if akm_flags & (WG_SEC_AKM_PSK | WG_SEC_AKM_SAE) == (WG_SEC_AKM_PSK | WG_SEC_AKM_SAE):
        parts.append("PSK/SAE")
    elif akm_flags & WG_SEC_AKM_SAE:
        parts.append("SAE")
    elif akm_flags & WG_SEC_AKM_PSK:
        parts.append("PSK")
    elif akm_flags & WG_SEC_AKM_EAP:
        parts.append("EAP")
    elif akm_flags & WG_SEC_AKM_OWE:
        parts.append("OWE")

    if cipher_flags & WG_SEC_CIPHER_CCMP_256:
        parts.append("CCMP-256")
    elif cipher_flags & WG_SEC_CIPHER_GCMP_256:
        parts.append("GCMP-256")
    elif cipher_flags & WG_SEC_CIPHER_GCMP_128:
        parts.append("GCMP-128")
    elif cipher_flags & WG_SEC_CIPHER_CCMP_128:
        parts.append("CCMP-128")
    elif cipher_flags & WG_SEC_CIPHER_TKIP:
        parts.append("TKIP")
    elif cipher_flags & WG_SEC_CIPHER_WEP:
        parts.append("WEP")

    return "-".join(parts)


def capability_token(proto_raw: str, akm_raw: str, cipher_raw: str) -> Optional[str]:
    proto = proto_raw.strip().upper()
    if not proto or proto == "OPEN":
        return None
    parts = [proto]
    akm = akm_raw.strip().upper()
    cipher = cipher_raw.strip().upper()
    if akm:
        parts.append(akm)
    if cipher:
        parts.append(cipher)
    return "-".join(parts)


def format_auth_mode_wigle(label: str) -> str:
    trimmed = label.strip()
    if not trimmed:
        return "[ESS]"
    if trimmed.startswith("["):
        return trimmed if "[ESS]" in trimmed else f"{trimmed}[ESS]"
    if trimmed.lower() == "open":
        return "[ESS]"

    first_dash = trimmed.find("-")
    proto_part = trimmed if first_dash < 0 else trimmed[:first_dash]
    rest = "" if first_dash < 0 else trimmed[first_dash + 1 :]
    second_dash = rest.find("-")
    akm_part = rest if second_dash < 0 else rest[:second_dash]
    cipher_part = "" if second_dash < 0 else rest[second_dash + 1 :]

    protos = [x.strip() for x in proto_part.split("/") if x.strip()]
    akms = [x.strip() for x in akm_part.split("/") if x.strip()]

    caps: list[str] = []
    if protos:
        if len(akms) == len(protos) and len(protos) > 1:
            for proto, akm in zip(protos, akms):
                t = capability_token(proto, akm, cipher_part)
                if t:
                    caps.append(t)
        elif len(akms) <= 1:
            akm = akms[0] if akms else ""
            for proto in protos:
                t = capability_token(proto, akm, cipher_part)
                if t:
                    caps.append(t)
        elif len(protos) == 1:
            for akm in akms:
                t = capability_token(protos[0], akm, cipher_part)
                if t:
                    caps.append(t)
        else:
            t = capability_token(trimmed, "", "")
            if t:
                caps.append(t)

    unique = []
    seen = set()
    for c in caps:
        if c not in seen:
            seen.add(c)
            unique.append(c)
    if not unique:
        return "[ESS]"
    return "".join(f"[{c}]" for c in unique) + "[ESS]"


def wigle_preheader(app_release: str = "wifinder-esp8266-v1") -> str:
    fields = [
        "WigleWifi-1.6",
        f"appRelease={app_release}",
        "model=ESP8266",
        "release=standalone",
        "device=nodemcu-v3",
        "display=wifinder",
        "board=esp-12e",
        "brand=espressif",
        "star=Sol",
        "body=3",
        "subBody=0",
    ]
    output = io.StringIO()
    writer = csv.writer(output)
    writer.writerow(fields)
    return output.getvalue().rstrip("\r\n")


def format_time_utc(unix_s: int) -> str:
    return dt.datetime.fromtimestamp(unix_s, tz=dt.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")


def records_to_wigle_csv(records: list[SightingRecord], app_release: str = "wifinder-esp8266-v1") -> str:
    output = io.StringIO()
    writer = csv.writer(output, lineterminator="\n")
    writer.writerow(wigle_preheader(app_release).split(","))
    writer.writerow(WIGLE_HEADER.split(","))

    for rec in records:
        label = build_security_label(rec.proto_flags, rec.akm_flags, rec.cipher_flags, rec.auth_mode)
        auth_mode = format_auth_mode_wigle(label)
        writer.writerow(
            [
                bssid_to_text(rec.bssid),
                rec.ssid,
                auth_mode,
                format_time_utc(rec.gps_unix_s),
                rec.channel,
                channel_to_frequency(rec.channel),
                rec.rssi,
                f"{rec.lat_e7 / 1e7:.7f}",
                f"{rec.lon_e7 / 1e7:.7f}",
                f"{rec.alt_mm / 1000.0:.1f}",
                f"{rec.accuracy_cm / 100.0:.1f}",
                "",
                "",
                "WIFI",
            ]
        )

    return output.getvalue()


def read_dump_from_serial(port: str, baud: int, timeout_s: float) -> bytes:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise RuntimeError("pyserial is required for --port mode; install with: pip install pyserial") from exc

    with serial.Serial(port=port, baudrate=baud, timeout=timeout_s) as ser:
        time.sleep(0.2)
        ser.reset_input_buffer()
        ser.write(b"dump\n")
        ser.flush()

        size = None
        start_deadline = time.time() + max(timeout_s, 8.0)
        while time.time() < start_deadline:
            line = ser.readline()
            if not line:
                continue
            try:
                text = line.decode("utf-8", errors="replace").strip()
            except Exception:
                continue
            if text.startswith("BEGIN_DUMP size="):
                size = int(text.split("=", 1)[1])
                break
        if size is None:
            raise RuntimeError("did not receive BEGIN_DUMP marker")

        payload = ser.read(size)
        if len(payload) != size:
            raise RuntimeError(f"incomplete dump: expected {size}, got {len(payload)} bytes")

        end_deadline = time.time() + max(timeout_s, 8.0)
        got_end = False
        while time.time() < end_deadline:
            line = ser.readline()
            if not line:
                continue
            text = line.decode("utf-8", errors="replace").strip()
            if text == "END_DUMP":
                got_end = True
                break
        if not got_end:
            raise RuntimeError("did not receive END_DUMP marker")

        return payload


def parse_blob(blob: bytes) -> tuple[LogHeader, list[SightingRecord]]:
    header = parse_header(blob)
    records = list(iter_records(blob))
    return header, records


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Serial port (for live dump), e.g. /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--timeout", type=float, default=2.0, help="Serial read timeout seconds")
    parser.add_argument("--from-bin", help="Read dump from existing binary file instead of serial")
    parser.add_argument("--bin-out", help="Where to save raw binary dump")
    parser.add_argument("--csv-out", required=True, help="Output WiGLE CSV path")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if not args.from_bin and not args.port:
        print("error: provide --port or --from-bin", file=sys.stderr)
        return 2

    if args.from_bin:
        blob = Path(args.from_bin).read_bytes()
    else:
        blob = read_dump_from_serial(args.port, args.baud, args.timeout)

    if args.bin_out:
        Path(args.bin_out).write_bytes(blob)

    _, records = parse_blob(blob)
    csv_text = records_to_wigle_csv(records)
    Path(args.csv_out).write_text(csv_text, encoding="utf-8")

    print(f"records={len(records)} csv={args.csv_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
