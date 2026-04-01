import importlib.util
import struct
import sys
import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).resolve().parents[1] / "esp8266_dump_to_wigle.py"
SPEC = importlib.util.spec_from_file_location("esp8266_dump_to_wigle", SCRIPT_PATH)
MOD = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
sys.modules[SPEC.name] = MOD
SPEC.loader.exec_module(MOD)


class DumpParserTest(unittest.TestCase):
    def _build_blob(self) -> bytes:
        header_no_crc = struct.pack(
            "<8sBB6sI",
            MOD.LOG_MAGIC,
            MOD.LOG_VERSION,
            0,
            bytes.fromhex("48A98AED1A4A"),
            1_711_234_567,
        )
        header_crc = MOD.crc16_ccitt(header_no_crc)
        header = header_no_crc + struct.pack("<H", header_crc)

        ssid = b"Red Dragon"
        fixed = MOD.RECORD_FIXED_STRUCT.pack(
            123456,  # monotonic
            1_711_234_590,  # gps unix
            377749000,  # lat_e7
            -1224194000,  # lon_e7
            15230,  # alt mm
            320,  # acc cm
            bytes.fromhex("40ED0025E002"),
            6,  # channel
            -57,  # rssi
            4,  # auth
            MOD.WG_SEC_PROTO_WPA2 | MOD.WG_SEC_PROTO_WPA3,
            MOD.WG_SEC_AKM_PSK | MOD.WG_SEC_AKM_SAE,
            MOD.WG_SEC_CIPHER_CCMP_128,
            len(ssid),
            0x03,
        )
        rec_crc = MOD.crc16_ccitt(fixed + ssid)
        record = fixed + ssid + struct.pack("<H", rec_crc)
        return header + record

    def test_parse_blob_and_csv(self) -> None:
        blob = self._build_blob()
        header, records = MOD.parse_blob(blob)
        self.assertEqual(1_711_234_567, header.session_unix_s)
        self.assertEqual(1, len(records))
        self.assertEqual("Red Dragon", records[0].ssid)
        self.assertEqual(6, records[0].channel)

        csv_text = MOD.records_to_wigle_csv(records)
        self.assertIn("MAC,SSID,AuthMode", csv_text)
        self.assertIn("40:ED:00:25:E0:02,Red Dragon", csv_text)

    def test_bad_crc_rejected(self) -> None:
        blob = bytearray(self._build_blob())
        blob[-1] ^= 0xFF
        with self.assertRaises(ValueError):
            MOD.parse_blob(bytes(blob))


if __name__ == "__main__":
    unittest.main()
