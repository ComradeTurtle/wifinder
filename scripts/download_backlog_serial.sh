#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PORT="/dev/ttyACM0"
BAUD="115200"
ACK_BATCH="32"
STATUS_INTERVAL_SEC="1.0"
IDLE_TIMEOUT_SEC="20.0"
OUT_DIR=""

usage() {
  cat <<'EOF'
Usage: scripts/download_backlog_serial.sh [options]

Downloads backlog from ESP over USB serial (WG framed protocol), writes CSV, and
ACKs replay records so sessions can be reclaimed on device.

Options:
  -p, --port <path>           Serial port (default: /dev/ttyACM0)
  -b, --baud <rate>           Serial baud (default: 115200)
  -o, --out-dir <dir>         Output directory (default: ./downloads/serial-backlog-YYYYmmdd-HHMMSS)
      --ack-batch <n>         Replay ACK batch size (default: 32)
      --status-interval <s>   Status poll interval seconds (default: 1.0)
      --idle-timeout <s>      Exit timeout after inactivity when done (default: 20.0)
  -h, --help                  Show help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -p|--port)
      PORT="$2"
      shift 2
      ;;
    -b|--baud)
      BAUD="$2"
      shift 2
      ;;
    -o|--out-dir)
      OUT_DIR="$2"
      shift 2
      ;;
    --ack-batch)
      ACK_BATCH="$2"
      shift 2
      ;;
    --status-interval)
      STATUS_INTERVAL_SEC="$2"
      shift 2
      ;;
    --idle-timeout)
      IDLE_TIMEOUT_SEC="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ -z "${OUT_DIR}" ]]; then
  OUT_DIR="${REPO_ROOT}/downloads/serial-backlog-$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "${OUT_DIR}"

python3 - "$REPO_ROOT" "$PORT" "$BAUD" "$OUT_DIR" "$ACK_BATCH" "$STATUS_INTERVAL_SEC" "$IDLE_TIMEOUT_SEC" <<'PY'
import csv
import glob
import os
import struct
import sys
import time
from dataclasses import dataclass, field

repo_root, port, baud, out_dir, ack_batch, status_interval_s, idle_timeout_s = sys.argv[1:]
baud_i = int(baud)
ack_batch_i = max(1, int(ack_batch))
status_interval = max(0.2, float(status_interval_s))
idle_timeout = max(3.0, float(idle_timeout_s))

sys.path.insert(0, repo_root)

try:
    import serial  # type: ignore
    from serial.tools import list_ports  # type: ignore
except Exception:
    print("Missing dependency: pyserial", file=sys.stderr)
    print("Install with: python3 -m pip install --user pyserial", file=sys.stderr)
    sys.exit(3)

from desktop.wifinder_desktop.protocol import (  # type: ignore
    COMMANDS,
    FRAME_HEADER_SIZE,
    MAGIC0,
    MAGIC1,
    MESSAGE_TYPES,
    PROTOCOL_VERSION,
    SIGHTING_SOURCE_REPLAY,
    FrameStreamDecoder,
    decode_sighting_payload,
    decode_status_payload,
)


def open_serial_exclusive(path: str, baudrate: int, timeout: float):
    kwargs = dict(port=path, baudrate=baudrate, timeout=timeout)
    try:
        return serial.Serial(exclusive=True, **kwargs)
    except TypeError:
        # Older pyserial without 'exclusive' kwarg.
        return serial.Serial(**kwargs)


@dataclass
class AckTracker:
    highest_contiguous: int = 0
    last_sent: int = 0
    gaps: set[int] = field(default_factory=set)


class Client:
    def __init__(self, ser: serial.Serial):
        self.ser = ser
        self.seq = 1
        self.proto_version = int(PROTOCOL_VERSION)
        self.decoder = FrameStreamDecoder(max_payload=244)
        self.trackers: dict[int, AckTracker] = {}
        self.last_status = None
        self.last_rx_time = time.monotonic()
        self.last_status_req = 0.0
        self.replay_seen = False
        self.rows_written = 0
        self.acks_sent = 0
        self.session_counts: dict[int, int] = {}
        self.error_count = 0
        self.rx_bytes = 0
        self.frames_seen = 0

    def _next_seq(self) -> int:
        v = self.seq
        self.seq += 1
        if self.seq > 0xFFFF:
            self.seq = 1
        return v

    def send_cmd(self, cmd_id: int, payload: bytes = b"") -> None:
        command_payload = bytes([cmd_id & 0xFF]) + payload
        seq = self._next_seq()
        frame = bytearray(FRAME_HEADER_SIZE + len(command_payload))
        frame[0] = MAGIC0
        frame[1] = MAGIC1
        frame[2] = self.proto_version & 0xFF
        frame[3] = MESSAGE_TYPES.COMMAND & 0xFF
        frame[4:6] = (seq & 0xFFFF).to_bytes(2, "little")
        frame[6:8] = (len(command_payload) & 0xFFFF).to_bytes(2, "little")
        frame[8:12] = (int(time.monotonic() * 1000) & 0xFFFFFFFF).to_bytes(4, "little")
        frame[12:] = command_payload
        self.ser.write(frame)
        self.ser.flush()

    def send_replay_ack(self, session_id: int, highest_seq: int) -> None:
        payload = struct.pack("<QI", session_id & 0xFFFFFFFFFFFFFFFF, highest_seq & 0xFFFFFFFF)
        self.send_cmd(COMMANDS.REPLAY_ACK, payload)
        self.acks_sent += 1

    def _track_and_maybe_ack(self, session_id: int, record_seq: int, force: bool = False) -> None:
        if session_id == 0 or record_seq == 0:
            return
        tr = self.trackers.setdefault(session_id, AckTracker())
        h = tr.highest_contiguous
        if record_seq <= h:
            pass
        elif record_seq == h + 1:
            tr.highest_contiguous = record_seq
            while (tr.highest_contiguous + 1) in tr.gaps:
                tr.gaps.remove(tr.highest_contiguous + 1)
                tr.highest_contiguous += 1
        else:
            tr.gaps.add(record_seq)

        delta = tr.highest_contiguous - tr.last_sent
        if tr.highest_contiguous > tr.last_sent and (force or delta >= ack_batch_i):
            self.send_replay_ack(session_id, tr.highest_contiguous)
            tr.last_sent = tr.highest_contiguous

    def flush_acks(self) -> None:
        for sid, tr in list(self.trackers.items()):
            if tr.highest_contiguous > tr.last_sent:
                self.send_replay_ack(sid, tr.highest_contiguous)
                tr.last_sent = tr.highest_contiguous

    def poll(self, csv_writer) -> None:
        data = self.ser.read(4096)
        now = time.monotonic()
        if data:
            self.last_rx_time = now
            self.rx_bytes += len(data)
        for frame in self.decoder.feed(data):
            self.frames_seen += 1
            self.last_rx_time = now
            if frame.type == MESSAGE_TYPES.STATUS:
                try:
                    self.last_status = decode_status_payload(frame.payload)
                except Exception:
                    self.error_count += 1
            elif frame.type == MESSAGE_TYPES.SIGHTING:
                try:
                    s = decode_sighting_payload(frame.payload)
                except Exception:
                    self.error_count += 1
                    continue
                if (s.source_flags & SIGHTING_SOURCE_REPLAY) == 0:
                    continue
                self.replay_seen = True
                self.rows_written += 1
                self.session_counts[s.session_id] = self.session_counts.get(s.session_id, 0) + 1
                csv_writer.writerow([
                    f"{s.session_id:016X}",
                    s.record_seq,
                    s.bssid,
                    s.ssid,
                    s.auth,
                    s.channel,
                    s.rssi,
                    s.node_id,
                    s.flags,
                    s.source_flags,
                    1 if s.gps_valid else 0,
                    s.gps_source,
                    s.gps_lat_e7,
                    s.gps_lon_e7,
                    s.gps_alt_mm,
                    s.gps_unix_time_s,
                    s.gps_accuracy_cm,
                ])
                self._track_and_maybe_ack(s.session_id, s.record_seq, force=False)
            elif frame.type == MESSAGE_TYPES.ERROR:
                self.error_count += 1

        if now - self.last_status_req >= status_interval:
            self.send_cmd(COMMANDS.REQUEST_STATUS)
            self.last_status_req = now


def status_done(st) -> bool:
    if st is None:
        return False
    queued = int(getattr(st, "queued_records", 0))
    replay_active = bool(getattr(st, "replay_active", False))
    return queued == 0 and not replay_active


def probe_port(candidate_port: str, baudrate: int, timeout_s: float = 1.8) -> bool:
    try:
        with open_serial_exclusive(candidate_port, baudrate, 0.15) as ser:
            dec = FrameStreamDecoder(max_payload=244)
            seq = 1
            proto_ver = int(PROTOCOL_VERSION)
            def send_cmd(cmd_id: int, payload: bytes = b"") -> None:
                nonlocal seq, proto_ver
                cmd_payload = bytes([cmd_id & 0xFF]) + payload
                frame = bytearray(FRAME_HEADER_SIZE + len(cmd_payload))
                frame[0] = MAGIC0
                frame[1] = MAGIC1
                frame[2] = proto_ver & 0xFF
                frame[3] = MESSAGE_TYPES.COMMAND & 0xFF
                frame[4:6] = (seq & 0xFFFF).to_bytes(2, "little")
                frame[6:8] = (len(cmd_payload) & 0xFFFF).to_bytes(2, "little")
                frame[8:12] = (int(time.monotonic() * 1000) & 0xFFFFFFFF).to_bytes(4, "little")
                frame[12:] = cmd_payload
                seq = 1 if seq >= 0xFFFF else seq + 1
                ser.write(frame)
                ser.flush()

            send_cmd(COMMANDS.REQUEST_STATUS)
            t_end = time.monotonic() + timeout_s
            while time.monotonic() < t_end:
                data = ser.read(4096)
                for fr in dec.feed(data):
                    if fr.type in (
                        MESSAGE_TYPES.STATUS,
                        MESSAGE_TYPES.ACK,
                        MESSAGE_TYPES.ERROR,
                        MESSAGE_TYPES.SIGHTING,
                        MESSAGE_TYPES.REPLAY_ACK,
                        MESSAGE_TYPES.NODE_TABLE,
                    ):
                        return True
    except Exception:
        return False
    return False


def resolve_port(preferred_port: str, baudrate: int) -> str:
    if probe_port(preferred_port, baudrate):
        return preferred_port

    candidates = sorted(set(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*")))
    ordered = [p for p in candidates if p != preferred_port]
    for cand in ordered:
        if probe_port(cand, baudrate):
            print(
                f"[serial-backlog] no WG frames on {preferred_port}; switched to detected WG port {cand}",
                flush=True,
            )
            return cand

    print(
        f"[serial-backlog] No immediate WG framed response on {preferred_port}; continuing with that port and extended handshake",
        file=sys.stderr,
    )
    return preferred_port


csv_path = os.path.join(out_dir, "backlog_serial.csv")
ports = sorted(list_ports.comports(), key=lambda p: p.device)
if ports:
    print("[serial-backlog] detected serial ports:")
    for p in ports:
        vid = f"{p.vid:04X}" if p.vid is not None else "----"
        pid = f"{p.pid:04X}" if p.pid is not None else "----"
        print(f"  - {p.device} vid:pid={vid}:{pid} desc={p.description}")
try:
    selected_port = resolve_port(port, baud_i)
except Exception as e:
    print(f"[serial-backlog] {e}", file=sys.stderr)
    sys.exit(5)

print(f"[serial-backlog] port={selected_port} baud={baud_i} out={csv_path}")

with open_serial_exclusive(selected_port, baud_i, 0.25) as ser, open(
    csv_path, "w", newline="", encoding="utf-8"
) as f:
    writer = csv.writer(f)
    writer.writerow(
        [
            "session_id_hex",
            "record_seq",
            "bssid",
            "ssid",
            "auth",
            "channel",
            "rssi",
            "node_id",
            "flags",
            "source_flags",
            "gps_valid",
            "gps_source",
            "gps_lat_e7",
            "gps_lon_e7",
            "gps_alt_mm",
            "gps_unix_time_s",
            "gps_accuracy_cm",
        ]
    )

    c = Client(ser)

    # Warmup: request status with protocol v2, then v1 fallback.
    warmup_deadline = time.monotonic() + 6.0
    while c.frames_seen == 0 and time.monotonic() < warmup_deadline:
        c.send_cmd(COMMANDS.REQUEST_STATUS)
        c.poll(writer)
        time.sleep(0.15)
    if c.frames_seen == 0:
        c.proto_version = 1
        print("[serial-backlog] no frames with protocol v2; retrying warmup with protocol v1", file=sys.stderr)
        warmup_deadline = time.monotonic() + 4.0
        while c.frames_seen == 0 and time.monotonic() < warmup_deadline:
            c.send_cmd(COMMANDS.REQUEST_STATUS)
            c.poll(writer)
            time.sleep(0.15)
    if c.frames_seen == 0:
        print(
            "[serial-backlog] No WG frames received after handshake.\n"
            f"rx_bytes={c.rx_bytes} proto_v2_then_v1 attempted.\n"
            "Likely causes: wrong endpoint, host serial task not consuming commands, or protocol mismatch.",
            file=sys.stderr,
        )
        sys.exit(6)

    # First valid command switches ESP USB stream into frame-only mode.
    c.send_cmd(COMMANDS.STOP)
    time.sleep(0.05)
    c.send_cmd(COMMANDS.SET_REPLAY, b"\x01")
    started_at = time.monotonic()
    done_since = None
    last_report = 0.0

    try:
        while True:
            c.poll(writer)

            # Periodic progress line.
            now = time.monotonic()
            if now - last_report >= 2.0:
                st = c.last_status
                q = int(getattr(st, "queued_records", 0)) if st is not None else -1
                ra = int(bool(getattr(st, "replay_active", False))) if st is not None else -1
                print(
                    f"[serial-backlog] rows={c.rows_written} queued={q} replay_active={ra} "
                    f"acks={c.acks_sent} errs={c.error_count} frames={c.frames_seen} rx_bytes={c.rx_bytes}",
                    flush=True,
                )
                last_report = now

            # Completion when status says empty and remains quiet.
            if status_done(c.last_status):
                if done_since is None:
                    done_since = now
                c.flush_acks()
                if now - done_since >= max(2.0, status_interval * 2):
                    break
            else:
                done_since = None

            # Safety idle timeout.
            if now - c.last_rx_time > idle_timeout:
                c.flush_acks()
                if status_done(c.last_status):
                    break
                print(
                    "[serial-backlog] idle timeout while replay still active/not-empty; "
                    "stopping to avoid hanging forever",
                    file=sys.stderr,
                )
                sys.exit(4)
    finally:
        # Best effort: stop replay mode.
        try:
            c.flush_acks()
            c.send_cmd(COMMANDS.SET_REPLAY, b"\x00")
            c.send_cmd(COMMANDS.REQUEST_STATUS)
        except Exception:
            pass

elapsed = time.monotonic() - started_at
session_count = len(c.session_counts)
print(
    f"[serial-backlog] done rows={c.rows_written} sessions={session_count} "
    f"acks={c.acks_sent} errors={c.error_count} elapsed={elapsed:.1f}s csv={csv_path}"
)
PY
