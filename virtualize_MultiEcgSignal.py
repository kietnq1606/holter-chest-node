import math
import re
import sys
from collections import deque
from pathlib import Path

from PyQt5.QtCore import QIODevice, QPointF, Qt, QTimer
from PyQt5.QtGui import QColor, QFont, QPainter, QPainterPath, QPen
from PyQt5.QtSerialPort import QSerialPort, QSerialPortInfo
from PyQt5.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QFileDialog,
    QFrame,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

SAMPLE_RATE_HZ = 500
DEFAULT_WINDOW_SECONDS = 1.0  # Stretched default: 1.0s (500 samples) instead of 5.0s
MIN_WINDOW_SECONDS = 0.05     # Max X zoom-in: 0.05s (25 samples)
MAX_WINDOW_SECONDS = 10.0     # Max X zoom-out: 10.0s (5000 samples)
MIN_Y_ZOOM = 0.1              # Max Y zoom-out (0.1x)
MAX_Y_ZOOM = 50.0             # Max Y zoom-in (50x)
MAX_BUFFER_SAMPLES = int(SAMPLE_RATE_HZ * MAX_WINDOW_SECONDS)

# SAADC & AD8232 hardware parameters per datasheet
ADC_MV_PER_LSB = 3600.0 / 4096.0   # 0.87890625 mV/LSB (Vref=0.6V, Gain=1/6, 12-bit)
AD8232_BASELINE_MV = 1676.0        # REFOUT = Vs / 2 = 1.65 V (1650 mV) at Vs = 3.3 V
AD8232_IA_GAIN = 100.0             # Instrumentation Amplifier fixed gain = 100 V/V
AD8232_OPAMP_GAIN = 11.0           # Op-Amp A1 gain = 11 V/V (datasheet standard cardiac monitor)
AD8232_TOTAL_GAIN = AD8232_IA_GAIN * AD8232_OPAMP_GAIN  # Total gain = 1100 V/V


def raw_lsb_to_ecg_mv(raw_lsb):
    """Convert 12-bit ADC raw count to input-referred ECG voltage (mV)."""
    adc_mv = raw_lsb * ADC_MV_PER_LSB
    return (adc_mv - AD8232_BASELINE_MV) / AD8232_TOTAL_GAIN


class BiquadSection:
    """2nd-order IIR Biquad filter section (Direct Form II Transposed) with transient-free init."""

    def __init__(self, b0, b1, b2, a1, a2):
        self.b0 = b0
        self.b1 = b1
        self.b2 = b2
        self.a1 = a1
        self.a2 = a2
        denom = 1.0 + a1 + a2
        self.dc_gain = (b0 + b1 + b2) / denom if abs(denom) > 1e-12 else 0.0
        self.z1 = 0.0
        self.z2 = 0.0
        self.initialized = False

    def reset(self):
        self.z1 = 0.0
        self.z2 = 0.0
        self.initialized = False

    def process(self, x):
        if not self.initialized:
            y0 = self.dc_gain * x
            self.z2 = self.b2 * x - self.a2 * y0
            self.z1 = self.b1 * x - self.a1 * y0 + self.z2
            self.initialized = True
        y = self.b0 * x + self.z1
        self.z1 = self.b1 * x - self.a1 * y + self.z2
        self.z2 = self.b2 * x - self.a2 * y
        return y


class MovingAverageSection:
    """Single-stage moving average smoother."""

    def __init__(self, window_size=5):
        self.window_size = max(2, int(window_size))
        self.buf = deque(maxlen=self.window_size)

    def reset(self):
        self.buf.clear()

    def process(self, x):
        self.buf.append(x)
        return sum(self.buf) / len(self.buf)


def make_biquad_lowpass(fs, cutoff_hz, q=0.70710678):
    w0 = 2.0 * math.pi * cutoff_hz / fs
    alpha = math.sin(w0) / (2.0 * q)
    c = math.cos(w0)
    b0 = (1.0 - c) * 0.5
    b1 = 1.0 - c
    b2 = (1.0 - c) * 0.5
    a0 = 1.0 + alpha
    a1 = -2.0 * c
    a2 = 1.0 - alpha
    return BiquadSection(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def make_biquad_highpass(fs, cutoff_hz, q=0.70710678):
    w0 = 2.0 * math.pi * cutoff_hz / fs
    alpha = math.sin(w0) / (2.0 * q)
    c = math.cos(w0)
    b0 = (1.0 + c) * 0.5
    b1 = -(1.0 + c)
    b2 = (1.0 + c) * 0.5
    a0 = 1.0 + alpha
    a1 = -2.0 * c
    a2 = 1.0 - alpha
    return BiquadSection(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def make_biquad_notch(fs, notch_hz, q=8.0):
    w0 = 2.0 * math.pi * notch_hz / fs
    alpha = math.sin(w0) / (2.0 * q)
    c = math.cos(w0)
    b0 = 1.0
    b1 = -2.0 * c
    b2 = 1.0
    a0 = 1.0 + alpha
    a1 = -2.0 * c
    a2 = 1.0 - alpha
    return BiquadSection(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


class EcgFilterChain:
    """Configurable multi-stage digital filter cascade for 500 Hz ECG signals."""

    def __init__(self, filter_type="ecg_full", stages=2, fs=SAMPLE_RATE_HZ):
        self.filter_type = filter_type
        self.stages = max(1, int(stages))
        self.fs = fs
        self.sections = []
        self._build_sections()

    def _build_sections(self):
        self.sections = []
        if self.filter_type == "none":
            return

        for _ in range(self.stages):
            if self.filter_type == "ecg_full":
                # Monitor ECG chain: HPF (0.5 Hz) -> Notch (50 Hz) -> LPF (40 Hz)
                self.sections.append(make_biquad_highpass(self.fs, 0.5))
                self.sections.append(make_biquad_notch(self.fs, 50.0, q=6.0))
                self.sections.append(make_biquad_lowpass(self.fs, 40.0))
            elif self.filter_type == "ecg_diag_full":
                # Diagnostic ECG chain: HPF (0.05 Hz) -> Notch (50 Hz) -> LPF (150 Hz)
                self.sections.append(make_biquad_highpass(self.fs, 0.05))
                self.sections.append(make_biquad_notch(self.fs, 50.0, q=6.0))
                self.sections.append(make_biquad_lowpass(self.fs, 150.0))
            elif self.filter_type == "bandpass":
                self.sections.append(make_biquad_highpass(self.fs, 0.5))
                self.sections.append(make_biquad_lowpass(self.fs, 40.0))
            elif self.filter_type == "bandpass_diag":
                self.sections.append(make_biquad_highpass(self.fs, 0.05))
                self.sections.append(make_biquad_lowpass(self.fs, 150.0))
            elif self.filter_type == "lowpass":
                self.sections.append(make_biquad_lowpass(self.fs, 40.0))
            elif self.filter_type == "lowpass_150":
                self.sections.append(make_biquad_lowpass(self.fs, 150.0))
            elif self.filter_type == "highpass":
                self.sections.append(make_biquad_highpass(self.fs, 0.5))
            elif self.filter_type == "highpass_005":
                self.sections.append(make_biquad_highpass(self.fs, 0.05))
            elif self.filter_type == "notch50":
                self.sections.append(make_biquad_notch(self.fs, 50.0, q=6.0))
            elif self.filter_type == "moving_avg":
                self.sections.append(MovingAverageSection(window_size=7))

    def configure(self, filter_type, stages):
        self.filter_type = filter_type
        self.stages = max(1, int(stages))
        self._build_sections()

    def reset(self):
        for sec in self.sections:
            sec.reset()

    def process_sample(self, x):
        y = x
        for sec in self.sections:
            y = sec.process(y)
        return y

    def process_block(self, samples):
        if not self.sections:
            return list(samples)
        return [self.process_sample(s) for s in samples]

# Strip ANSI escape sequences and Zephyr "--- N messages dropped ---" warnings
ANSI_ESCAPE_RE = re.compile(r"\x1b\[[0-9;]*[a-zA-Z]|\[\d+(?:;\d+)*m")
DROPPED_MSG_RE = re.compile(r"---\s*\d+\s+messages\s+dropped\s*---", re.IGNORECASE)

# Matches header printed by filter_raw_test.c or ad8232_test.c:
# [BUF #0001 | 100 ms] LO+=0 LO-=0 (OK) | n=50 | RAW(min=1860 max=1945 mean=1895) | FILT(min=1875 max=1912 mean=1892) LSB
BUF_HEADER_RE = re.compile(
    r"\[BUF\s+#(\d+)\s*\|\s*(\d+)\s*ms\]\s*"
    r"LO\+=([01])\s+LO-=([01])\s*\(([^)]+)\)"
    r"(?:\s*\|\s*n=(\d+))?"
    r"(?:\s*\|\s*RAW\(min=(-?\d+)\s+max=(-?\d+)\s+mean=(-?\d+)\)\s*\|\s*FILT\(min=(-?\d+)\s+max=(-?\d+)\s+mean=(-?\d+)\))?"
)
INT_TOKEN_RE = re.compile(r"-?\d+")


class EcgPlotWidget(QWidget):
    """High-speed 500 Hz dual-channel overlaid ECG waveform canvas rendered with QPainter."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumHeight(360)
        self.window_seconds = DEFAULT_WINDOW_SECONDS
        self.y_zoom = 1.0
        self.raw_samples_mv = deque(maxlen=MAX_BUFFER_SAMPLES)
        self.filt_samples_mv = deque(maxlen=MAX_BUFFER_SAMPLES)
        self.show_raw = True
        self.show_filt = True
        self.on_zoom_changed = None

    @property
    def visible_sample_count(self):
        return max(10, int(round(SAMPLE_RATE_HZ * self.window_seconds)))

    def set_window_seconds(self, seconds):
        clamped = max(MIN_WINDOW_SECONDS, min(MAX_WINDOW_SECONDS, seconds))
        self.window_seconds = round(clamped, 2)
        if self.on_zoom_changed:
            self.on_zoom_changed(self.window_seconds)
        self.update()

    def set_y_zoom(self, zoom):
        self.y_zoom = max(MIN_Y_ZOOM, min(MAX_Y_ZOOM, zoom))
        self.update()

    def zoom_in(self):
        if self.window_seconds <= 0.2:
            step = 0.05
        elif self.window_seconds <= 1.0:
            step = 0.1
        else:
            step = 0.5
        self.set_window_seconds(self.window_seconds - step)

    def zoom_out(self):
        if self.window_seconds < 0.2:
            step = 0.05
        elif self.window_seconds < 1.0:
            step = 0.1
        else:
            step = 0.5
        self.set_window_seconds(self.window_seconds + step)

    def zoom_y_in(self):
        self.set_y_zoom(self.y_zoom * 1.4)

    def zoom_y_out(self):
        self.set_y_zoom(self.y_zoom / 1.4)

    def reset_zoom(self):
        self.y_zoom = 1.0
        self.set_window_seconds(DEFAULT_WINDOW_SECONDS)

    def wheelEvent(self, event):
        modifiers = event.modifiers()
        if modifiers & (Qt.ControlModifier | Qt.ShiftModifier):
            if event.angleDelta().y() > 0:
                self.zoom_y_in()
            elif event.angleDelta().y() < 0:
                self.zoom_y_out()
        else:
            if event.angleDelta().y() > 0:
                self.zoom_in()
            elif event.angleDelta().y() < 0:
                self.zoom_out()
        event.accept()

    def append_raw_samples(self, values_mv):
        self.raw_samples_mv.extend(values_mv)
        self.update()

    def append_filt_samples(self, values_mv):
        self.filt_samples_mv.extend(values_mv)
        self.update()

    def clear_samples(self):
        self.raw_samples_mv.clear()
        self.filt_samples_mv.clear()
        self.update()

    @staticmethod
    def _build_waveform_path(visible_samples, vis_max, plot_x, plot_y, plot_w, plot_h, y_min, y_span):
        count = len(visible_samples)
        start_offset = vis_max - count
        path = QPainterPath()
        for idx, val in enumerate(visible_samples):
            x_norm = (start_offset + idx) / max(1, vis_max - 1)
            y_norm = (val - y_min) / y_span
            px = plot_x + x_norm * plot_w
            py = plot_y + plot_h - y_norm * plot_h
            if idx == 0:
                path.moveTo(QPointF(px, py))
            else:
                path.lineTo(QPointF(px, py))
        return path

    def paintEvent(self, event):
        del event
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing, True)

        rect = self.rect()
        painter.fillRect(rect, QColor("#11161d"))

        left_margin = 92
        right_margin = 20
        top_margin = 20
        bottom_margin = 36

        plot_w = max(10, rect.width() - left_margin - right_margin)
        plot_h = max(10, rect.height() - top_margin - bottom_margin)
        plot_x = left_margin
        plot_y = top_margin

        vis_max = self.visible_sample_count
        raw_all = list(self.raw_samples_mv)
        filt_all = list(self.filt_samples_mv)
        vis_raw = raw_all[-vis_max:] if (raw_all and self.show_raw) else []
        vis_filt = filt_all[-vis_max:] if (filt_all and self.show_filt) else []

        combined_visible = vis_raw + vis_filt

        # Compute shared Y-axis range so both signals are overlaid on the same scale
        if combined_visible:
            min_v = min(combined_visible)
            max_v = max(combined_visible)
            mid = 0.5 * (min_v + max_v)
            span = max(max_v - min_v, 0.08)
            half_range = (0.5 * span * 1.25) / self.y_zoom
            y_min = mid - half_range
            y_max = mid + half_range
        else:
            half_range = 1.5 / self.y_zoom
            y_min = -half_range
            y_max = half_range

        minor_pen = QPen(QColor("#1b2430"), 1, Qt.SolidLine)
        major_pen = QPen(QColor("#283546"), 1, Qt.DashLine)
        border_pen = QPen(QColor("#3b4d63"), 1, Qt.SolidLine)
        label_color = QColor("#9fb0c3")

        font = QFont("Consolas", 9)
        painter.setFont(font)

        # Minor vertical divisions (25 divisions across the visible window)
        minor_x_divs = 25
        for i in range(1, minor_x_divs):
            x = plot_x + (plot_w * i) / minor_x_divs
            painter.setPen(minor_pen)
            painter.drawLine(int(x), plot_y, int(x), plot_y + plot_h)

        # Minor horizontal divisions (20 divisions)
        minor_y_divs = 20
        for i in range(1, minor_y_divs):
            y = plot_y + (plot_h * i) / minor_y_divs
            painter.setPen(minor_pen)
            painter.drawLine(plot_x, int(y), plot_x + plot_w, int(y))

        # Major horizontal grid + Y labels (6 levels)
        y_ticks = 5
        for i in range(y_ticks + 1):
            frac = i / y_ticks
            y = plot_y + frac * plot_h
            val = y_max - frac * (y_max - y_min)

            if 0 < i < y_ticks:
                painter.setPen(major_pen)
                painter.drawLine(plot_x, int(y), plot_x + plot_w, int(y))

            painter.setPen(label_color)
            painter.drawText(
                4,
                int(y) - 10,
                left_margin - 12,
                20,
                Qt.AlignRight | Qt.AlignVCenter,
                f"{val:+.3f} mV",
            )

        # Major vertical grid + X time labels
        x_ticks = 5
        for i in range(x_ticks + 1):
            frac = i / x_ticks
            x = plot_x + frac * plot_w
            sec = -self.window_seconds + frac * self.window_seconds

            if 0 < i < x_ticks:
                painter.setPen(major_pen)
                painter.drawLine(int(x), plot_y, int(x), plot_y + plot_h)

            painter.setPen(label_color)
            painter.drawText(
                int(x) - 35,
                plot_y + plot_h + 8,
                70,
                22,
                Qt.AlignHCenter | Qt.AlignTop,
                f"{sec:.2f}s",
            )

        # Plot border
        painter.setPen(border_pen)
        painter.drawRect(plot_x, plot_y, plot_w, plot_h)

        # Draw overlaid ECG waveforms (RAW first, FILT on top)
        has_drawn = False
        y_span = max(1e-6, y_max - y_min)

        if len(vis_raw) >= 2:
            raw_path = self._build_waveform_path(
                vis_raw, vis_max, plot_x, plot_y, plot_w, plot_h, y_min, y_span
            )
            raw_pen = QPen(QColor("#ff9100"), 1.5, Qt.SolidLine)
            painter.setPen(raw_pen)
            painter.drawPath(raw_path)
            has_drawn = True

        if len(vis_filt) >= 2:
            filt_path = self._build_waveform_path(
                vis_filt, vis_max, plot_x, plot_y, plot_w, plot_h, y_min, y_span
            )
            filt_pen = QPen(QColor("#00e5ff"), 1.7, Qt.SolidLine)
            painter.setPen(filt_pen)
            painter.drawPath(filt_path)
            has_drawn = True

        # Top-right overlay legend
        legend_x = plot_x + plot_w - 220
        legend_y = plot_y + 10
        painter.fillRect(legend_x, legend_y, 210, 44, QColor(17, 22, 29, 210))
        painter.setPen(QPen(QColor("#3b4d63"), 1))
        painter.drawRect(legend_x, legend_y, 210, 44)

        painter.setPen(QPen(QColor("#ff9100"), 2.5))
        painter.drawLine(legend_x + 10, legend_y + 14, legend_x + 32, legend_y + 14)
        painter.setPen(QColor("#ff9100"))
        painter.drawText(legend_x + 40, legend_y + 6, 165, 16, Qt.AlignLeft | Qt.AlignVCenter, "RAW (P0.29 / AIN5)")

        painter.setPen(QPen(QColor("#00e5ff"), 2.5))
        painter.drawLine(legend_x + 10, legend_y + 32, legend_x + 32, legend_y + 32)
        painter.setPen(QColor("#00e5ff"))
        painter.drawText(legend_x + 40, legend_y + 24, 165, 16, Qt.AlignLeft | Qt.AlignVCenter, "FILT (P0.02 / AIN0)")

        if not has_drawn:
            painter.setPen(QColor("#63758a"))
            painter.drawText(
                plot_x,
                plot_y,
                plot_w,
                plot_h,
                Qt.AlignCenter,
                "Waiting for AD8232 RAW + FILT stream data...",
            )


class EcgVisualizerWindow(QMainWindow):
    def __init__(self):
        super().__init__()

        self.serial = QSerialPort(self)
        self.rx_bytes = 0
        self.rx_line_buffer = ""
        self.packet_counter = 0
        self.paused = False
        self.raw_samples_history = deque(maxlen=MAX_BUFFER_SAMPLES)
        self.filt_samples_history = deque(maxlen=MAX_BUFFER_SAMPLES)
        self.raw_filter_chain = EcgFilterChain(filter_type="ecg_full", stages=2)
        self.filt_filter_chain = EcgFilterChain(filter_type="ecg_full", stages=2)

        self.saved_port_name = None
        self.saved_baud_rate = None
        self.auto_reconnect_enabled = False

        self.reconnect_timer = QTimer(self)
        self.reconnect_timer.setInterval(1000)
        self.reconnect_timer.timeout.connect(self.try_reconnect)

        self.setWindowTitle("AD8232 RAW vs FILTER OVERLAY VISUALIZER (500 Hz)")
        self.resize(1120, 680)
        self.setMinimumSize(820, 480)

        self._build_ui()
        self._connect_signals()
        self.refresh_ports()
        self._set_connected_ui(False)

    def _build_ui(self):
        central = QWidget(self)
        self.setCentralWidget(central)

        root_layout = QVBoxLayout(central)
        root_layout.setContentsMargins(12, 12, 12, 12)
        root_layout.setSpacing(8)

        # Row 1: Connection & Control Bar
        top_bar = QHBoxLayout()
        top_bar.setSpacing(8)

        top_bar.addWidget(QLabel("Port"))
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(160)
        top_bar.addWidget(self.port_combo)

        top_bar.addSpacing(6)
        top_bar.addWidget(QLabel("Baud"))
        self.baud_combo = QComboBox()
        self.baud_combo.setMinimumWidth(100)
        for baud in (
            9600,
            19200,
            38400,
            57600,
            115200,
            230400,
            460800,
            921600,
        ):
            self.baud_combo.addItem(str(baud), baud)
        default_idx = self.baud_combo.findData(115200)
        if default_idx >= 0:
            self.baud_combo.setCurrentIndex(default_idx)
        top_bar.addWidget(self.baud_combo)

        self.refresh_button = QPushButton("Refresh")
        self.connect_button = QPushButton("Connect")
        top_bar.addWidget(self.refresh_button)
        top_bar.addWidget(self.connect_button)

        top_bar.addSpacing(10)
        top_bar.addWidget(QLabel("Filter"))
        self.filter_combo = QComboBox()
        self.filter_combo.setMinimumWidth(235)
        self.filter_combo.addItem("ECG Monitor Full (0.5 - 40Hz + Notch 50Hz)", "ecg_full")
        self.filter_combo.addItem("ECG Diagnostic Full (0.05 - 150Hz + Notch 50Hz)", "ecg_diag_full")
        self.filter_combo.addItem("Bandpass Monitor (0.5 - 40 Hz)", "bandpass")
        self.filter_combo.addItem("Bandpass Diagnostic (0.05 - 150 Hz)", "bandpass_diag")
        self.filter_combo.addItem("Low-pass (40 Hz)", "lowpass")
        self.filter_combo.addItem("Low-pass (150 Hz)", "lowpass_150")
        self.filter_combo.addItem("High-pass (0.5 Hz - Zero DC)", "highpass")
        self.filter_combo.addItem("High-pass (0.05 Hz - Zero DC)", "highpass_005")
        self.filter_combo.addItem("Notch (50 Hz)", "notch50")
        self.filter_combo.addItem("Moving Average", "moving_avg")
        self.filter_combo.addItem("None (Raw)", "none")
        top_bar.addWidget(self.filter_combo)

        top_bar.addSpacing(4)
        top_bar.addWidget(QLabel("Stages"))
        self.stages_combo = QComboBox()
        self.stages_combo.setMinimumWidth(80)
        for st in range(1, 21):
            self.stages_combo.addItem(f"{st} tầng", st)
        self.stages_combo.setCurrentIndex(1)  # Default: 2 stages
        top_bar.addWidget(self.stages_combo)

        top_bar.addStretch(1)

        self.open_log_button = QPushButton("Open Log")
        self.zoom_in_button = QPushButton("X Zoom +")
        self.zoom_out_button = QPushButton("X Zoom -")
        self.zoom_y_in_button = QPushButton("Y Zoom +")
        self.zoom_y_out_button = QPushButton("Y Zoom -")
        self.reset_zoom_button = QPushButton("Fit")
        self.pause_button = QPushButton("Pause")
        self.clear_button = QPushButton("Clear")
        top_bar.addWidget(self.open_log_button)
        top_bar.addWidget(self.zoom_in_button)
        top_bar.addWidget(self.zoom_out_button)
        top_bar.addWidget(self.zoom_y_in_button)
        top_bar.addWidget(self.zoom_y_out_button)
        top_bar.addWidget(self.reset_zoom_button)
        top_bar.addWidget(self.pause_button)
        top_bar.addWidget(self.clear_button)

        root_layout.addLayout(top_bar)

        # Row 2: Telemetry & Voltage Statistics Bar
        info_frame = QFrame()
        info_frame.setFrameShape(QFrame.StyledPanel)
        info_layout = QVBoxLayout(info_frame)
        info_layout.setContentsMargins(10, 6, 10, 6)
        info_layout.setSpacing(4)

        status_row = QHBoxLayout()
        self.conn_status_label = QLabel("○ DISCONNECTED")
        self.conn_status_label.setStyleSheet("font-weight: 600;")

        self.rx_count_label = QLabel("RX: 0 B")
        self.buffer_info_label = QLabel("Buffer: --")
        self.lead_status_label = QLabel("Leads: --")
        self.lead_status_label.setStyleSheet("font-weight: 600;")

        status_row.addWidget(self.conn_status_label)
        status_row.addSpacing(20)
        status_row.addWidget(self.rx_count_label)
        status_row.addSpacing(20)
        status_row.addWidget(self.buffer_info_label)
        status_row.addSpacing(20)
        status_row.addWidget(self.lead_status_label)
        status_row.addSpacing(24)

        self.show_raw_check = QCheckBox("Show RAW (P0.29)")
        self.show_raw_check.setChecked(True)
        self.show_raw_check.setStyleSheet("color: #d97706; font-weight: 600;")
        self.show_filt_check = QCheckBox("Show FILT (P0.02)")
        self.show_filt_check.setChecked(True)
        self.show_filt_check.setStyleSheet("color: #0284c7; font-weight: 600;")
        status_row.addWidget(self.show_raw_check)
        status_row.addSpacing(12)
        status_row.addWidget(self.show_filt_check)
        status_row.addStretch(1)

        raw_stats_row = QHBoxLayout()
        raw_tag = QLabel("RAW (P0.29):")
        raw_tag.setMinimumWidth(95)
        raw_tag.setStyleSheet("color: #d97706; font-weight: 700;")
        self.raw_vmin_label = QLabel("V_min: -- mV")
        self.raw_vmax_label = QLabel("V_max: -- mV")
        self.raw_vmean_label = QLabel("V_mean: -- mV")
        self.raw_vpp_label = QLabel("V_pp: -- mV")
        raw_stats_row.addWidget(raw_tag)
        raw_stats_row.addWidget(self.raw_vmin_label)
        raw_stats_row.addSpacing(24)
        raw_stats_row.addWidget(self.raw_vmax_label)
        raw_stats_row.addSpacing(24)
        raw_stats_row.addWidget(self.raw_vmean_label)
        raw_stats_row.addSpacing(24)
        raw_stats_row.addWidget(self.raw_vpp_label)
        raw_stats_row.addStretch(1)

        filt_stats_row = QHBoxLayout()
        filt_tag = QLabel("FILT (P0.02):")
        filt_tag.setMinimumWidth(95)
        filt_tag.setStyleSheet("color: #0284c7; font-weight: 700;")
        self.filt_vmin_label = QLabel("V_min: -- mV")
        self.filt_vmax_label = QLabel("V_max: -- mV")
        self.filt_vmean_label = QLabel("V_mean: -- mV")
        self.filt_vpp_label = QLabel("V_pp: -- mV")
        filt_stats_row.addWidget(filt_tag)
        filt_stats_row.addWidget(self.filt_vmin_label)
        filt_stats_row.addSpacing(24)
        filt_stats_row.addWidget(self.filt_vmax_label)
        filt_stats_row.addSpacing(24)
        filt_stats_row.addWidget(self.filt_vmean_label)
        filt_stats_row.addSpacing(24)
        filt_stats_row.addWidget(self.filt_vpp_label)
        filt_stats_row.addStretch(1)

        info_layout.addLayout(status_row)
        info_layout.addLayout(raw_stats_row)
        info_layout.addLayout(filt_stats_row)
        root_layout.addWidget(info_frame)

        # Row 3: Main ECG Plot Area
        self.plot_widget = EcgPlotWidget(self)
        root_layout.addWidget(self.plot_widget, 1)

    def _connect_signals(self):
        self.serial.readyRead.connect(self.on_ready_read)
        self.serial.errorOccurred.connect(self.on_serial_error)

        self.refresh_button.clicked.connect(self.refresh_ports)
        self.connect_button.clicked.connect(self.toggle_connection)
        self.filter_combo.currentIndexChanged.connect(self.on_filter_config_changed)
        self.stages_combo.currentIndexChanged.connect(self.on_filter_config_changed)
        self.show_raw_check.toggled.connect(self.on_visibility_changed)
        self.show_filt_check.toggled.connect(self.on_visibility_changed)
        self.open_log_button.clicked.connect(self.open_log_file)
        self.zoom_in_button.clicked.connect(self.plot_widget.zoom_in)
        self.zoom_out_button.clicked.connect(self.plot_widget.zoom_out)
        self.zoom_y_in_button.clicked.connect(self.plot_widget.zoom_y_in)
        self.zoom_y_out_button.clicked.connect(self.plot_widget.zoom_y_out)
        self.reset_zoom_button.clicked.connect(self.plot_widget.reset_zoom)
        self.pause_button.clicked.connect(self.toggle_pause)
        self.clear_button.clicked.connect(self.clear_data)

    def refresh_ports(self):
        selected_name = self.port_combo.currentData()
        self.port_combo.clear()

        for info in QSerialPortInfo.availablePorts():
            label = info.portName()
            desc = info.description().strip()
            if desc:
                label += f" — {desc}"
            self.port_combo.addItem(label, info.portName())

        if selected_name:
            for idx in range(self.port_combo.count()):
                if self.port_combo.itemData(idx) == selected_name:
                    self.port_combo.setCurrentIndex(idx)
                    break

    def _configure_serial(self, port_name, baud_rate):
        self.serial.setPortName(port_name)
        self.serial.setBaudRate(baud_rate)
        self.serial.setDataBits(QSerialPort.Data8)
        self.serial.setParity(QSerialPort.NoParity)
        self.serial.setStopBits(QSerialPort.OneStop)
        self.serial.setFlowControl(QSerialPort.NoFlowControl)

    def toggle_connection(self):
        if self.serial.isOpen() or self.auto_reconnect_enabled:
            self.disconnect_serial(manual=True)
        else:
            self.connect_serial()

    def _open_serial_port(self):
        if self.serial.open(QIODevice.ReadWrite) or self.serial.open(QIODevice.ReadOnly):
            self.serial.setDataTerminalReady(True)
            self.serial.setRequestToSend(True)
            return True
        return False

    def connect_serial(self):
        port_name = self.port_combo.currentData()
        if not port_name:
            QMessageBox.warning(self, "UART", "No COM port found.")
            return

        baud_rate = int(self.baud_combo.currentData())
        self.saved_port_name = port_name
        self.saved_baud_rate = baud_rate
        self.auto_reconnect_enabled = True
        self.rx_line_buffer = ""

        self._configure_serial(port_name, baud_rate)
        if self._open_serial_port():
            self.reconnect_timer.stop()
            self._set_connected_ui(True)
            return

        self._set_reconnecting_ui()
        self.reconnect_timer.start()

    def disconnect_serial(self, manual=False):
        if manual:
            self.auto_reconnect_enabled = False
            self.reconnect_timer.stop()

        if self.serial.isOpen():
            self.serial.close()

        if manual or not self.auto_reconnect_enabled:
            self._set_connected_ui(False)
        else:
            self._set_reconnecting_ui()
            if not self.reconnect_timer.isActive():
                self.reconnect_timer.start()

    def try_reconnect(self):
        if not self.auto_reconnect_enabled or self.serial.isOpen():
            self.reconnect_timer.stop()
            return
        if not self.saved_port_name or self.saved_baud_rate is None:
            return

        self._configure_serial(self.saved_port_name, self.saved_baud_rate)
        if self._open_serial_port():
            self.reconnect_timer.stop()
            self._set_connected_ui(True)
        else:
            self._set_reconnecting_ui()

    def _set_reconnecting_ui(self):
        self.port_combo.setEnabled(False)
        self.baud_combo.setEnabled(False)
        self.refresh_button.setEnabled(False)
        self.connect_button.setText("Disconnect")
        port_name = self.saved_port_name or ""
        self.conn_status_label.setText(f"↻ RECONNECTING ({port_name})")
        self.conn_status_label.setStyleSheet("color: #f59e0b; font-weight: 600;")

    def _set_connected_ui(self, connected):
        self.port_combo.setEnabled(not connected)
        self.baud_combo.setEnabled(not connected)
        self.refresh_button.setEnabled(not connected)

        if connected:
            port_name = self.serial.portName()
            baud = self.serial.baudRate()
            self.connect_button.setText("Disconnect")
            self.conn_status_label.setText(f"● CONNECTED ({port_name} @ {baud})")
            self.conn_status_label.setStyleSheet("color: #16a34a; font-weight: 600;")
        else:
            self.connect_button.setText("Connect")
            self.conn_status_label.setText("○ DISCONNECTED")
            self.conn_status_label.setStyleSheet("color: #6b7280; font-weight: 600;")

    def on_serial_error(self, error):
        if error == QSerialPort.NoError:
            return
        if error == QSerialPort.ResourceError:
            self.disconnect_serial(manual=False)

    @staticmethod
    def _format_bytes(value):
        if value < 1024:
            return f"{value} B"
        if value < 1024 * 1024:
            return f"{value / 1024:.1f} KB"
        return f"{value / (1024 * 1024):.2f} MB"

    def on_ready_read(self):
        data = bytes(self.serial.readAll())
        if not data:
            return

        self.rx_bytes += len(data)
        self.rx_count_label.setText(f"RX: {self._format_bytes(self.rx_bytes)}")

        text = data.decode("utf-8", errors="replace")
        self.rx_line_buffer += text

        while "\n" in self.rx_line_buffer:
            line, self.rx_line_buffer = self.rx_line_buffer.split("\n", 1)
            self._process_line(line)

    def on_visibility_changed(self):
        self.plot_widget.show_raw = self.show_raw_check.isChecked()
        self.plot_widget.show_filt = self.show_filt_check.isChecked()
        self.plot_widget.update()

    def on_filter_config_changed(self):
        ftype = self.filter_combo.currentData() or "none"
        stages = self.stages_combo.currentData() or 1
        self.stages_combo.setEnabled(ftype != "none")

        # Configure identical software filter chains for both RAW and FILT signals
        self.raw_filter_chain.configure(ftype, stages)
        self.filt_filter_chain.configure(ftype, stages)

        self.plot_widget.raw_samples_mv.clear()
        self.plot_widget.filt_samples_mv.clear()

        if self.raw_samples_history:
            raw_filtered_all = self.raw_filter_chain.process_block(self.raw_samples_history)
            self.plot_widget.append_raw_samples(raw_filtered_all)
            vis_n = min(len(raw_filtered_all), 50)
            self._update_raw_stats_from_mv(raw_filtered_all[-vis_n:])

        if self.filt_samples_history:
            filt_filtered_all = self.filt_filter_chain.process_block(self.filt_samples_history)
            self.plot_widget.append_filt_samples(filt_filtered_all)
            vis_n = min(len(filt_filtered_all), 50)
            self._update_filt_stats_from_mv(filt_filtered_all[-vis_n:])

        self.plot_widget.update()

    def _update_raw_stats_from_mv(self, mv_values):
        vmin = min(mv_values)
        vmax = max(mv_values)
        vmean = sum(mv_values) / len(mv_values)
        vpp = vmax - vmin

        self.raw_vmin_label.setText(f"V_min: {vmin:+.3f} mV")
        self.raw_vmax_label.setText(f"V_max: {vmax:+.3f} mV")
        self.raw_vmean_label.setText(f"V_mean: {vmean:+.3f} mV")
        self.raw_vpp_label.setText(f"V_pp: {vpp:.3f} mV")

    def _update_filt_stats_from_mv(self, mv_values):
        vmin = min(mv_values)
        vmax = max(mv_values)
        vmean = sum(mv_values) / len(mv_values)
        vpp = vmax - vmin

        self.filt_vmin_label.setText(f"V_min: {vmin:+.3f} mV")
        self.filt_vmax_label.setText(f"V_max: {vmax:+.3f} mV")
        self.filt_vmean_label.setText(f"V_mean: {vmean:+.3f} mV")
        self.filt_vpp_label.setText(f"V_pp: {vpp:.3f} mV")

    def _process_line(self, raw_line):
        if self.paused:
            return

        # Remove ANSI color codes (\x1b[1;31m, \x1b[0m) and "--- 19 messages dropped ---"
        cleaned = ANSI_ESCAPE_RE.sub(" ", raw_line)
        cleaned = DROPPED_MSG_RE.sub(" ", cleaned).strip()
        if not cleaned:
            return

        if "[FAIL]" in cleaned:
            self.lead_status_label.setText(cleaned)
            self.lead_status_label.setStyleSheet("color: #dc2626; font-weight: 600;")
            return

        header_match = BUF_HEADER_RE.search(cleaned)
        if header_match:
            buf_id = int(header_match.group(1))
            elapsed_ms = int(header_match.group(2))
            lo_plus = int(header_match.group(3))
            lo_minus = int(header_match.group(4))
            status_str = header_match.group(5).strip()

            self.packet_counter = buf_id
            self.buffer_info_label.setText(f"Buffer: #{buf_id:04d} ({elapsed_ms} ms)")
            self.lead_status_label.setText(
                f"Leads: {status_str} (LO+={lo_plus}, LO-={lo_minus})"
            )
            if lo_plus != 0 or lo_minus != 0:
                self.lead_status_label.setStyleSheet("color: #dc2626; font-weight: 600;")
            else:
                self.lead_status_label.setStyleSheet("color: #16a34a; font-weight: 600;")

            if header_match.group(7) is not None and self.raw_filter_chain.filter_type == "none":
                r_min = raw_lsb_to_ecg_mv(int(header_match.group(7)))
                r_max = raw_lsb_to_ecg_mv(int(header_match.group(8)))
                r_mean = raw_lsb_to_ecg_mv(int(header_match.group(9)))
                self.raw_vmin_label.setText(f"V_min: {r_min:+.3f} mV")
                self.raw_vmax_label.setText(f"V_max: {r_max:+.3f} mV")
                self.raw_vmean_label.setText(f"V_mean: {r_mean:+.3f} mV")
                self.raw_vpp_label.setText(f"V_pp: {r_max - r_min:.3f} mV")

                f_min = raw_lsb_to_ecg_mv(int(header_match.group(10)))
                f_max = raw_lsb_to_ecg_mv(int(header_match.group(11)))
                f_mean = raw_lsb_to_ecg_mv(int(header_match.group(12)))
                self.filt_vmin_label.setText(f"V_min: {f_min:+.3f} mV")
                self.filt_vmax_label.setText(f"V_max: {f_max:+.3f} mV")
                self.filt_vmean_label.setText(f"V_mean: {f_mean:+.3f} mV")
                self.filt_vpp_label.setText(f"V_pp: {f_max - f_min:.3f} mV")
            return

        # Skip startup banner lines ([PASS], ===, ---)
        if cleaned.startswith(("[PASS]", "[INFO]", "===", "---")):
            return

        upper = cleaned.upper()
        is_filt_line = "FILT:" in upper
        is_raw_line = ("RAW:" in upper) and not is_filt_line

        if is_filt_line:
            payload = cleaned[upper.index("FILT:") + 5 :]
        elif is_raw_line:
            payload = cleaned[upper.index("RAW:") + 4 :]
        else:
            payload = cleaned
        tokens = INT_TOKEN_RE.findall(payload)
        if len(tokens) >= 3:
            valid_mv = []
            for tok in tokens:
                val = int(tok)
                if 0 <= val <= 4095:
                    valid_mv.append(raw_lsb_to_ecg_mv(val))

            if valid_mv:
                if is_filt_line:
                    self.filt_samples_history.extend(valid_mv)
                    filtered_samples = self.filt_filter_chain.process_block(valid_mv)
                    self._update_filt_stats_from_mv(filtered_samples)
                    self.plot_widget.append_filt_samples(filtered_samples)
                else:
                    self.raw_samples_history.extend(valid_mv)
                    filtered_samples = self.raw_filter_chain.process_block(valid_mv)
                    if not is_raw_line:
                        self.packet_counter += 1
                    if self.buffer_info_label.text() == "Buffer: --":
                        self.buffer_info_label.setText(f"Buffer: #{self.packet_counter:04d}")
                    self._update_raw_stats_from_mv(filtered_samples)
                    self.plot_widget.append_raw_samples(filtered_samples)

    def toggle_pause(self):
        self.paused = not self.paused
        self.pause_button.setText("Resume" if self.paused else "Pause")

    def clear_data(self):
        self.raw_samples_history.clear()
        self.filt_samples_history.clear()
        self.raw_filter_chain.reset()
        self.filt_filter_chain.reset()
        self.plot_widget.clear_samples()
        self.rx_bytes = 0
        self.packet_counter = 0
        self.rx_count_label.setText("RX: 0 B")
        self.buffer_info_label.setText("Buffer: --")
        self.lead_status_label.setText("Leads: --")
        self.lead_status_label.setStyleSheet("font-weight: 600;")
        self.raw_vmin_label.setText("V_min: -- mV")
        self.raw_vmax_label.setText("V_max: -- mV")
        self.raw_vmean_label.setText("V_mean: -- mV")
        self.raw_vpp_label.setText("V_pp: -- mV")
        self.filt_vmin_label.setText("V_min: -- mV")
        self.filt_vmax_label.setText("V_max: -- mV")
        self.filt_vmean_label.setText("V_mean: -- mV")
        self.filt_vpp_label.setText("V_pp: -- mV")

    def open_log_file(self):
        file_name, _ = QFileDialog.getOpenFileName(
            self,
            "Open UART Log File",
            str(Path.home()),
            "Log files (*.log *.txt);;All files (*.*)",
        )
        if not file_name:
            return

        try:
            content = Path(file_name).read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            QMessageBox.critical(self, "Open Log", f"Failed to read file:\n\n{exc}")
            return

        was_paused = self.paused
        self.paused = False
        self.clear_data()

        for line in content.splitlines():
            self._process_line(line)

        self.paused = was_paused

    def closeEvent(self, event):
        self.auto_reconnect_enabled = False
        self.reconnect_timer.stop()
        if self.serial.isOpen():
            self.serial.close()
        event.accept()


def main():
    app = QApplication(sys.argv)
    app.setApplicationName("AD8232 ECG Signal Visualizer")

    window = EcgVisualizerWindow()
    window.show()

    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
