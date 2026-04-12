import tempfile
import unittest
from pathlib import Path
import sys

_DESKTOP_ROOT = Path(__file__).resolve().parents[1]
if str(_DESKTOP_ROOT) not in sys.path:
    sys.path.insert(0, str(_DESKTOP_ROOT))

from wifinder_desktop.wigle_csv import WigleCsvFormatter, WigleCsvLogger, WigleWifiRow


class WigleCsvTests(unittest.TestCase):
    def test_header_and_auth_format(self) -> None:
        header = WigleCsvFormatter.header(app_release="wifinder-desktop-1")
        self.assertEqual(len(header), 2)
        self.assertTrue(header[0].startswith("WigleWifi-1.6"))
        self.assertEqual(
            header[1],
            "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type",
        )
        auth = WigleCsvFormatter.format_auth_mode("WPA2/WPA3-PSK/SAE-CCMP-128")
        self.assertIn("[WPA2-PSK-CCMP-128]", auth)
        self.assertIn("[WPA3-SAE-CCMP-128]", auth)
        self.assertTrue(auth.endswith("[ESS]"))

    def test_logger_writes_rows(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "wigle-test.csv"
            logger = WigleCsvLogger()
            logger.start_session(path, app_release="wifinder-desktop-1")
            row = WigleWifiRow(
                mac="40:ED:00:25:E0:02",
                ssid="Red Dragon",
                auth_mode="WPA2-PSK-CCMP-128",
                first_seen_epoch_ms=1_711_660_000_000,
                channel=1,
                rssi=-44,
                latitude=39.133389,
                longitude=20.962672,
                altitude_meters=22.3,
                accuracy_meters=2.3,
            )
            logger.append(row)
            logger.stop_session()
            text = path.read_text(encoding="utf-8")
            self.assertIn("WigleWifi-1.6", text)
            self.assertIn("40:ED:00:25:E0:02,Red Dragon", text)
            self.assertIn("WIFI", text)


if __name__ == "__main__":
    unittest.main()
