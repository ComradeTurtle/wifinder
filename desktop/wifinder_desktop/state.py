from __future__ import annotations

from dataclasses import dataclass


@dataclass
class SightingRow:
    bssid: str
    ssid: str
    auth: str
    channel: int
    rssi: int
    seen_ms: int
    first_seen_ms: int
    seen_count: int
    age_ms: int = 0


class ConsoleState:
    def __init__(self) -> None:
        self.sightings: dict[str, SightingRow] = {}
        self.ssid_memory: dict[str, str] = {}

    def apply_sighting(
        self,
        bssid: str,
        ssid: str,
        auth: str,
        channel: int,
        rssi: int,
        seen_ms: int,
    ) -> None:
        existing = self.sightings.get(bssid)
        prev_ssid = (existing.ssid if existing else "") or self.ssid_memory.get(bssid, "")
        if ssid:
            self.ssid_memory[bssid] = ssid
        remembered = self.ssid_memory.get(bssid, "")
        next_ssid = remembered or prev_ssid or ssid
        first_seen = existing.first_seen_ms if existing else seen_ms
        seen_count = (existing.seen_count if existing else 0) + 1
        self.sightings[bssid] = SightingRow(
            bssid=bssid,
            ssid=next_ssid,
            auth=auth,
            channel=channel,
            rssi=rssi,
            seen_ms=seen_ms,
            first_seen_ms=first_seen,
            seen_count=seen_count,
            age_ms=0,
        )

    def prune(self, now_ms: int, max_age_ms: int) -> int:
        removed = 0
        stale = []
        for bssid, row in self.sightings.items():
            if now_ms - row.seen_ms > max_age_ms:
                stale.append(bssid)
        for bssid in stale:
            del self.sightings[bssid]
            removed += 1
        return removed

    def to_rows(self, now_ms: int, max_age_ms: int | None = None) -> list[SightingRow]:
        rows = []
        for row in self.sightings.values():
            age = max(0, now_ms - row.seen_ms)
            if max_age_ms is not None and age > max_age_ms:
                continue
            rows.append(
                SightingRow(
                    bssid=row.bssid,
                    ssid=row.ssid,
                    auth=row.auth,
                    channel=row.channel,
                    rssi=row.rssi,
                    seen_ms=row.seen_ms,
                    first_seen_ms=row.first_seen_ms,
                    seen_count=row.seen_count,
                    age_ms=age,
                )
            )
        rows.sort(key=lambda r: r.seen_ms, reverse=True)
        return rows

