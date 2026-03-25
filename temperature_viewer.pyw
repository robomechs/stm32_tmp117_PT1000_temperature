import csv
import math
import re
import sys
import time
from collections import deque
from datetime import datetime
from pathlib import Path

import pyqtgraph as pg
import serial
import serial.tools.list_ports
from PyQt6.QtCore import Qt, QTimer
from PyQt6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)


SENSOR_NAMES = ("PT1000", "TMP117")
FIELD_RE = re.compile(
    r"\b(PT1000|TMP117)(?:_(RAW|LPF))?\s*=\s*([-+]?\d+(?:\.\d+)?|error)\s*(?:C)?",
    re.IGNORECASE,
)
SINGLE_TEMP_RE = re.compile(
    r"^\s*(?:T\s*=\s*)?([-+]?\d+(?:\.\d+)?)\s*(?:C)?\s*$",
    re.IGNORECASE,
)
PORT_BAUDRATE = 115200
POLL_INTERVAL_MS = 20
DEFAULT_SAMPLE_PERIOD_S = 0.25
MAX_PLOT_POINTS = 1200
DEFAULT_X_WINDOW_S = 30.0


class LowPassFilter:
    def __init__(self):
        self.mode = "Off"
        self.cutoff_hz = 0.5
        self.sma_samples = 4
        self._ema_value = None
        self._sma_values = deque()
        self._last_timestamp = None

    def configure(self, mode: str, cutoff_hz: float, sma_samples: int) -> None:
        mode = mode.strip()
        cutoff_hz = max(0.01, float(cutoff_hz))
        sma_samples = max(1, int(sma_samples))
        if (
            mode != self.mode
            or abs(cutoff_hz - self.cutoff_hz) > 1e-9
            or sma_samples != self.sma_samples
        ):
            self.mode = mode
            self.cutoff_hz = cutoff_hz
            self.sma_samples = sma_samples
            self.reset()

    def reset(self) -> None:
        self._ema_value = None
        self._sma_values.clear()
        self._last_timestamp = None

    def apply(self, value: float, timestamp: float) -> float:
        if self.mode == "Off":
            self._last_timestamp = timestamp
            return value

        if self.mode == "EMA":
            if self._ema_value is None:
                self._ema_value = value
            else:
                if self._last_timestamp is None:
                    dt = DEFAULT_SAMPLE_PERIOD_S
                else:
                    dt = max(1e-3, timestamp - self._last_timestamp)
                rc = 1.0 / (2.0 * math.pi * self.cutoff_hz)
                alpha = dt / (rc + dt)
                self._ema_value += alpha * (value - self._ema_value)
            self._last_timestamp = timestamp
            return self._ema_value

        self._sma_values.append(value)
        while len(self._sma_values) > self.sma_samples:
            self._sma_values.popleft()
        self._last_timestamp = timestamp
        return sum(self._sma_values) / len(self._sma_values)


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Temperature Viewer")
        self.resize(1220, 760)

        self.ser = None
        self.rx_buffer = bytearray()
        self.filters = {name: LowPassFilter() for name in SENSOR_NAMES}
        self.sensor_data = {name: self._new_sensor_store() for name in SENSOR_NAMES}

        self.session_start_timestamp = None
        self.plot_start_timestamp = None
        self.total_frames = 0

        self.record_file = None
        self.record_writer = None
        self.last_record_path = None

        pg.setConfigOptions(antialias=True)

        self._build_ui()

        self.poll_timer = QTimer(self)
        self.poll_timer.timeout.connect(self.poll_serial)
        self.poll_timer.start(POLL_INTERVAL_MS)

        self.plot_timer = QTimer(self)
        self.plot_timer.timeout.connect(self.update_plot)
        self.plot_timer.start(100)

        self.refresh_ports()
        self.apply_lpf_settings()
        self.update_readouts()

    @staticmethod
    def _new_sensor_store() -> dict:
        return {
            "time": deque(maxlen=MAX_PLOT_POINTS),
            "raw": deque(maxlen=MAX_PLOT_POINTS),
            "filtered": deque(maxlen=MAX_PLOT_POINTS),
            "raw_value": None,
            "filtered_value": None,
            "error": False,
        }

    def _build_ui(self) -> None:
        central = QWidget()
        self.setCentralWidget(central)

        root = QVBoxLayout(central)
        root.setContentsMargins(6, 6, 6, 6)
        root.setSpacing(6)

        top_row = QHBoxLayout()
        top_row.setSpacing(6)
        root.addLayout(top_row)

        conn_group = QGroupBox("Connection")
        conn_layout = QGridLayout(conn_group)

        self.port_combo = QComboBox()
        self.refresh_button = QPushButton("Refresh")
        self.connect_button = QPushButton("Connect")
        self.single_sensor_combo = QComboBox()
        self.single_sensor_combo.addItems(SENSOR_NAMES)
        self.status_label = QLabel("Disconnected")
        self.status_label.setStyleSheet("font-weight: 600; color: #a02020;")

        conn_layout.addWidget(QLabel("Port"), 0, 0)
        conn_layout.addWidget(self.port_combo, 0, 1)
        conn_layout.addWidget(self.refresh_button, 0, 2)
        conn_layout.addWidget(self.connect_button, 0, 3)
        conn_layout.addWidget(QLabel("Baud"), 1, 0)
        conn_layout.addWidget(QLabel(str(PORT_BAUDRATE)), 1, 1)
        conn_layout.addWidget(QLabel("Status"), 1, 2)
        conn_layout.addWidget(self.status_label, 1, 3)
        conn_layout.addWidget(QLabel("Single line"), 2, 0)
        conn_layout.addWidget(self.single_sensor_combo, 2, 1)
        conn_layout.addWidget(QLabel("Legacy plain-value mode"), 2, 2, 1, 2)

        self.refresh_button.clicked.connect(self.refresh_ports)
        self.connect_button.clicked.connect(self.toggle_connection)
        top_row.addWidget(conn_group, 3)

        lpf_group = QGroupBox("LPF")
        lpf_layout = QGridLayout(lpf_group)

        self.lpf_mode_combo = QComboBox()
        self.lpf_mode_combo.addItems(["Off", "EMA", "SMA"])
        self.lpf_mode_combo.setCurrentText("EMA")

        self.ema_cutoff_spin = QDoubleSpinBox()
        self.ema_cutoff_spin.setRange(0.01, 20.0)
        self.ema_cutoff_spin.setDecimals(2)
        self.ema_cutoff_spin.setValue(0.50)
        self.ema_cutoff_spin.setSuffix(" Hz")

        self.sma_samples_spin = QSpinBox()
        self.sma_samples_spin.setRange(1, 128)
        self.sma_samples_spin.setValue(4)
        self.sma_samples_spin.setSuffix(" samples")

        lpf_layout.addWidget(QLabel("Mode"), 0, 0)
        lpf_layout.addWidget(self.lpf_mode_combo, 0, 1)
        lpf_layout.addWidget(QLabel("EMA cutoff"), 1, 0)
        lpf_layout.addWidget(self.ema_cutoff_spin, 1, 1)
        lpf_layout.addWidget(QLabel("SMA window"), 2, 0)
        lpf_layout.addWidget(self.sma_samples_spin, 2, 1)

        self.lpf_mode_combo.currentTextChanged.connect(self.apply_lpf_settings)
        self.ema_cutoff_spin.valueChanged.connect(self.apply_lpf_settings)
        self.sma_samples_spin.valueChanged.connect(self.apply_lpf_settings)
        top_row.addWidget(lpf_group, 2)

        view_group = QGroupBox("View")
        view_layout = QGridLayout(view_group)

        self.sensor_mode_combo = QComboBox()
        self.sensor_mode_combo.addItems(["Both", "PT1000", "TMP117"])
        self.sensor_mode_combo.currentTextChanged.connect(self._on_plot_mode_changed)

        self.show_raw_check = QCheckBox("Show raw")
        self.show_raw_check.setChecked(True)
        self.show_raw_check.toggled.connect(self._on_plot_mode_changed)

        self.show_filtered_check = QCheckBox("Show filtered")
        self.show_filtered_check.setChecked(True)
        self.show_filtered_check.toggled.connect(self._on_plot_mode_changed)

        self.follow_latest_check = QCheckBox("Follow latest")
        self.follow_latest_check.setChecked(True)
        self.follow_latest_check.toggled.connect(self.apply_plot_view)

        self.x_window_spin = QDoubleSpinBox()
        self.x_window_spin.setRange(5.0, 3600.0)
        self.x_window_spin.setDecimals(0)
        self.x_window_spin.setValue(DEFAULT_X_WINDOW_S)
        self.x_window_spin.setSuffix(" s")
        self.x_window_spin.valueChanged.connect(self.apply_plot_view)

        self.auto_range_check = QCheckBox("Auto Y")
        self.auto_range_check.setChecked(True)
        self.auto_range_check.toggled.connect(self.apply_plot_view)

        self.show_all_button = QPushButton("Show all")
        self.reset_view_button = QPushButton("Reset view")
        self.clear_plot_button = QPushButton("Clear plot")
        self.help_button = QPushButton("Help")

        self.show_all_button.clicked.connect(self.show_all_data)
        self.reset_view_button.clicked.connect(self.reset_plot_view)
        self.clear_plot_button.clicked.connect(self.clear_plot_history)
        self.help_button.clicked.connect(self.show_help)

        view_layout.addWidget(QLabel("Sensors"), 0, 0)
        view_layout.addWidget(self.sensor_mode_combo, 0, 1)
        view_layout.addWidget(self.show_raw_check, 0, 2)
        view_layout.addWidget(self.show_filtered_check, 0, 3)
        view_layout.addWidget(self.follow_latest_check, 1, 0)
        view_layout.addWidget(QLabel("Visible X"), 1, 1)
        view_layout.addWidget(self.x_window_spin, 1, 2)
        view_layout.addWidget(self.auto_range_check, 1, 3)
        view_layout.addWidget(self.show_all_button, 2, 0)
        view_layout.addWidget(self.reset_view_button, 2, 1)
        view_layout.addWidget(self.clear_plot_button, 2, 2)
        view_layout.addWidget(self.help_button, 2, 3)

        top_row.addWidget(view_group, 4)

        readout_group = QGroupBox("Readout")
        readout_layout = QGridLayout(readout_group)

        self.pt1000_raw_label = QLabel("--.--- C")
        self.pt1000_filtered_label = QLabel("--.--- C")
        self.tmp117_raw_label = QLabel("--.--- C")
        self.tmp117_filtered_label = QLabel("--.--- C")
        self.frame_counter_label = QLabel("0")

        readout_layout.addWidget(QLabel("Sensor"), 0, 0)
        readout_layout.addWidget(QLabel("Raw"), 0, 1)
        readout_layout.addWidget(QLabel("Filtered"), 0, 2)
        readout_layout.addWidget(QLabel("PT1000"), 1, 0)
        readout_layout.addWidget(self.pt1000_raw_label, 1, 1)
        readout_layout.addWidget(self.pt1000_filtered_label, 1, 2)
        readout_layout.addWidget(QLabel("TMP117"), 2, 0)
        readout_layout.addWidget(self.tmp117_raw_label, 2, 1)
        readout_layout.addWidget(self.tmp117_filtered_label, 2, 2)
        readout_layout.addWidget(QLabel("Frames"), 3, 0)
        readout_layout.addWidget(self.frame_counter_label, 3, 1)

        top_row.addWidget(readout_group, 3)

        log_group = QGroupBox("Logging")
        log_layout = QGridLayout(log_group)

        self.record_button = QPushButton("Start CSV")
        self.record_path_label = QLabel("Not recording")
        self.record_path_label.setStyleSheet("color: #505050;")
        self.record_path_label.setWordWrap(True)
        self.record_button.clicked.connect(self.toggle_recording)

        log_layout.addWidget(self.record_button, 0, 0)
        log_layout.addWidget(self.record_path_label, 1, 0)
        top_row.addWidget(log_group, 2)

        self.plot_widget = pg.PlotWidget()
        self.plot_widget.showGrid(x=True, y=True, alpha=0.2)
        self.plot_widget.setLabel("left", "Temperature", units="C")
        self.plot_widget.setLabel("bottom", "Time", units="s")
        self.plot_widget.addLegend(offset=(10, 10))
        self.plot_widget.setClipToView(True)

        self.pt1000_raw_curve = self.plot_widget.plot(
            pen=pg.mkPen(color="#9a9a9a", width=1.2, style=Qt.PenStyle.DashLine),
            name="PT1000 raw",
        )
        self.pt1000_filtered_curve = self.plot_widget.plot(
            pen=pg.mkPen(color="#c04020", width=2.4),
            name="PT1000 filtered",
        )
        self.tmp117_raw_curve = self.plot_widget.plot(
            pen=pg.mkPen(color="#8aa0c8", width=1.2, style=Qt.PenStyle.DashLine),
            name="TMP117 raw",
        )
        self.tmp117_filtered_curve = self.plot_widget.plot(
            pen=pg.mkPen(color="#207070", width=2.4),
            name="TMP117 filtered",
        )

        root.addWidget(self.plot_widget, 1)

    def apply_lpf_settings(self, *_args) -> None:
        mode = self.lpf_mode_combo.currentText()
        self.ema_cutoff_spin.setEnabled(mode == "EMA")
        self.sma_samples_spin.setEnabled(mode == "SMA")

        for filter_obj in self.filters.values():
            filter_obj.configure(
                mode,
                self.ema_cutoff_spin.value(),
                self.sma_samples_spin.value(),
            )

        self.recompute_filtered_history()

    def recompute_filtered_history(self) -> None:
        for sensor_name in SENSOR_NAMES:
            sensor_store = self.sensor_data[sensor_name]
            time_points = list(sensor_store["time"])
            raw_points = list(sensor_store["raw"])
            filter_obj = self.filters[sensor_name]
            filter_obj.reset()

            sensor_store["filtered"].clear()
            for timestamp, raw_value in zip(time_points, raw_points):
                sensor_store["filtered"].append(filter_obj.apply(raw_value, timestamp))

            if sensor_store["filtered"]:
                sensor_store["filtered_value"] = sensor_store["filtered"][-1]
            elif sensor_store["raw_value"] is not None and not sensor_store["error"]:
                sensor_store["filtered_value"] = filter_obj.apply(
                    sensor_store["raw_value"],
                    time_points[-1] if time_points else 0.0,
                )
            else:
                sensor_store["filtered_value"] = None

        self.update_readouts()
        self.update_plot()

    def _active_sensor_names(self) -> tuple[str, ...]:
        mode = self.sensor_mode_combo.currentText()
        if mode == "Both":
            return SENSOR_NAMES
        return (mode,)

    def _visible_time_range(self):
        x_min = None
        x_max = None

        for sensor_name in self._active_sensor_names():
            time_data = self.sensor_data[sensor_name]["time"]
            if not time_data:
                continue
            if x_min is None or time_data[0] < x_min:
                x_min = time_data[0]
            if x_max is None or time_data[-1] > x_max:
                x_max = time_data[-1]

        return x_min, x_max

    def apply_plot_view(self, *_args) -> None:
        view_box = self.plot_widget.getViewBox()
        follow_latest = self.follow_latest_check.isChecked()
        self.x_window_spin.setEnabled(follow_latest)
        view_box.setMouseEnabled(x=not follow_latest, y=True)

        if self.auto_range_check.isChecked():
            self.plot_widget.enableAutoRange(axis="y", enable=True)
        else:
            self.plot_widget.enableAutoRange(axis="y", enable=False)

        if not follow_latest:
            return

        x_min, x_max = self._visible_time_range()
        if x_min is None or x_max is None:
            return

        span = max(1.0, self.x_window_spin.value())
        x_min = max(0.0, x_max - span)
        if x_max <= x_min:
            x_max = x_min + 1.0
        self.plot_widget.setXRange(x_min, x_max, padding=0.02)

    def show_all_data(self) -> None:
        x_min, x_max = self._visible_time_range()
        if x_min is None or x_max is None:
            return

        self.follow_latest_check.setChecked(False)
        if x_max <= x_min:
            x_max = x_min + 1.0
        self.plot_widget.setXRange(x_min, x_max, padding=0.02)

    def reset_plot_view(self) -> None:
        self.auto_range_check.setChecked(True)
        self.follow_latest_check.setChecked(True)
        self.apply_plot_view()

    def _clear_all_curves(self) -> None:
        self.pt1000_raw_curve.setData([], [])
        self.pt1000_filtered_curve.setData([], [])
        self.tmp117_raw_curve.setData([], [])
        self.tmp117_filtered_curve.setData([], [])

    def clear_plot_history(self) -> None:
        for sensor_name in SENSOR_NAMES:
            sensor_store = self.sensor_data[sensor_name]
            sensor_store["time"].clear()
            sensor_store["raw"].clear()
            sensor_store["filtered"].clear()
            sensor_store["raw_value"] = None
            sensor_store["filtered_value"] = None
            sensor_store["error"] = False

        for filter_obj in self.filters.values():
            filter_obj.reset()

        self.rx_buffer.clear()
        self.plot_start_timestamp = None
        self.total_frames = 0
        self._clear_all_curves()
        self.update_readouts()
        self.apply_plot_view()

    def show_help(self) -> None:
        QMessageBox.information(
            self,
            "Viewer help",
            "1. Select COM and click Connect.\n"
            "2. The viewer LPF is software-side again. Tune it in the LPF box.\n"
            "3. Show raw and Show filtered hide or show those traces independently.\n"
            "4. Sensors = Both / PT1000 / TMP117 limits which sensors are plotted.\n"
            "5. Follow latest keeps the newest data on screen. Turn it off for manual pan/zoom.\n"
            "6. Show all fits history. Clear plot restarts X from zero.\n"
            "7. Single line is only for old plain-value firmware mode.\n\n"
            "Accepted examples:\n"
            "PT1000=25.020 C TMP117=24.980 C\n"
            "PT1000=25.020 C\n"
            "25.020 C",
        )

    def refresh_ports(self) -> None:
        current_port = self.port_combo.currentData()
        self.port_combo.clear()

        preferred_index = -1
        ports = list(serial.tools.list_ports.comports())
        for index, port in enumerate(ports):
            self.port_combo.addItem(port.device, port.device)
            self.port_combo.setItemData(
                index,
                f"{port.device} | {port.description}",
                Qt.ItemDataRole.ToolTipRole,
            )

            desc = f"{port.description} {port.hwid}".lower()
            if current_port and port.device == current_port:
                preferred_index = index
            elif preferred_index < 0 and "ch340" in desc:
                preferred_index = index
            elif preferred_index < 0 and "usb serial" in desc:
                preferred_index = index

        if preferred_index >= 0:
            self.port_combo.setCurrentIndex(preferred_index)

        self.connect_button.setEnabled(self.port_combo.count() > 0 or self.ser is not None)

    def toggle_connection(self) -> None:
        if self.ser is not None:
            self.disconnect_serial()
            return

        port = self.port_combo.currentData()
        if not port:
            QMessageBox.warning(self, "No COM port", "Select a COM port first.")
            return

        try:
            self.ser = serial.Serial(port=port, baudrate=PORT_BAUDRATE, timeout=0)
        except Exception as exc:
            QMessageBox.critical(self, "Connection failed", str(exc))
            self.ser = None
            self.set_status("Disconnected", "#a02020")
            return

        self.session_start_timestamp = None
        self.clear_plot_history()
        self.connect_button.setText("Disconnect")
        self.set_status(f"Connected: {port}", "#206020")

    def disconnect_serial(self) -> None:
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self.connect_button.setText("Connect")
        self.set_status("Disconnected", "#a02020")

    def set_status(self, text: str, color: str) -> None:
        self.status_label.setText(text)
        self.status_label.setStyleSheet(f"font-weight: 600; color: {color};")

    def poll_serial(self) -> None:
        if self.ser is None:
            return

        try:
            waiting = self.ser.in_waiting
            if waiting <= 0:
                return
            self.rx_buffer.extend(self.ser.read(waiting))
        except Exception as exc:
            self.disconnect_serial()
            QMessageBox.critical(self, "Serial error", str(exc))
            return

        while True:
            newline_index = self.rx_buffer.find(b"\n")
            if newline_index < 0:
                break

            line_bytes = self.rx_buffer[:newline_index]
            del self.rx_buffer[: newline_index + 1]
            line = line_bytes.decode("ascii", errors="ignore").strip()
            if line:
                self.handle_line(line)

        if len(self.rx_buffer) > 2048:
            del self.rx_buffer[:-256]

    def parse_line(self, line: str):
        parsed = {}
        field_matches = FIELD_RE.findall(line)

        if field_matches:
            for sensor_name, field_name, value_text in field_matches:
                sensor_key = sensor_name.upper()
                sensor_entry = parsed.setdefault(sensor_key, {})
                field_key = (field_name or "VALUE").upper()
                sensor_entry[field_key] = None if value_text.lower() == "error" else float(value_text)

            normalized = {}
            for sensor_name, sensor_entry in parsed.items():
                if "RAW" in sensor_entry:
                    normalized[sensor_name] = sensor_entry["RAW"]
                elif "VALUE" in sensor_entry:
                    normalized[sensor_name] = sensor_entry["VALUE"]
                elif "LPF" in sensor_entry:
                    normalized[sensor_name] = sensor_entry["LPF"]

            return normalized if normalized else None

        match = SINGLE_TEMP_RE.match(line)
        if match:
            return {self.single_sensor_combo.currentText(): float(match.group(1))}

        return None

    def handle_line(self, line: str) -> None:
        parsed = self.parse_line(line)
        if not parsed:
            return

        timestamp = time.monotonic()
        if self.session_start_timestamp is None:
            self.session_start_timestamp = timestamp
        if self.plot_start_timestamp is None:
            self.plot_start_timestamp = timestamp

        session_elapsed_s = timestamp - self.session_start_timestamp
        plot_elapsed_s = timestamp - self.plot_start_timestamp
        csv_row = [f"{session_elapsed_s:.3f}"]

        for sensor_name in SENSOR_NAMES:
            raw_value = parsed.get(sensor_name, "missing")
            sensor_store = self.sensor_data[sensor_name]

            if raw_value == "missing":
                csv_row.extend(["", ""])
                continue

            if raw_value is None:
                sensor_store["raw_value"] = None
                sensor_store["filtered_value"] = None
                sensor_store["error"] = True
                csv_row.extend(["error", "error"])
                continue

            filtered_value = self.filters[sensor_name].apply(raw_value, plot_elapsed_s)
            sensor_store["time"].append(plot_elapsed_s)
            sensor_store["raw"].append(raw_value)
            sensor_store["filtered"].append(filtered_value)
            sensor_store["raw_value"] = raw_value
            sensor_store["filtered_value"] = filtered_value
            sensor_store["error"] = False

            csv_row.extend([f"{raw_value:.3f}", f"{filtered_value:.3f}"])

        self.total_frames += 1
        self.update_readouts()

        if self.record_writer is not None and self.record_file is not None:
            self.record_writer.writerow(csv_row)
            self.record_file.flush()

    @staticmethod
    def _format_value(value, is_error: bool) -> str:
        if is_error:
            return "error"
        if value is None:
            return "--.--- C"
        return f"{value:.3f} C"

    def update_readouts(self) -> None:
        self.pt1000_raw_label.setText(
            self._format_value(
                self.sensor_data["PT1000"]["raw_value"],
                self.sensor_data["PT1000"]["error"],
            )
        )
        self.pt1000_filtered_label.setText(
            self._format_value(
                self.sensor_data["PT1000"]["filtered_value"],
                self.sensor_data["PT1000"]["error"],
            )
        )
        self.tmp117_raw_label.setText(
            self._format_value(
                self.sensor_data["TMP117"]["raw_value"],
                self.sensor_data["TMP117"]["error"],
            )
        )
        self.tmp117_filtered_label.setText(
            self._format_value(
                self.sensor_data["TMP117"]["filtered_value"],
                self.sensor_data["TMP117"]["error"],
            )
        )
        self.frame_counter_label.setText(str(self.total_frames))

    def _on_plot_mode_changed(self, *_args) -> None:
        self.update_plot()
        self.apply_plot_view()

    def update_plot(self) -> None:
        active_names = set(self._active_sensor_names())
        show_raw = self.show_raw_check.isChecked()
        show_filtered = self.show_filtered_check.isChecked()

        for sensor_name, raw_curve, filtered_curve in (
            ("PT1000", self.pt1000_raw_curve, self.pt1000_filtered_curve),
            ("TMP117", self.tmp117_raw_curve, self.tmp117_filtered_curve),
        ):
            sensor_store = self.sensor_data[sensor_name]
            if sensor_name in active_names and show_raw:
                raw_curve.setData(list(sensor_store["time"]), list(sensor_store["raw"]))
            else:
                raw_curve.setData([], [])

            if sensor_name in active_names and show_filtered:
                filtered_curve.setData(
                    list(sensor_store["time"]),
                    list(sensor_store["filtered"]),
                )
            else:
                filtered_curve.setData([], [])

        self.apply_plot_view()

    def toggle_recording(self) -> None:
        if self.record_file is not None:
            self.stop_recording()
            return

        default_name = f"temperature_viewer_log_{datetime.now():%Y%m%d_%H%M%S}.csv"
        default_path = str(Path(__file__).resolve().parent / default_name)
        file_path, _ = QFileDialog.getSaveFileName(
            self,
            "Save CSV",
            default_path,
            "CSV files (*.csv)",
        )
        if not file_path:
            return

        try:
            self.record_file = open(file_path, "w", newline="", encoding="ascii")
        except OSError as exc:
            QMessageBox.critical(self, "CSV error", str(exc))
            self.record_file = None
            return

        self.record_writer = csv.writer(self.record_file)
        self.record_writer.writerow(
            [
                "timestamp_s",
                "pt1000_raw_c",
                "pt1000_filtered_c",
                "tmp117_raw_c",
                "tmp117_filtered_c",
            ]
        )
        self.last_record_path = file_path
        self.record_button.setText("Stop CSV")
        self.record_path_label.setText(file_path)
        self.record_path_label.setToolTip(file_path)

    def stop_recording(self) -> None:
        if self.record_file is not None:
            try:
                self.record_file.close()
            except Exception:
                pass

        self.record_file = None
        self.record_writer = None
        self.record_button.setText("Start CSV")
        if self.last_record_path:
            self.record_path_label.setText(f"Saved: {self.last_record_path}")
            self.record_path_label.setToolTip(self.last_record_path)
        else:
            self.record_path_label.setText("Not recording")
            self.record_path_label.setToolTip("")

    def closeEvent(self, event) -> None:
        self.stop_recording()
        self.disconnect_serial()
        super().closeEvent(event)


def main() -> int:
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
