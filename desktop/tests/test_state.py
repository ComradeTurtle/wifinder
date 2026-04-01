import unittest
from pathlib import Path
import sys

_DESKTOP_ROOT = Path(__file__).resolve().parents[1]
if str(_DESKTOP_ROOT) not in sys.path:
    sys.path.insert(0, str(_DESKTOP_ROOT))

from espwigle_desktop.state import ConsoleState


class StateTests(unittest.TestCase):
    def test_ssid_memory_prevents_blank_overwrite(self) -> None:
        state = ConsoleState()
        state.apply_sighting(
            bssid="40:ED:00:25:E0:02",
            ssid="Red Dragon",
            auth="WPA2-PSK-CCMP-128",
            channel=1,
            rssi=-45,
            seen_ms=1_000,
        )
        state.apply_sighting(
            bssid="40:ED:00:25:E0:02",
            ssid="",
            auth="WPA2-PSK-CCMP-128",
            channel=1,
            rssi=-46,
            seen_ms=1_500,
        )
        rows = state.to_rows(now_ms=2_000)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].ssid, "Red Dragon")
        self.assertEqual(rows[0].seen_count, 2)

    def test_prune_removes_stale_entries(self) -> None:
        state = ConsoleState()
        state.apply_sighting("A", "a", "OPEN", 1, -50, 1_000)
        state.apply_sighting("B", "b", "OPEN", 1, -50, 10_000)
        removed = state.prune(now_ms=12_000, max_age_ms=5_000)
        self.assertEqual(removed, 1)
        self.assertEqual(len(state.sightings), 1)
        self.assertIn("B", state.sightings)

    def test_rows_sorted_newest_first(self) -> None:
        state = ConsoleState()
        state.apply_sighting("A", "a", "OPEN", 1, -50, 2_000)
        state.apply_sighting("B", "b", "OPEN", 1, -60, 4_000)
        rows = state.to_rows(now_ms=5_000)
        self.assertEqual([r.bssid for r in rows], ["B", "A"])
        self.assertEqual(rows[0].age_ms, 1_000)
        self.assertEqual(rows[1].age_ms, 3_000)


if __name__ == "__main__":
    unittest.main()
