import unittest
from pathlib import Path
import sys

_DESKTOP_ROOT = Path(__file__).resolve().parents[1]
if str(_DESKTOP_ROOT) not in sys.path:
    sys.path.insert(0, str(_DESKTOP_ROOT))

from wifinder_desktop.protocol import (
    COMMANDS,
    FRAME_HEADER_SIZE,
    MESSAGE_TYPES,
    PROTOCOL_VERSION,
    FrameStreamDecoder,
    decode_frame,
    decode_gps_payload,
    decode_sighting_payload,
    decode_status_payload,
    encode_clear_storage_frame,
    encode_set_replay_frame,
    encode_replay_ack_frame,
    encode_command_frame,
)


class ProtocolTests(unittest.TestCase):
    def test_encode_decode_roundtrip(self) -> None:
        frame = encode_command_frame(seq=7, device_ms=12345, command_id=COMMANDS.START)
        decoded = decode_frame(frame)
        self.assertEqual(decoded.version, PROTOCOL_VERSION)
        self.assertEqual(decoded.type, MESSAGE_TYPES.COMMAND)
        self.assertEqual(decoded.seq, 7)
        self.assertEqual(decoded.device_ms, 12345)
        self.assertEqual(decoded.payload, bytes([COMMANDS.START]))

    def test_stream_decoder_handles_noise_and_chunks(self) -> None:
        frame = encode_command_frame(seq=1, device_ms=99, command_id=COMMANDS.REQUEST_STATUS)
        decoder = FrameStreamDecoder()
        out = []
        out.extend(decoder.feed(b"junk"))
        out.extend(decoder.feed(frame[:3]))
        out.extend(decoder.feed(frame[3:9]))
        out.extend(decoder.feed(frame[9:]))
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].type, MESSAGE_TYPES.COMMAND)
        self.assertEqual(out[0].len, 1)

    def test_decode_status_payload_extended(self) -> None:
        payload = bytearray(29)
        payload[0] = 1
        payload[1] = 0
        payload[2] = 11
        payload[3:5] = (250).to_bytes(2, "little")
        payload[5:7] = (0x1FFF).to_bytes(2, "little")
        payload[7:9] = (333).to_bytes(2, "little")
        payload[9:11] = (44).to_bytes(2, "little")
        payload[11:13] = (5).to_bytes(2, "little")
        payload[13] = 1
        payload[14] = 1
        payload[15:17] = (2).to_bytes(2, "little")
        payload[17:19] = (12).to_bytes(2, "little")
        payload[19] = 1
        payload[20:22] = (3).to_bytes(2, "little")
        payload[22:24] = (123).to_bytes(2, "little")
        payload[24:26] = (9).to_bytes(2, "little")
        payload[26] = 6
        payload[27:29] = (0x0AAA).to_bytes(2, "little")
        status = decode_status_payload(bytes(payload))
        self.assertTrue(status.scanning)
        self.assertEqual(status.current_channel, 11)
        self.assertTrue(status.gps_valid)
        self.assertTrue(status.node_link_up)
        self.assertEqual(status.node_channel, 6)
        self.assertEqual(status.node_channel_mask, 0x0AAA)

    def test_decode_gps_payload(self) -> None:
        payload = bytearray(33)
        payload[0] = 1
        payload[1] = 1
        payload[2:6] = int(391334180).to_bytes(4, "little", signed=True)
        payload[6:10] = int(209626470).to_bytes(4, "little", signed=True)
        payload[10:14] = int(12345).to_bytes(4, "little", signed=True)
        payload[14:18] = (15000).to_bytes(4, "little")
        payload[18:22] = (90000).to_bytes(4, "little")
        payload[22:26] = (1711660000).to_bytes(4, "little")
        payload[26:28] = (230).to_bytes(2, "little")
        payload[28] = 8
        payload[29:31] = (517).to_bytes(2, "little")
        payload[31:33] = (613).to_bytes(2, "little")
        gps = decode_gps_payload(bytes(payload))
        self.assertTrue(gps.valid)
        self.assertEqual(gps.sat_count, 8)
        self.assertEqual(gps.hdop_centi, 517)
        self.assertEqual(gps.pdop_centi, 613)

    def test_decode_sighting_payload(self) -> None:
        payload = bytearray()
        payload.extend(bytes.fromhex("40 ED 00 25 E0 02"))
        payload.append(1)  # channel
        payload.append((256 - 44) & 0xFF)  # -44 dBm
        payload.append(4)  # auth code
        payload.append(0b00000110)  # WPA2/WPA3
        payload.append(0b00000110)  # PSK+SAE
        payload.append(0b00000010)  # CCMP-128
        ssid = b"Red Dragon"
        payload.append(len(ssid))
        payload.extend(ssid)
        payload.append(0x01)
        payload.extend((0x0123456789ABCDEF).to_bytes(8, "little"))
        payload.extend((42).to_bytes(4, "little"))
        payload.append(1)
        payload.append(0x01)
        payload.append(0x01)  # gps valid
        payload.append(0x01)  # gps source uart
        payload.extend((391333890).to_bytes(4, "little", signed=True))
        payload.extend((209626726).to_bytes(4, "little", signed=True))
        payload.extend((22300).to_bytes(4, "little", signed=True))
        payload.extend((1711565786).to_bytes(4, "little", signed=False))
        payload.extend((1200).to_bytes(2, "little", signed=False))
        sighting = decode_sighting_payload(bytes(payload))
        self.assertEqual(sighting.bssid, "40:ED:00:25:E0:02")
        self.assertEqual(sighting.ssid, "Red Dragon")
        self.assertEqual(sighting.channel, 1)
        self.assertEqual(sighting.rssi, -44)
        self.assertIn("WPA2", sighting.auth)
        self.assertIn("CCMP-128", sighting.auth)
        self.assertEqual(sighting.session_id, 0x0123456789ABCDEF)
        self.assertEqual(sighting.record_seq, 42)
        self.assertEqual(sighting.node_id, 1)
        self.assertEqual(sighting.source_flags, 0x01)
        self.assertTrue(sighting.gps_valid)
        self.assertEqual(sighting.gps_source, 1)
        self.assertEqual(sighting.gps_lat_e7, 391333890)
        self.assertEqual(sighting.gps_lon_e7, 209626726)
        self.assertEqual(sighting.gps_alt_mm, 22300)
        self.assertEqual(sighting.gps_unix_time_s, 1711565786)
        self.assertEqual(sighting.gps_accuracy_cm, 1200)

    def test_decode_sighting_payload_legacy_without_gps_extension(self) -> None:
        payload = bytearray()
        payload.extend(bytes.fromhex("48 A9 8A ED 1A 4A"))
        payload.append(6)
        payload.append((256 - 57) & 0xFF)
        payload.append(4)
        payload.append(0b00000110)
        payload.append(0b00000110)
        payload.append(0b00000010)
        ssid = b"Legacy"
        payload.append(len(ssid))
        payload.extend(ssid)
        payload.append(0x01)
        payload.extend((0x0123456789ABCDEF).to_bytes(8, "little"))
        payload.extend((42).to_bytes(4, "little"))
        payload.append(1)
        payload.append(0x01)

        sighting = decode_sighting_payload(bytes(payload))
        self.assertFalse(sighting.gps_valid)
        self.assertEqual(sighting.gps_source, 0)

    def test_encode_replay_ack(self) -> None:
        frame = encode_replay_ack_frame(
            seq=11,
            device_ms=555,
            session_id=0x1122334455667788,
            highest_seq=77,
        )
        decoded = decode_frame(frame)
        self.assertEqual(decoded.type, MESSAGE_TYPES.COMMAND)
        self.assertEqual(decoded.payload[0], COMMANDS.REPLAY_ACK)
        self.assertEqual(int.from_bytes(decoded.payload[1:9], "little"), 0x1122334455667788)
        self.assertEqual(int.from_bytes(decoded.payload[9:13], "little"), 77)

    def test_encode_set_replay(self) -> None:
        frame = encode_set_replay_frame(seq=12, device_ms=777, enabled=True)
        decoded = decode_frame(frame)
        self.assertEqual(decoded.type, MESSAGE_TYPES.COMMAND)
        self.assertEqual(decoded.payload[0], COMMANDS.SET_REPLAY)
        self.assertEqual(decoded.payload[1], 1)

    def test_encode_clear_storage(self) -> None:
        frame = encode_clear_storage_frame(seq=13, device_ms=888)
        decoded = decode_frame(frame)
        self.assertEqual(decoded.type, MESSAGE_TYPES.COMMAND)
        self.assertEqual(decoded.payload, bytes([COMMANDS.CLEAR_STORAGE]))

    def test_frame_header_size_constant(self) -> None:
        self.assertEqual(FRAME_HEADER_SIZE, 12)


if __name__ == "__main__":
    unittest.main()
