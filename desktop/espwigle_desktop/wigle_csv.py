from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import TextIO

WIFI_HEADER = (
    "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,"
    "CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type"
)


def _channel_to_frequency(channel: int) -> int:
    if 1 <= channel <= 13:
        return 2407 + channel * 5
    if channel == 14:
        return 2484
    if 32 <= channel <= 177:
        return 5000 + channel * 5
    return 0


def _csv_escape(value: str) -> str:
    if "," in value or '"' in value or "\n" in value:
        return '"' + value.replace('"', '""') + '"'
    return value


@dataclass
class WigleWifiRow:
    mac: str
    ssid: str
    auth_mode: str
    first_seen_epoch_ms: int
    channel: int
    rssi: int
    latitude: float
    longitude: float
    altitude_meters: float
    accuracy_meters: float
    frequency: int | None = None
    rcois: str = ""
    mfgr_id: str = ""
    type: str = "WIFI"


class WigleCsvFormatter:
    @staticmethod
    def header(
        app_release: str,
        model: str = "unknown",
        release: str = "unknown",
        device: str = "unknown",
        display: str = "unknown",
        board: str = "unknown",
        brand: str = "unknown",
    ) -> list[str]:
        pre = [
            "WigleWifi-1.6",
            f"appRelease={app_release}",
            f"model={model}",
            f"release={release}",
            f"device={device}",
            f"display={display}",
            f"board={board}",
            f"brand={brand}",
            "star=Sol",
            "body=3",
            "subBody=0",
        ]
        preline = ",".join(_csv_escape(x) for x in pre)
        return [preline, WIFI_HEADER]

    @staticmethod
    def format_auth_mode(auth: str) -> str:
        trimmed = auth.strip()
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

        protos = [p.strip().upper() for p in proto_part.split("/") if p.strip()]
        akms = [a.strip().upper() for a in akm_part.split("/") if a.strip()]
        cipher = cipher_part.strip().upper()

        caps: list[str] = []
        if protos:
            if len(akms) == len(protos) and len(protos) > 1:
                for i, proto in enumerate(protos):
                    token = WigleCsvFormatter._capability_token(proto, akms[i], cipher)
                    if token:
                        caps.append(token)
            elif len(akms) <= 1:
                akm = akms[0] if akms else ""
                for proto in protos:
                    token = WigleCsvFormatter._capability_token(proto, akm, cipher)
                    if token:
                        caps.append(token)
            elif len(protos) == 1:
                for akm in akms:
                    token = WigleCsvFormatter._capability_token(protos[0], akm, cipher)
                    if token:
                        caps.append(token)

        unique = list(dict.fromkeys(caps))
        if not unique:
            return "[ESS]"
        return "".join(f"[{x}]" for x in unique) + "[ESS]"

    @staticmethod
    def _capability_token(proto: str, akm: str, cipher: str) -> str | None:
        if not proto or proto == "OPEN":
            return None
        out = [proto]
        if akm:
            out.append(akm)
        if cipher:
            out.append(cipher)
        return "-".join(out)

    @staticmethod
    def format_row(row: WigleWifiRow) -> str:
        seen = datetime.fromtimestamp(row.first_seen_epoch_ms / 1000.0, tz=timezone.utc)
        seen_str = seen.strftime("%Y-%m-%d %H:%M:%S")
        freq = row.frequency if row.frequency is not None else _channel_to_frequency(row.channel)
        values = [
            _csv_escape(row.mac),
            _csv_escape(row.ssid),
            _csv_escape(WigleCsvFormatter.format_auth_mode(row.auth_mode)),
            _csv_escape(seen_str),
            str(row.channel),
            str(freq),
            str(row.rssi),
            f"{row.latitude:.7f}",
            f"{row.longitude:.7f}",
            f"{row.altitude_meters:.1f}",
            f"{row.accuracy_meters:.1f}",
            _csv_escape(row.rcois),
            _csv_escape(row.mfgr_id),
            _csv_escape(row.type),
        ]
        return ",".join(values)


class WigleCsvLogger:
    def __init__(self) -> None:
        self._f: TextIO | None = None
        self._path: Path | None = None

    @property
    def is_active(self) -> bool:
        return self._f is not None

    @property
    def current_path(self) -> Path | None:
        return self._path

    def start_session(self, path: Path, app_release: str) -> None:
        self.stop_session()
        path.parent.mkdir(parents=True, exist_ok=True)
        self._f = path.open("w", encoding="utf-8", newline="")
        self._path = path
        for line in WigleCsvFormatter.header(app_release=app_release):
            self._f.write(line + "\n")
        self._f.flush()

    def append(self, row: WigleWifiRow) -> None:
        if self._f is None:
            return
        self._f.write(WigleCsvFormatter.format_row(row))
        self._f.write("\n")
        self._f.flush()

    def stop_session(self) -> None:
        if self._f is not None:
            self._f.flush()
            self._f.close()
            self._f = None
        self._path = None
