from __future__ import annotations

from dataclasses import dataclass

PROTOCOL_VERSION = 2
FRAME_HEADER_SIZE = 12
MAGIC0 = 0x57
MAGIC1 = 0x47

class _ConstMap(dict):
    def __getattr__(self, name: str) -> int:
        try:
            return self[name]
        except KeyError as exc:
            raise AttributeError(name) from exc


MESSAGE_TYPES = _ConstMap(
    STATUS=0x01,
    SIGHTING=0x02,
    ACK=0x03,
    ERROR=0x04,
    SNAPSHOT_END=0x05,
    CONFIG=0x06,
    GPS=0x07,
    REPLAY_ACK=0x08,
    NODE_TABLE=0x09,
    COMMAND=0x81,
)

COMMANDS = _ConstMap(
    START=0x01,
    STOP=0x02,
    SET_HOP_MS=0x03,
    SET_CHANNEL_MASK=0x04,
    SET_BOOT_MODE=0x05,
    REQUEST_STATUS=0x06,
    REQUEST_SNAPSHOT=0x07,
    SET_GPS_FIX=0x08,
    REPLAY_ACK=0x09,
    SET_REPLAY=0x0A,
    CLEAR_STORAGE=0x0B,
    SET_GPS_NAV_RATE=0x0C,
)

AUTH_BY_CODE = {
    0: "UNKNOWN",
    1: "OPEN",
    2: "WEP?",
    3: "WPA",
    4: "WPA2/WPA3",
}

SEC_PROTO_WPA = 1 << 0
SEC_PROTO_WPA2 = 1 << 1
SEC_PROTO_WPA3 = 1 << 2

SEC_AKM_EAP = 1 << 0
SEC_AKM_PSK = 1 << 1
SEC_AKM_SAE = 1 << 2
SEC_AKM_OWE = 1 << 3

SEC_CIPHER_TKIP = 1 << 0
SEC_CIPHER_CCMP_128 = 1 << 1
SEC_CIPHER_GCMP_128 = 1 << 2
SEC_CIPHER_GCMP_256 = 1 << 3
SEC_CIPHER_CCMP_256 = 1 << 4
SEC_CIPHER_WEP = 1 << 5

SIGHTING_SOURCE_LIVE = 1 << 0
SIGHTING_SOURCE_REPLAY = 1 << 1


@dataclass(frozen=True)
class WgFrame:
    version: int
    type: int
    seq: int
    len: int
    device_ms: int
    payload: bytes


@dataclass(frozen=True)
class StatusPayload:
    scanning: bool
    ble_encrypted: bool
    current_channel: int
    hop_ms: int
    channel_mask: int
    unique_bssids: int
    packets_per_sec: int
    dropped_notifies: int
    boot_mode: int
    gps_valid: bool
    gps_age_s: int
    gps_accuracy_dm: int
    node_link_up: bool
    node_last_seen_s: int
    node_packets_per_sec: int
    node_forwarded_sightings: int
    node_channel: int
    node_channel_mask: int
    session_open: bool
    session_id: int
    queued_records: int
    queued_bytes: int
    replay_active: bool
    replay_cursor: int
    queue_full: bool
    dropped_flash_full: int
    node_count: int


@dataclass(frozen=True)
class GpsPayload:
    valid: bool
    source: int
    lat_e7: int
    lon_e7: int
    alt_mm: int
    speed_mmps: int
    bearing_mdeg: int
    unix_time_s: int
    accuracy_cm: int
    sat_count: int
    hdop_centi: int
    pdop_centi: int


@dataclass(frozen=True)
class SightingPayload:
    bssid: str
    channel: int
    rssi: int
    auth_code: int
    proto_flags: int
    akm_flags: int
    cipher_flags: int
    auth: str
    ssid: str
    flags: int
    session_id: int
    record_seq: int
    node_id: int
    source_flags: int
    gps_valid: bool
    gps_source: int
    gps_lat_e7: int
    gps_lon_e7: int
    gps_alt_mm: int
    gps_unix_time_s: int
    gps_accuracy_cm: int


def _u16(value: int) -> bytes:
    return int(value & 0xFFFF).to_bytes(2, "little", signed=False)


def _u32(value: int) -> bytes:
    return int(value & 0xFFFFFFFF).to_bytes(4, "little", signed=False)


def _u64(value: int) -> bytes:
    return int(value & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "little", signed=False)


def encode_command_frame(
    *, seq: int, device_ms: int, command_id: int, payload: bytes = b""
) -> bytes:
    command_payload = bytes([command_id & 0xFF]) + payload
    out = bytearray(FRAME_HEADER_SIZE + len(command_payload))
    out[0] = MAGIC0
    out[1] = MAGIC1
    out[2] = PROTOCOL_VERSION
    out[3] = MESSAGE_TYPES["COMMAND"]
    out[4:6] = _u16(seq)
    out[6:8] = _u16(len(command_payload))
    out[8:12] = _u32(device_ms)
    out[12:] = command_payload
    return bytes(out)


def encode_replay_ack_frame(*, seq: int, device_ms: int, session_id: int, highest_seq: int) -> bytes:
    payload = _u64(session_id) + _u32(highest_seq)
    return encode_command_frame(
        seq=seq,
        device_ms=device_ms,
        command_id=COMMANDS.REPLAY_ACK,
        payload=payload,
    )


def encode_set_replay_frame(*, seq: int, device_ms: int, enabled: bool) -> bytes:
    payload = bytes([1 if enabled else 0])
    return encode_command_frame(
        seq=seq,
        device_ms=device_ms,
        command_id=COMMANDS.SET_REPLAY,
        payload=payload,
    )


def encode_clear_storage_frame(*, seq: int, device_ms: int) -> bytes:
    return encode_command_frame(
        seq=seq,
        device_ms=device_ms,
        command_id=COMMANDS.CLEAR_STORAGE,
    )


def decode_frame(frame_bytes: bytes) -> WgFrame:
    if len(frame_bytes) < FRAME_HEADER_SIZE:
        raise ValueError("Frame too short")
    if frame_bytes[0] != MAGIC0 or frame_bytes[1] != MAGIC1:
        raise ValueError("Bad frame magic")

    version = frame_bytes[2]
    msg_type = frame_bytes[3]
    seq = int.from_bytes(frame_bytes[4:6], "little")
    payload_len = int.from_bytes(frame_bytes[6:8], "little")
    device_ms = int.from_bytes(frame_bytes[8:12], "little")
    expected = FRAME_HEADER_SIZE + payload_len
    if len(frame_bytes) < expected:
        raise ValueError("Truncated frame")
    payload = frame_bytes[FRAME_HEADER_SIZE:expected]
    return WgFrame(
        version=version,
        type=msg_type,
        seq=seq,
        len=payload_len,
        device_ms=device_ms,
        payload=payload,
    )


def decode_status_payload(payload: bytes) -> StatusPayload:
    if len(payload) < 14:
        raise ValueError("Bad status payload")
    gps_valid = payload[14] == 1 if len(payload) >= 15 else False
    gps_age_s = int.from_bytes(payload[15:17], "little") if len(payload) >= 17 else 0
    gps_accuracy_dm = int.from_bytes(payload[17:19], "little") if len(payload) >= 19 else 0
    node_link_up = payload[19] == 1 if len(payload) >= 20 else False
    node_last_seen_s = int.from_bytes(payload[20:22], "little") if len(payload) >= 22 else 0
    node_packets_per_sec = (
        int.from_bytes(payload[22:24], "little") if len(payload) >= 24 else 0
    )
    node_forwarded_sightings = (
        int.from_bytes(payload[24:26], "little") if len(payload) >= 26 else 0
    )
    node_channel = payload[26] if len(payload) >= 27 else 0
    node_channel_mask = int.from_bytes(payload[27:29], "little") if len(payload) >= 29 else 0
    session_open = payload[29] == 1 if len(payload) >= 30 else False
    session_id = int.from_bytes(payload[30:38], "little") if len(payload) >= 38 else 0
    queued_records = int.from_bytes(payload[38:42], "little") if len(payload) >= 42 else 0
    queued_bytes = int.from_bytes(payload[42:46], "little") if len(payload) >= 46 else 0
    replay_active = payload[46] == 1 if len(payload) >= 47 else False
    replay_cursor = int.from_bytes(payload[47:51], "little") if len(payload) >= 51 else 0
    queue_full = payload[51] == 1 if len(payload) >= 52 else False
    dropped_flash_full = int.from_bytes(payload[52:56], "little") if len(payload) >= 56 else 0
    node_count = payload[56] if len(payload) >= 57 else 0
    return StatusPayload(
        scanning=payload[0] == 1,
        ble_encrypted=payload[1] == 1,
        current_channel=payload[2],
        hop_ms=int.from_bytes(payload[3:5], "little"),
        channel_mask=int.from_bytes(payload[5:7], "little"),
        unique_bssids=int.from_bytes(payload[7:9], "little"),
        packets_per_sec=int.from_bytes(payload[9:11], "little"),
        dropped_notifies=int.from_bytes(payload[11:13], "little"),
        boot_mode=payload[13],
        gps_valid=gps_valid,
        gps_age_s=gps_age_s,
        gps_accuracy_dm=gps_accuracy_dm,
        node_link_up=node_link_up,
        node_last_seen_s=node_last_seen_s,
        node_packets_per_sec=node_packets_per_sec,
        node_forwarded_sightings=node_forwarded_sightings,
        node_channel=node_channel,
        node_channel_mask=node_channel_mask,
        session_open=session_open,
        session_id=session_id,
        queued_records=queued_records,
        queued_bytes=queued_bytes,
        replay_active=replay_active,
        replay_cursor=replay_cursor,
        queue_full=queue_full,
        dropped_flash_full=dropped_flash_full,
        node_count=node_count,
    )


def decode_gps_payload(payload: bytes) -> GpsPayload:
    if len(payload) < 28:
        raise ValueError("Bad gps payload")
    return GpsPayload(
        valid=payload[0] == 1,
        source=payload[1],
        lat_e7=int.from_bytes(payload[2:6], "little", signed=True),
        lon_e7=int.from_bytes(payload[6:10], "little", signed=True),
        alt_mm=int.from_bytes(payload[10:14], "little", signed=True),
        speed_mmps=int.from_bytes(payload[14:18], "little"),
        bearing_mdeg=int.from_bytes(payload[18:22], "little"),
        unix_time_s=int.from_bytes(payload[22:26], "little"),
        accuracy_cm=int.from_bytes(payload[26:28], "little"),
        sat_count=payload[28] if len(payload) >= 29 else 0,
        hdop_centi=int.from_bytes(payload[29:31], "little") if len(payload) >= 31 else 0,
        pdop_centi=int.from_bytes(payload[31:33], "little") if len(payload) >= 33 else 0,
    )


def decode_sighting_payload(payload: bytes) -> SightingPayload:
    if len(payload) < 14:
        raise ValueError("Bad sighting payload")

    bssid = ":".join(f"{x:02X}" for x in payload[0:6])
    channel = payload[6]
    rssi = payload[7] if payload[7] < 128 else payload[7] - 256
    auth_code = payload[8]
    proto_flags = payload[9]
    akm_flags = payload[10]
    cipher_flags = payload[11]
    ssid_len = min(payload[12], 32)
    expected = 14 + ssid_len
    if len(payload) < expected:
        raise ValueError("Truncated sighting payload")
    ssid = payload[13 : 13 + ssid_len].decode("utf-8", errors="replace")
    flags = payload[13 + ssid_len]
    session_id = 0
    record_seq = 0
    node_id = 0
    source_flags = 0
    gps_valid = False
    gps_source = 0
    gps_lat_e7 = 0
    gps_lon_e7 = 0
    gps_alt_mm = 0
    gps_unix_time_s = 0
    gps_accuracy_cm = 0
    trailer_offset = 14 + ssid_len
    if len(payload) >= trailer_offset + 14:
        session_id = int.from_bytes(payload[trailer_offset : trailer_offset + 8], "little")
        record_seq = int.from_bytes(payload[trailer_offset + 8 : trailer_offset + 12], "little")
        node_id = payload[trailer_offset + 12]
        source_flags = payload[trailer_offset + 13]
    gps_offset = trailer_offset + 14
    if len(payload) >= gps_offset + 20:
        gps_valid = payload[gps_offset] == 1
        gps_source = payload[gps_offset + 1]
        gps_lat_e7 = int.from_bytes(payload[gps_offset + 2 : gps_offset + 6], "little", signed=True)
        gps_lon_e7 = int.from_bytes(payload[gps_offset + 6 : gps_offset + 10], "little", signed=True)
        gps_alt_mm = int.from_bytes(payload[gps_offset + 10 : gps_offset + 14], "little", signed=True)
        gps_unix_time_s = int.from_bytes(payload[gps_offset + 14 : gps_offset + 18], "little")
        gps_accuracy_cm = int.from_bytes(payload[gps_offset + 18 : gps_offset + 20], "little")
    auth = format_security(auth_code, proto_flags, akm_flags, cipher_flags)
    return SightingPayload(
        bssid=bssid,
        channel=channel,
        rssi=rssi,
        auth_code=auth_code,
        proto_flags=proto_flags,
        akm_flags=akm_flags,
        cipher_flags=cipher_flags,
        auth=auth,
        ssid=ssid,
        flags=flags,
        session_id=session_id,
        record_seq=record_seq,
        node_id=node_id,
        source_flags=source_flags,
        gps_valid=gps_valid,
        gps_source=gps_source,
        gps_lat_e7=gps_lat_e7,
        gps_lon_e7=gps_lon_e7,
        gps_alt_mm=gps_alt_mm,
        gps_unix_time_s=gps_unix_time_s,
        gps_accuracy_cm=gps_accuracy_cm,
    )


def format_security(auth_code: int, proto_flags: int, akm_flags: int, cipher_flags: int) -> str:
    parts: list[str] = []

    if (proto_flags & (SEC_PROTO_WPA2 | SEC_PROTO_WPA3)) == (SEC_PROTO_WPA2 | SEC_PROTO_WPA3):
        parts.append("WPA2/WPA3")
    elif proto_flags & SEC_PROTO_WPA3:
        parts.append("WPA3")
    elif proto_flags & SEC_PROTO_WPA2:
        parts.append("WPA2")
    elif proto_flags & SEC_PROTO_WPA:
        parts.append("WPA")
    elif auth_code == 2 or (cipher_flags & SEC_CIPHER_WEP):
        parts.append("WEP")
    elif auth_code == 1:
        parts.append("OPEN")
    else:
        return AUTH_BY_CODE.get(auth_code, "UNKNOWN")

    if (akm_flags & (SEC_AKM_PSK | SEC_AKM_SAE)) == (SEC_AKM_PSK | SEC_AKM_SAE):
        parts.append("PSK/SAE")
    elif akm_flags & SEC_AKM_SAE:
        parts.append("SAE")
    elif akm_flags & SEC_AKM_PSK:
        parts.append("PSK")
    elif akm_flags & SEC_AKM_EAP:
        parts.append("EAP")
    elif akm_flags & SEC_AKM_OWE:
        parts.append("OWE")

    if cipher_flags & SEC_CIPHER_CCMP_256:
        parts.append("CCMP-256")
    elif cipher_flags & SEC_CIPHER_GCMP_256:
        parts.append("GCMP-256")
    elif cipher_flags & SEC_CIPHER_GCMP_128:
        parts.append("GCMP-128")
    elif cipher_flags & SEC_CIPHER_CCMP_128:
        parts.append("CCMP-128")
    elif cipher_flags & SEC_CIPHER_TKIP:
        parts.append("TKIP")
    elif cipher_flags & SEC_CIPHER_WEP:
        parts.append("WEP")

    return "-".join(parts)


class FrameStreamDecoder:
    """Byte stream parser that resynchronizes on WG magic."""

    def __init__(self, max_payload: int = 196) -> None:
        self._buf = bytearray()
        self._max_payload = max_payload

    def feed(self, data: bytes) -> list[WgFrame]:
        if not data:
            return []
        self._buf.extend(data)
        out: list[WgFrame] = []
        while True:
            if len(self._buf) < 2:
                break
            if self._buf[0] != MAGIC0 or self._buf[1] != MAGIC1:
                del self._buf[0]
                continue
            if len(self._buf) < FRAME_HEADER_SIZE:
                break
            payload_len = int.from_bytes(self._buf[6:8], "little")
            if payload_len > self._max_payload:
                del self._buf[0]
                continue
            frame_len = FRAME_HEADER_SIZE + payload_len
            if len(self._buf) < frame_len:
                break
            raw = bytes(self._buf[:frame_len])
            del self._buf[:frame_len]
            try:
                out.append(decode_frame(raw))
            except ValueError:
                # Robustness fallback: continue scanning.
                continue
        return out
