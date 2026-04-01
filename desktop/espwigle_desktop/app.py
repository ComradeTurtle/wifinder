from __future__ import annotations

import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

from PySide6.QtCore import QTimer, Qt
from PySide6.QtGui import QFont, QTextCursor
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QTableWidget,
    QTableWidgetItem,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

import serial
from serial.tools import list_ports

from .protocol import (
    COMMANDS,
    MESSAGE_TYPES,
    FrameStreamDecoder,
    decode_gps_payload,
    decode_sighting_payload,
    decode_status_payload,
    encode_command_frame,
)
from .state import ConsoleState
from .wigle_csv import WigleCsvLogger, WigleWifiRow

MIN_HOP_MS = 50
MAX_HOP_MS = 2000
MIN_VISIBLE_TIMEOUT_SEC = 5
MAX_VISIBLE_TIMEOUT_SEC = 300

AUTO_HOP_MIN_MS = 70
AUTO_HOP_SPEED_CAP_MMPS = 33333
AUTO_HOP_MAX_REDUCTION_PCT = 60
AUTO_HOP_MIN_APPLY_INTERVAL_MS = 1200
AUTO_HOP_MIN_DELTA_MS = 10
AUTO_HOP_LOG_INTERVAL_MS = 10000

MAX_ESP_GPS_AGE_MS_FOR_LOGGING = 5000
MIN_LOG_INTERVAL_PER_BSSID_MS = 2000
NO_GPS_LOG_THROTTLE_MS = 10000
REPLAY_ACK_BATCH_SIZE = 25
REPLAY_ACK_MIN_INTERVAL_MS = 2000


def now_ms() -> int:
    return int(time.time() * 1000)


def gps_source_label(source: int) -> str:
    if source == 1:
        return "UART"
    if source == 2:
        return "PHONE"
    return "NONE"


def gps_nav_mode_label(mode: int) -> str:
    if mode == 1:
        return "Force 1 Hz"
    if mode == 2:
        return "Force 2 Hz"
    if mode == 4:
        return "Force 4 Hz"
    return "Auto"


def compute_auto_hop_ms(base_hop_ms: int, speed_mmps: int) -> int:
    base = max(MIN_HOP_MS, min(MAX_HOP_MS, base_hop_ms))
    bounded_speed = max(0, min(AUTO_HOP_SPEED_CAP_MMPS, speed_mmps))
    reduction_pct = max(
        0,
        min(
            AUTO_HOP_MAX_REDUCTION_PCT,
            (bounded_speed * AUTO_HOP_MAX_REDUCTION_PCT) // AUTO_HOP_SPEED_CAP_MMPS,
        ),
    )
    scaled = min(base, ((base * (100 - reduction_pct) + 50) // 100))
    dynamic_min = min(base, AUTO_HOP_MIN_MS)
    return max(dynamic_min, min(base, scaled))


@dataclass
class LastEspGps:
    valid: bool = False
    source: int = 0
    lat_e7: int = 0
    lon_e7: int = 0
    alt_mm: int = 0
    speed_mmps: int = 0
    bearing_mdeg: int = 0
    unix_time_s: int = 0
    accuracy_cm: int = 0
    sat_count: int = 0
    hdop_centi: int = 0
    pdop_centi: int = 0
    received_ms: int = 0


class DesktopWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("ESPWIGLE Desktop Console")
        self.resize(1320, 860)

        self.serial: serial.Serial | None = None
        self.decoder = FrameStreamDecoder()
        self.seq = 1

        self.state = ConsoleState()
        self.visible_timeout_sec = 25
        self.connected = False
        self.scanning = False
        self.current_channel = 1
        self.hop_ms = 250
        self.channel_mask = 0x1FFF
        self.unique_bssids = 0
        self.packets_per_sec = 0
        self.dropped_notifies = 0
        self.boot_mode = 0
        self.gps_valid = False
        self.gps_source = 0
        self.gps_age_s = 0
        self.gps_accuracy_dm = 0
        self.gps_sat_count = 0
        self.gps_hdop_centi = 0
        self.gps_pdop_centi = 0
        self.node_link_up = False
        self.node_last_seen_s = 0
        self.node_packets_per_sec = 0
        self.node_forwarded_sightings = 0
        self.node_channel = 0
        self.node_channel_mask = 0
        self.session_open = False
        self.session_id = 0
        self.queued_records = 0
        self.queued_bytes = 0
        self.replay_active = False
        self.replay_cursor = 0
        self.queue_full = False
        self.dropped_flash_full = 0
        self.node_count = 0
        self.last_esp_gps = LastEspGps()

        self.hop_dirty = False
        self.channel_dirty = False
        self.auto_hop_enabled = False
        self.auto_hop_base_ms = 250
        self.auto_hop_applied_ms = 250
        self.last_auto_hop_apply_ms = 0
        self.last_auto_hop_log_ms = 0
        self.gps_nav_mode = 0

        self.csv_logger = WigleCsvLogger()
        self.logging_enabled = False
        self.last_csv_write_by_bssid: dict[str, int] = {}
        self.last_no_gps_log_ms = 0
        self.session_ack_high: dict[int, int] = {}
        self.session_ack_pending: dict[int, set[int]] = {}
        self.session_ack_sent: dict[int, int] = {}
        self.session_ack_last_ms: dict[int, int] = {}
        self.download_backlog_active = False
        self.download_backlog_auto_stop = False

        self._build_ui()
        self.refresh_ports()
        self._sync_ui_from_state()

        self.poll_timer = QTimer(self)
        self.poll_timer.timeout.connect(self._poll_serial)
        self.poll_timer.start(40)

        self.ui_timer = QTimer(self)
        self.ui_timer.timeout.connect(self._periodic_refresh)
        self.ui_timer.start(1000)

    def _build_ui(self) -> None:
        root = QWidget(self)
        self.setCentralWidget(root)
        layout = QVBoxLayout(root)

        conn_row = QHBoxLayout()
        self.port_combo = QComboBox()
        self.refresh_btn = QPushButton("Refresh Ports")
        self.refresh_btn.clicked.connect(self.refresh_ports)
        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(self.on_toggle_connect)
        self.baud_spin = QSpinBox()
        self.baud_spin.setRange(9600, 3000000)
        self.baud_spin.setSingleStep(100)
        self.baud_spin.setValue(115200)
        conn_row.addWidget(QLabel("Port"))
        conn_row.addWidget(self.port_combo, 2)
        conn_row.addWidget(QLabel("Baud"))
        conn_row.addWidget(self.baud_spin)
        conn_row.addWidget(self.refresh_btn)
        conn_row.addWidget(self.connect_btn)
        layout.addLayout(conn_row)

        status_row = QHBoxLayout()
        self.conn_chip = QLabel("SERIAL ✗")
        self.scan_chip = QLabel("SCAN ✗")
        self.gps_chip = QLabel("GPS ✗")
        self.node_chip = QLabel("NODE ✗")
        self.csv_chip = QLabel("CSV ✗")
        for chip in [self.conn_chip, self.scan_chip, self.gps_chip, self.node_chip, self.csv_chip]:
            chip.setStyleSheet("padding:6px 10px; border:1px solid #666; border-radius:8px;")
            status_row.addWidget(chip)
        status_row.addStretch(1)
        layout.addLayout(status_row)

        action_row = QHBoxLayout()
        self.btn_start = QPushButton("Start")
        self.btn_stop = QPushButton("Stop")
        self.btn_status = QPushButton("Status")
        self.btn_snapshot = QPushButton("Snapshot")
        self.btn_download = QPushButton("Download ESP")
        self.btn_clear = QPushButton("Clear")
        self.btn_clear_storage = QPushButton("Clear ESP")
        self.btn_csv = QPushButton("Start CSV")
        self.csv_path_label = QLabel("-")
        self.csv_path_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.btn_start.clicked.connect(self.send_start)
        self.btn_stop.clicked.connect(self.send_stop)
        self.btn_status.clicked.connect(self.request_status)
        self.btn_snapshot.clicked.connect(self.request_snapshot)
        self.btn_download.clicked.connect(self.toggle_backlog_download)
        self.btn_clear.clicked.connect(self.clear_sightings)
        self.btn_clear_storage.clicked.connect(self.clear_storage_sessions)
        self.btn_csv.clicked.connect(self.toggle_csv)
        action_row.addWidget(self.btn_start)
        action_row.addWidget(self.btn_stop)
        action_row.addWidget(self.btn_status)
        action_row.addWidget(self.btn_snapshot)
        action_row.addWidget(self.btn_download)
        action_row.addWidget(self.btn_clear)
        action_row.addWidget(self.btn_clear_storage)
        action_row.addWidget(self.btn_csv)
        action_row.addWidget(QLabel("CSV Path:"))
        action_row.addWidget(self.csv_path_label, 1)
        layout.addLayout(action_row)

        config_box = QGroupBox("Controls")
        config_layout = QGridLayout(config_box)
        self.hop_spin = QSpinBox()
        self.hop_spin.setRange(MIN_HOP_MS, MAX_HOP_MS)
        self.hop_spin.valueChanged.connect(self._mark_hop_dirty)
        self.auto_hop_check = QCheckBox("Auto hop by speed")
        self.auto_hop_check.toggled.connect(self.on_toggle_auto_hop)
        self.apply_hop_btn = QPushButton("Apply Hop")
        self.apply_hop_btn.clicked.connect(self.apply_hop)
        self.boot_combo = QComboBox()
        self.boot_combo.addItem("Manual", 0)
        self.boot_combo.addItem("Auto", 1)
        self.apply_boot_btn = QPushButton("Apply Boot")
        self.apply_boot_btn.clicked.connect(self.apply_boot)
        self.gps_nav_combo = QComboBox()
        self.gps_nav_combo.addItem("Auto", 0)
        self.gps_nav_combo.addItem("Force 1 Hz", 1)
        self.gps_nav_combo.addItem("Force 2 Hz", 2)
        self.gps_nav_combo.addItem("Force 4 Hz", 4)
        self.apply_gps_nav_btn = QPushButton("Apply GPS Nav")
        self.apply_gps_nav_btn.clicked.connect(self.apply_gps_nav_mode)
        self.visible_spin = QSpinBox()
        self.visible_spin.setRange(MIN_VISIBLE_TIMEOUT_SEC, MAX_VISIBLE_TIMEOUT_SEC)
        self.visible_spin.setValue(self.visible_timeout_sec)
        self.visible_apply_btn = QPushButton("Apply Timeout")
        self.visible_apply_btn.clicked.connect(self.apply_visible_timeout)
        config_layout.addWidget(QLabel("Hop (ms)"), 0, 0)
        config_layout.addWidget(self.hop_spin, 0, 1)
        config_layout.addWidget(self.auto_hop_check, 0, 2)
        config_layout.addWidget(self.apply_hop_btn, 0, 3)
        config_layout.addWidget(QLabel("Boot"), 1, 0)
        config_layout.addWidget(self.boot_combo, 1, 1)
        config_layout.addWidget(self.apply_boot_btn, 1, 2)
        config_layout.addWidget(QLabel("GPS Nav"), 1, 3)
        config_layout.addWidget(self.gps_nav_combo, 1, 4)
        config_layout.addWidget(self.apply_gps_nav_btn, 1, 5)
        config_layout.addWidget(QLabel("Visible Timeout (s)"), 2, 0)
        config_layout.addWidget(self.visible_spin, 2, 1)
        config_layout.addWidget(self.visible_apply_btn, 2, 2)

        self.channel_checks: list[QCheckBox] = []
        chan_grid = QGridLayout()
        for ch in range(1, 14):
            cb = QCheckBox(str(ch))
            cb.setChecked(True)
            cb.toggled.connect(self._mark_channel_dirty)
            self.channel_checks.append(cb)
            chan_grid.addWidget(cb, (ch - 1) // 7, (ch - 1) % 7)
        self.apply_channels_btn = QPushButton("Apply Channels")
        self.apply_channels_btn.clicked.connect(self.apply_channels)
        config_layout.addLayout(chan_grid, 3, 0, 1, 5)
        config_layout.addWidget(self.apply_channels_btn, 3, 5)
        layout.addWidget(config_box)

        detail_row = QHBoxLayout()
        self.general_box = QGroupBox("General Status")
        self.gps_box = QGroupBox("GPS Status")
        self.node_box = QGroupBox("Node Status")
        self.general_form = QFormLayout(self.general_box)
        self.gps_form = QFormLayout(self.gps_box)
        self.node_form = QFormLayout(self.node_box)
        self.general_fields = self._make_detail_fields(
            self.general_form,
            [
                "Link",
                "Scanner",
                "Channel",
                "Hop",
                "Mask",
                "Unique",
                "Packets/s",
                "Drops",
                "Boot",
                "GPS Nav",
                "Session",
                "Queue",
                "Replay",
                "Dropped Full",
                "Visible Rows",
            ],
        )
        self.gps_fields = self._make_detail_fields(
            self.gps_form,
            ["Source", "Valid", "Age", "Accuracy", "Satellites", "HDOP", "PDOP", "Speed"],
        )
        self.node_fields = self._make_detail_fields(
            self.node_form,
            ["Link", "Last Seen", "Channel", "Mask", "Packets/s", "Sightings", "Nodes"],
        )
        detail_row.addWidget(self.general_box)
        detail_row.addWidget(self.gps_box)
        detail_row.addWidget(self.node_box)
        layout.addLayout(detail_row)

        self.table = QTableWidget(0, 7)
        self.table.setHorizontalHeaderLabels(["BSSID", "SSID", "Auth", "Ch", "RSSI", "Age(s)", "Seen"])
        self.table.verticalHeader().setDefaultSectionSize(22)
        self.table.setAlternatingRowColors(True)
        self.table.setSortingEnabled(False)
        mono = QFont("Monospace")
        mono.setStyleHint(QFont.TypeWriter)
        self.table.setFont(mono)
        layout.addWidget(self.table, 3)

        self.log = QTextEdit()
        self.log.setReadOnly(True)
        self.log.setLineWrapMode(QTextEdit.NoWrap)
        self.log.setMaximumHeight(180)
        self.log.setFont(mono)
        layout.addWidget(self.log)

    @staticmethod
    def _make_detail_fields(form: QFormLayout, keys: list[str]) -> dict[str, QLabel]:
        out: dict[str, QLabel] = {}
        for key in keys:
            label = QLabel("-")
            label.setTextInteractionFlags(Qt.TextSelectableByMouse)
            form.addRow(key, label)
            out[key] = label
        return out

    def _append_log(self, message: str) -> None:
        ts = datetime.now().strftime("%H:%M:%S")
        self.log.insertPlainText(f"[{ts}] {message}\n")
        self.log.moveCursor(QTextCursor.End)

    def refresh_ports(self) -> None:
        ports = sorted(list_ports.comports(), key=lambda p: p.device)
        cur = self.port_combo.currentText()
        self.port_combo.clear()
        for p in ports:
            self.port_combo.addItem(p.device)
        if cur:
            idx = self.port_combo.findText(cur)
            if idx >= 0:
                self.port_combo.setCurrentIndex(idx)

    def on_toggle_connect(self) -> None:
        if self.connected:
            self.disconnect_serial()
            return
        port = self.port_combo.currentText().strip()
        if not port:
            QMessageBox.warning(self, "No Port", "Select a serial port first.")
            return
        try:
            self.serial = serial.Serial(
                port=port,
                baudrate=self.baud_spin.value(),
                timeout=0,
                write_timeout=0.2,
            )
        except Exception as exc:
            self._append_log(f"Connect failed: {exc}")
            return
        self.connected = True
        self.connect_btn.setText("Disconnect")
        self._append_log(f"Connected to {port}")
        self.request_status()
        self.request_snapshot()
        self._sync_ui_from_state()

    def disconnect_serial(self) -> None:
        if self.serial is not None:
            try:
                self.serial.close()
            except Exception:
                pass
        self.serial = None
        self.connected = False
        self.download_backlog_active = False
        self.download_backlog_auto_stop = False
        self.connect_btn.setText("Connect")
        self._append_log("Disconnected")
        self._sync_ui_from_state()

    def closeEvent(self, event) -> None:  # type: ignore[override]
        self.disconnect_serial()
        self.stop_csv()
        super().closeEvent(event)

    def _poll_serial(self) -> None:
        if not self.connected or self.serial is None:
            return
        try:
            data = self.serial.read(512)
        except Exception as exc:
            self._append_log(f"Serial read error: {exc}")
            self.disconnect_serial()
            return
        if not data:
            return
        for frame in self.decoder.feed(data):
            self._handle_frame(frame)

    def _handle_frame(self, frame) -> None:
        if frame.type == MESSAGE_TYPES.STATUS:
            status = decode_status_payload(frame.payload)
            self.scanning = status.scanning
            self.current_channel = status.current_channel
            self.hop_ms = status.hop_ms
            self.channel_mask = status.channel_mask
            self.unique_bssids = status.unique_bssids
            self.packets_per_sec = status.packets_per_sec
            self.dropped_notifies = status.dropped_notifies
            self.boot_mode = status.boot_mode
            self.gps_valid = status.gps_valid
            self.gps_age_s = status.gps_age_s
            self.gps_accuracy_dm = status.gps_accuracy_dm
            self.node_link_up = status.node_link_up
            self.node_last_seen_s = status.node_last_seen_s
            self.node_packets_per_sec = status.node_packets_per_sec
            self.node_forwarded_sightings = status.node_forwarded_sightings
            self.node_channel = status.node_channel
            self.node_channel_mask = status.node_channel_mask
            self.session_open = status.session_open
            self.session_id = status.session_id
            self.queued_records = status.queued_records
            self.queued_bytes = status.queued_bytes
            self.replay_active = status.replay_active
            self.replay_cursor = status.replay_cursor
            self.queue_full = status.queue_full
            self.dropped_flash_full = status.dropped_flash_full
            self.node_count = status.node_count
            self.auto_hop_applied_ms = status.hop_ms
            if self.download_backlog_active and status.scanning:
                self.download_backlog_active = False
                self._append_log("Backlog download stopped: scanner is active")
                should_auto_stop_csv = self.download_backlog_auto_stop
                self.download_backlog_auto_stop = False
                if should_auto_stop_csv and self.logging_enabled:
                    self.stop_csv()
            elif (
                self.download_backlog_active
                and status.queued_records == 0
                and not status.replay_active
            ):
                self.download_backlog_active = False
                self._send_command(COMMANDS.SET_REPLAY, b"\x00")
                self._append_log("Backlog download complete")
                should_auto_stop_csv = self.download_backlog_auto_stop
                self.download_backlog_auto_stop = False
                if should_auto_stop_csv and self.logging_enabled:
                    self.stop_csv()
            if not self.hop_dirty:
                self.hop_spin.setValue(status.hop_ms)
            if not self.channel_dirty:
                self._apply_mask_to_ui(status.channel_mask)
            self._sync_ui_from_state()
            self._maybe_apply_auto_hop(force=False, reason="status")
            return

        if frame.type == MESSAGE_TYPES.GPS:
            gps = decode_gps_payload(frame.payload)
            self.last_esp_gps = LastEspGps(
                valid=gps.valid,
                source=gps.source,
                lat_e7=gps.lat_e7,
                lon_e7=gps.lon_e7,
                alt_mm=gps.alt_mm,
                speed_mmps=gps.speed_mmps,
                bearing_mdeg=gps.bearing_mdeg,
                unix_time_s=gps.unix_time_s,
                accuracy_cm=gps.accuracy_cm,
                sat_count=gps.sat_count,
                hdop_centi=gps.hdop_centi,
                pdop_centi=gps.pdop_centi,
                received_ms=now_ms(),
            )
            self.gps_source = gps.source
            self.gps_sat_count = gps.sat_count
            self.gps_hdop_centi = gps.hdop_centi
            self.gps_pdop_centi = gps.pdop_centi
            self._sync_ui_from_state()
            self._maybe_apply_auto_hop(force=False, reason="gps")
            return

        if frame.type == MESSAGE_TYPES.SIGHTING:
            s = decode_sighting_payload(frame.payload)
            is_replay = (s.source_flags & 0x02) != 0
            if is_replay and not self._track_record_seq(s.session_id, s.record_seq):
                return
            seen_ms = now_ms()
            self.state.apply_sighting(
                bssid=s.bssid,
                ssid=s.ssid,
                auth=s.auth,
                channel=s.channel,
                rssi=s.rssi,
                seen_ms=seen_ms,
            )
            self._maybe_append_csv_row(s, seen_ms)
            self._refresh_table()
            self._sync_ui_from_state()
            return

        if frame.type == MESSAGE_TYPES.NODE_TABLE:
            return

        if frame.type == MESSAGE_TYPES.ACK:
            cmd = frame.payload[0] if frame.payload else 0
            self._append_log(f"ACK cmd=0x{cmd:02X}")
            self.hop_dirty = False
            self.channel_dirty = False
            return

        if frame.type == MESSAGE_TYPES.ERROR:
            cmd = frame.payload[0] if len(frame.payload) >= 1 else 0
            code = frame.payload[1] if len(frame.payload) >= 2 else 0
            self._append_log(f"ERROR cmd=0x{cmd:02X} code={code}")
            return

        if frame.type == MESSAGE_TYPES.SNAPSHOT_END:
            self._append_log("Snapshot complete")
            return

    def _track_record_seq(self, session_id: int, record_seq: int) -> bool:
        if session_id <= 0 or record_seq <= 0:
            return True

        highest = self.session_ack_high.get(session_id, 0)
        gaps = self.session_ack_pending.setdefault(session_id, set())
        is_new = True

        if record_seq <= highest:
            is_new = False
        elif record_seq == highest + 1:
            highest = record_seq
            while (highest + 1) in gaps:
                gaps.remove(highest + 1)
                highest += 1
            self.session_ack_high[session_id] = highest
        else:
            if record_seq in gaps:
                is_new = False
            else:
                gaps.add(record_seq)

        self._maybe_send_replay_ack(session_id, force=False)
        return is_new

    def _maybe_send_replay_ack(self, session_id: int, force: bool) -> None:
        highest = self.session_ack_high.get(session_id, 0)
        sent = self.session_ack_sent.get(session_id, 0)
        if highest <= 0 or highest <= sent:
            return
        if not self.connected:
            return
        now = now_ms()
        last_ms = self.session_ack_last_ms.get(session_id, 0)
        if not force:
            if (highest - sent) < REPLAY_ACK_BATCH_SIZE and (now - last_ms) < REPLAY_ACK_MIN_INTERVAL_MS:
                return
        payload = session_id.to_bytes(8, "little", signed=False) + highest.to_bytes(
            4, "little", signed=False
        )
        self._send_command(COMMANDS.REPLAY_ACK, payload)
        self.session_ack_sent[session_id] = highest
        self.session_ack_last_ms[session_id] = now

    def _flush_replay_acks(self) -> None:
        for session_id in list(self.session_ack_high.keys()):
            self._maybe_send_replay_ack(session_id, force=False)

    def _send_command(self, command_id: int, payload: bytes = b"") -> bool:
        if not self.connected or self.serial is None:
            self._append_log("Command blocked: not connected")
            return False
        frame = encode_command_frame(
            seq=self.seq,
            device_ms=now_ms() & 0xFFFFFFFF,
            command_id=command_id,
            payload=payload,
        )
        self.seq = (self.seq + 1) & 0xFFFF
        if self.seq == 0:
            self.seq = 1
        try:
            self.serial.write(frame)
            return True
        except Exception as exc:
            self._append_log(f"Command write failed: {exc}")
            self.disconnect_serial()
            return False

    def send_start(self) -> None:
        self._send_command(COMMANDS.START)

    def send_stop(self) -> None:
        self._send_command(COMMANDS.STOP)

    def request_status(self) -> None:
        self._send_command(COMMANDS.REQUEST_STATUS)

    def request_snapshot(self) -> None:
        self._send_command(COMMANDS.REQUEST_SNAPSHOT)

    def toggle_backlog_download(self) -> None:
        if not self.connected:
            self._append_log("Backlog download blocked: not connected")
            return
        if self.scanning:
            self._append_log("Backlog download blocked: stop scan first")
            return
        if self.download_backlog_active:
            if not self._send_command(COMMANDS.SET_REPLAY, b"\x00"):
                return
            self.download_backlog_active = False
            self._append_log("Backlog download stopped")
            should_auto_stop_csv = self.download_backlog_auto_stop
            self.download_backlog_auto_stop = False
            if should_auto_stop_csv and self.logging_enabled:
                self.stop_csv()
            self._sync_ui_from_state()
            return

        self.download_backlog_auto_stop = not self.logging_enabled
        if self.download_backlog_auto_stop:
            self.start_csv()
        self.session_ack_high.clear()
        self.session_ack_pending.clear()
        self.session_ack_sent.clear()
        self.session_ack_last_ms.clear()
        if not self._send_command(COMMANDS.SET_REPLAY, b"\x01"):
            if self.download_backlog_auto_stop and self.logging_enabled:
                self.stop_csv()
            self.download_backlog_auto_stop = False
            return
        self.download_backlog_active = True
        self._append_log("Backlog download started")
        self.request_status()
        self._sync_ui_from_state()

    def apply_hop(self) -> None:
        hop = int(self.hop_spin.value())
        self.auto_hop_base_ms = hop
        if self.auto_hop_enabled:
            self._append_log(f"Auto-hop base set to {hop}ms")
            self._maybe_apply_auto_hop(force=True, reason="base-update")
            return
        payload = bytes([hop & 0xFF, (hop >> 8) & 0xFF])
        self._send_command(COMMANDS.SET_HOP_MS, payload)
        self._append_log(f"Set hop to {hop}ms")
        self.hop_dirty = False

    def apply_channels(self) -> None:
        mask = self._mask_from_ui()
        if mask == 0:
            self._append_log("Select at least one channel")
            return
        payload = bytes([mask & 0xFF, (mask >> 8) & 0xFF])
        self._send_command(COMMANDS.SET_CHANNEL_MASK, payload)
        self._append_log(f"Set channel mask 0x{mask:04X}")
        self.channel_dirty = False

    def apply_boot(self) -> None:
        mode = int(self.boot_combo.currentData())
        self._send_command(COMMANDS.SET_BOOT_MODE, bytes([mode & 0xFF]))
        self._append_log(f"Set boot mode {'Auto' if mode == 1 else 'Manual'}")

    def apply_gps_nav_mode(self) -> None:
        mode = int(self.gps_nav_combo.currentData())
        if mode not in (0, 1, 2, 4):
            self._append_log("Invalid GPS nav mode")
            return
        if self._send_command(COMMANDS.SET_GPS_NAV_RATE, bytes([mode & 0xFF])):
            self.gps_nav_mode = mode
            self._append_log(f"Set GPS nav mode {gps_nav_mode_label(mode)}")
            self._sync_ui_from_state()

    def apply_visible_timeout(self) -> None:
        self.visible_timeout_sec = max(
            MIN_VISIBLE_TIMEOUT_SEC,
            min(MAX_VISIBLE_TIMEOUT_SEC, int(self.visible_spin.value())),
        )
        self._append_log(f"Visible timeout set to {self.visible_timeout_sec}s")
        self._refresh_table()

    def clear_sightings(self) -> None:
        self.state.sightings.clear()
        self.state.ssid_memory.clear()
        self.last_csv_write_by_bssid.clear()
        self._refresh_table()
        self._append_log("Cleared visible sightings")

    def clear_storage_sessions(self) -> None:
        if not self.connected:
            self._append_log("Storage clear blocked: not connected")
            return
        if self.scanning:
            self._append_log("Storage clear blocked: stop scan first")
            return
        if self.download_backlog_active:
            if not self._send_command(COMMANDS.SET_REPLAY, b"\x00"):
                return
            self.download_backlog_active = False
            self.download_backlog_auto_stop = False
        if not self._send_command(COMMANDS.CLEAR_STORAGE):
            return
        self.session_ack_high.clear()
        self.session_ack_pending.clear()
        self.session_ack_sent.clear()
        self.session_ack_last_ms.clear()
        self.queued_records = 0
        self.queued_bytes = 0
        self.replay_active = False
        self.replay_cursor = 0
        self.queue_full = False
        self._append_log("ESP storage clear requested")
        self.request_status()
        self._sync_ui_from_state()

    def toggle_csv(self) -> None:
        if self.logging_enabled:
            self.stop_csv()
        else:
            self.start_csv()

    def start_csv(self) -> None:
        if self.logging_enabled:
            return
        ts = datetime.now().strftime("%Y%m%d-%H%M%S")
        path = Path.home() / "Downloads" / "espwigle" / f"wigle-{ts}.csv"
        self.csv_logger.start_session(path, app_release="espwigle-desktop-1")
        self.logging_enabled = True
        self.btn_csv.setText("Stop CSV")
        self.csv_path_label.setText(str(path))
        self._append_log(f"CSV logging started: {path}")
        self._sync_ui_from_state()

    def stop_csv(self) -> None:
        if not self.logging_enabled:
            return
        self.csv_logger.stop_session()
        self.logging_enabled = False
        self.btn_csv.setText("Start CSV")
        self._append_log("CSV logging stopped")
        self._sync_ui_from_state()

    def _periodic_refresh(self) -> None:
        now = now_ms()
        self._flush_replay_acks()
        self.state.prune(now_ms=now, max_age_ms=self.visible_timeout_sec * 1000)
        self._refresh_table()
        self._sync_ui_from_state()
        self._maybe_apply_auto_hop(force=False, reason="tick")

    def _refresh_table(self) -> None:
        rows = self.state.to_rows(now_ms=now_ms(), max_age_ms=self.visible_timeout_sec * 1000)
        self.table.setRowCount(len(rows))
        for i, row in enumerate(rows):
            values = [
                row.bssid,
                row.ssid if row.ssid else "<hidden>",
                row.auth,
                str(row.channel),
                str(row.rssi),
                f"{row.age_ms / 1000.0:.1f}",
                str(row.seen_count),
            ]
            for j, value in enumerate(values):
                item = QTableWidgetItem(value)
                if j in (0, 5):
                    item.setTextAlignment(Qt.AlignRight | Qt.AlignVCenter)
                self.table.setItem(i, j, item)

    def _sync_ui_from_state(self) -> None:
        self.btn_download.setText("Stop Download" if self.download_backlog_active else "Download ESP")
        self.conn_chip.setText("SERIAL ✓" if self.connected else "SERIAL ✗")
        self.scan_chip.setText("SCAN ✓" if self.scanning else "SCAN ✗")
        self.gps_chip.setText("GPS ✓" if self.gps_valid else "GPS ✗")
        self.node_chip.setText("NODE ✓" if self.node_link_up else "NODE ✗")
        self.csv_chip.setText("CSV ✓" if self.logging_enabled else "CSV ✗")

        self.general_fields["Link"].setText("Connected" if self.connected else "Disconnected")
        self.general_fields["Scanner"].setText("Running" if self.scanning else "Stopped")
        self.general_fields["Channel"].setText(str(self.current_channel))
        if self.auto_hop_enabled:
            self.general_fields["Hop"].setText(
                f"Auto base={self.auto_hop_base_ms}ms applied={self.auto_hop_applied_ms}ms"
            )
        else:
            self.general_fields["Hop"].setText(f"{self.hop_ms}ms")
        self.general_fields["Mask"].setText(f"0x{self.channel_mask:04X}")
        self.general_fields["Unique"].setText(str(self.unique_bssids))
        self.general_fields["Packets/s"].setText(str(self.packets_per_sec))
        self.general_fields["Drops"].setText(str(self.dropped_notifies))
        self.general_fields["Boot"].setText("Auto" if self.boot_mode == 1 else "Manual")
        self.general_fields["GPS Nav"].setText(gps_nav_mode_label(self.gps_nav_mode))
        if self.session_open and self.session_id != 0:
            self.general_fields["Session"].setText(f"Open 0x{self.session_id:016X}")
        elif self.session_id != 0:
            self.general_fields["Session"].setText(f"Closed 0x{self.session_id:016X}")
        else:
            self.general_fields["Session"].setText("-")
        self.general_fields["Queue"].setText(
            f"{self.queued_records} rec / {self.queued_bytes} B"
        )
        self.general_fields["Replay"].setText(
            f"{'Active' if self.replay_active else 'Idle'} @ {self.replay_cursor}"
        )
        self.general_fields["Dropped Full"].setText(
            f"{self.dropped_flash_full}{' (FULL)' if self.queue_full else ''}"
        )
        self.general_fields["Visible Rows"].setText(str(len(self.state.sightings)))

        hdop = f"{self.gps_hdop_centi / 100.0:.2f}" if self.gps_hdop_centi > 0 else "-"
        pdop = f"{self.gps_pdop_centi / 100.0:.2f}" if self.gps_pdop_centi > 0 else "-"
        self.gps_fields["Source"].setText(gps_source_label(self.gps_source))
        self.gps_fields["Valid"].setText("Yes" if self.gps_valid else "No")
        self.gps_fields["Age"].setText(f"{self.gps_age_s}s")
        self.gps_fields["Accuracy"].setText(f"±{self.gps_accuracy_dm / 10.0:.1f}m")
        self.gps_fields["Satellites"].setText(str(self.gps_sat_count or 0))
        self.gps_fields["HDOP"].setText(hdop)
        self.gps_fields["PDOP"].setText(pdop)
        self.gps_fields["Speed"].setText(f"{self.last_esp_gps.speed_mmps * 0.0036:.1f} km/h")

        self.node_fields["Link"].setText("Up" if self.node_link_up else "Down")
        self.node_fields["Last Seen"].setText(f"{self.node_last_seen_s}s")
        self.node_fields["Channel"].setText(str(self.node_channel))
        self.node_fields["Mask"].setText(f"0x{self.node_channel_mask:04X}")
        self.node_fields["Packets/s"].setText(str(self.node_packets_per_sec))
        self.node_fields["Sightings"].setText(str(self.node_forwarded_sightings))
        self.node_fields["Nodes"].setText(str(self.node_count))

        nav_idx = self.gps_nav_combo.findData(self.gps_nav_mode)
        if nav_idx < 0:
            nav_idx = 0
        self.gps_nav_combo.blockSignals(True)
        self.gps_nav_combo.setCurrentIndex(nav_idx)
        self.gps_nav_combo.blockSignals(False)

    def _mask_from_ui(self) -> int:
        mask = 0
        for i, cb in enumerate(self.channel_checks, start=1):
            if cb.isChecked():
                mask |= 1 << (i - 1)
        return mask

    def _apply_mask_to_ui(self, mask: int) -> None:
        for i, cb in enumerate(self.channel_checks, start=1):
            cb.blockSignals(True)
            cb.setChecked((mask & (1 << (i - 1))) != 0)
            cb.blockSignals(False)

    def _mark_hop_dirty(self) -> None:
        self.hop_dirty = True

    def _mark_channel_dirty(self) -> None:
        self.channel_dirty = True

    def on_toggle_auto_hop(self, enabled: bool) -> None:
        self.auto_hop_enabled = enabled
        self.auto_hop_base_ms = int(self.hop_spin.value())
        if enabled:
            self._append_log(f"Auto-hop enabled (base {self.auto_hop_base_ms}ms)")
            self._maybe_apply_auto_hop(force=True, reason="enabled")
        else:
            self._append_log("Auto-hop disabled")
            payload = bytes([self.auto_hop_base_ms & 0xFF, (self.auto_hop_base_ms >> 8) & 0xFF])
            self._send_command(COMMANDS.SET_HOP_MS, payload)

    def _maybe_apply_auto_hop(self, force: bool, reason: str) -> None:
        if not self.auto_hop_enabled or not self.connected:
            return
        now = now_ms()
        speed_mmps = max(0, self.last_esp_gps.speed_mmps if self.last_esp_gps.valid else 0)
        target = compute_auto_hop_ms(self.auto_hop_base_ms, speed_mmps)
        if not force:
            if abs(target - self.auto_hop_applied_ms) < AUTO_HOP_MIN_DELTA_MS:
                return
            if now - self.last_auto_hop_apply_ms < AUTO_HOP_MIN_APPLY_INTERVAL_MS:
                return
        payload = bytes([target & 0xFF, (target >> 8) & 0xFF])
        self._send_command(COMMANDS.SET_HOP_MS, payload)
        self.auto_hop_applied_ms = target
        self.last_auto_hop_apply_ms = now
        if force or now - self.last_auto_hop_log_ms >= AUTO_HOP_LOG_INTERVAL_MS:
            self.last_auto_hop_log_ms = now
            self._append_log(
                f"Auto hop {self.auto_hop_base_ms}->{target}ms @ {speed_mmps * 0.0036:.1f} km/h ({reason})"
            )

    def _resolve_csv_sample(
        self, sighting, now: int
    ) -> tuple[int, tuple[float, float, float, float]] | None:
        has_payload_gps = (
            sighting.gps_valid
            and -900_000_000 <= sighting.gps_lat_e7 <= 900_000_000
            and -1_800_000_000 <= sighting.gps_lon_e7 <= 1_800_000_000
        )
        if has_payload_gps:
            row_ts = sighting.gps_unix_time_s * 1000 if sighting.gps_unix_time_s > 0 else now
            return (
                row_ts,
                (
                    sighting.gps_lat_e7 / 10_000_000.0,
                    sighting.gps_lon_e7 / 10_000_000.0,
                    sighting.gps_alt_mm / 1000.0,
                    sighting.gps_accuracy_cm / 100.0 if sighting.gps_accuracy_cm > 0 else 0.0,
                ),
            )

        gps = self.last_esp_gps
        if not gps.valid:
            return None
        age = now - gps.received_ms
        if age < 0 or age > MAX_ESP_GPS_AGE_MS_FOR_LOGGING:
            return None
        return (
            now,
            (
                gps.lat_e7 / 10_000_000.0,
                gps.lon_e7 / 10_000_000.0,
                gps.alt_mm / 1000.0,
                gps.accuracy_cm / 100.0 if gps.accuracy_cm > 0 else 0.0,
            ),
        )

    def _maybe_append_csv_row(self, sighting, now: int) -> None:
        if not self.logging_enabled:
            return
        if sighting.flags & 0x02:
            return
        sample = self._resolve_csv_sample(sighting, now)
        if sample is None:
            if now - self.last_no_gps_log_ms >= NO_GPS_LOG_THROTTLE_MS:
                self.last_no_gps_log_ms = now
                self._append_log("CSV paused: waiting for valid ESP GPS fix")
            return
        row_ts, loc = sample
        last_write = self.last_csv_write_by_bssid.get(sighting.bssid, 0)
        if last_write > 0:
            if row_ts <= last_write:
                return
            if row_ts - last_write < MIN_LOG_INTERVAL_PER_BSSID_MS:
                return
        self.last_csv_write_by_bssid[sighting.bssid] = row_ts
        row = self.state.sightings.get(sighting.bssid)
        if row is None:
            return
        self.csv_logger.append(
            WigleWifiRow(
                mac=row.bssid,
                ssid=row.ssid,
                auth_mode=row.auth,
                first_seen_epoch_ms=row_ts,
                channel=row.channel,
                rssi=row.rssi,
                latitude=loc[0],
                longitude=loc[1],
                altitude_meters=loc[2],
                accuracy_meters=loc[3],
            )
        )


def run() -> int:
    app = QApplication([])
    window = DesktopWindow()
    window.show()
    return app.exec()
