import sys
from pathlib import Path

from PyQt5.QtCore import QIODevice, Qt, QTimer
from PyQt5.QtGui import QFontDatabase, QTextCursor
from PyQt5.QtSerialPort import QSerialPort, QSerialPortInfo
from PyQt5.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QVBoxLayout,
    QWidget,
)


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()

        self.serial = QSerialPort(self)
        self.rx_bytes = 0

        # Remember the selected UART configuration for automatic reconnect.
        self.saved_port_name = None
        self.saved_baud_rate = None
        self.auto_reconnect_enabled = False

        self.reconnect_timer = QTimer(self)
        self.reconnect_timer.setInterval(1000)
        self.reconnect_timer.timeout.connect(self.try_reconnect)

        self.setWindowTitle("UART DEBUG")
        self.resize(1000, 650)
        self.setMinimumSize(720, 420)

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

        # Top connection bar
        top_bar = QHBoxLayout()
        top_bar.setSpacing(8)

        port_label = QLabel("Port")
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(120)

        baud_label = QLabel("Baud")
        self.baud_combo = QComboBox()
        self.baud_combo.setMinimumWidth(120)

        for baud in (
            1200, 2400, 4800, 9600, 19200, 38400,
            57600, 115200, 230400, 460800, 921600
        ):
            self.baud_combo.addItem(str(baud), baud)

        default_index = self.baud_combo.findData(115200)
        if default_index >= 0:
            self.baud_combo.setCurrentIndex(default_index)

        self.refresh_button = QPushButton("Refresh")
        self.connect_button = QPushButton("Connect")

        top_bar.addWidget(port_label)
        top_bar.addWidget(self.port_combo)
        top_bar.addSpacing(8)
        top_bar.addWidget(baud_label)
        top_bar.addWidget(self.baud_combo)
        top_bar.addWidget(self.refresh_button)
        top_bar.addWidget(self.connect_button)
        top_bar.addStretch(1)

        root_layout.addLayout(top_bar)

        # Receive header
        rx_header = QHBoxLayout()
        self.rx_title = QLabel("UART RX")
        self.rx_title.setStyleSheet("font-weight: 600;")
        self.rx_count_label = QLabel("RX: 0 B")

        rx_header.addWidget(self.rx_title)
        rx_header.addStretch(1)
        rx_header.addWidget(self.rx_count_label)

        root_layout.addLayout(rx_header)

        # Main UART log view
        self.log_view = QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setLineWrapMode(QPlainTextEdit.WidgetWidth)
        self.log_view.document().setMaximumBlockCount(50000)

        fixed_font = QFontDatabase.systemFont(QFontDatabase.FixedFont)
        self.log_view.setFont(fixed_font)

        root_layout.addWidget(self.log_view, 1)

        # Bottom bar
        bottom_bar = QHBoxLayout()
        bottom_bar.setSpacing(8)

        self.auto_scroll_checkbox = QCheckBox("Auto scroll")
        self.auto_scroll_checkbox.setChecked(True)

        self.clear_button = QPushButton("Clear")
        self.save_button = QPushButton("Save Log")

        self.status_label = QLabel("DISCONNECTED")
        self.status_label.setAlignment(Qt.AlignRight | Qt.AlignVCenter)

        bottom_bar.addWidget(self.auto_scroll_checkbox)
        bottom_bar.addWidget(self.clear_button)
        bottom_bar.addWidget(self.save_button)
        bottom_bar.addStretch(1)
        bottom_bar.addWidget(self.status_label)

        root_layout.addLayout(bottom_bar)

        # Keyboard order
        self.setTabOrder(self.port_combo, self.baud_combo)
        self.setTabOrder(self.baud_combo, self.refresh_button)
        self.setTabOrder(self.refresh_button, self.connect_button)
        self.setTabOrder(self.connect_button, self.log_view)
        self.setTabOrder(self.log_view, self.auto_scroll_checkbox)
        self.setTabOrder(self.auto_scroll_checkbox, self.clear_button)
        self.setTabOrder(self.clear_button, self.save_button)

    def _connect_signals(self):
        self.serial.readyRead.connect(self.on_ready_read)
        self.serial.errorOccurred.connect(self.on_serial_error)

        self.refresh_button.clicked.connect(self.refresh_ports)
        self.connect_button.clicked.connect(self.toggle_connection)
        self.clear_button.clicked.connect(self.clear_log)
        self.save_button.clicked.connect(self.save_log)

    def refresh_ports(self):
        selected_name = self.port_combo.currentData()

        self.port_combo.clear()

        ports = QSerialPortInfo.availablePorts()
        for info in ports:
            label = info.portName()

            description = info.description().strip()
            if description:
                label += f" — {description}"

            self.port_combo.addItem(label, info.portName())

        if selected_name:
            for index in range(self.port_combo.count()):
                if self.port_combo.itemData(index) == selected_name:
                    self.port_combo.setCurrentIndex(index)
                    break

    def toggle_connection(self):
        if self.serial.isOpen() or self.auto_reconnect_enabled:
            self.disconnect_serial(manual=True)
        else:
            self.connect_serial()

    def _configure_serial(self, port_name, baud_rate):
        self.serial.setPortName(port_name)
        self.serial.setBaudRate(baud_rate)
        self.serial.setDataBits(QSerialPort.Data8)
        self.serial.setParity(QSerialPort.NoParity)
        self.serial.setStopBits(QSerialPort.OneStop)
        self.serial.setFlowControl(QSerialPort.NoFlowControl)

    def connect_serial(self):
        port_name = self.port_combo.currentData()
        if not port_name:
            QMessageBox.warning(self, "UART", "Không tìm thấy COM port.")
            return

        baud_rate = int(self.baud_combo.currentData())

        # Store the port once. Future reconnects reuse these values.
        self.saved_port_name = port_name
        self.saved_baud_rate = baud_rate
        self.auto_reconnect_enabled = True

        self._configure_serial(port_name, baud_rate)

        if self.serial.open(QIODevice.ReadOnly):
            self.reconnect_timer.stop()
            self._set_connected_ui(True)
            return

        # Keep retrying silently if the port is temporarily unavailable.
        self._set_reconnecting_ui()
        self.reconnect_timer.start()

    def disconnect_serial(self, manual=False):
        if manual:
            self.auto_reconnect_enabled = False
            self.reconnect_timer.stop()

        if self.serial.isOpen():
            self.serial.close()

        if manual:
            self._set_connected_ui(False)
        elif self.auto_reconnect_enabled:
            self._set_reconnecting_ui()
            if not self.reconnect_timer.isActive():
                self.reconnect_timer.start()
        else:
            self._set_connected_ui(False)

    def try_reconnect(self):
        if not self.auto_reconnect_enabled:
            self.reconnect_timer.stop()
            return

        if self.serial.isOpen():
            self.reconnect_timer.stop()
            return

        if not self.saved_port_name or self.saved_baud_rate is None:
            return

        self._configure_serial(self.saved_port_name, self.saved_baud_rate)

        if self.serial.open(QIODevice.ReadOnly):
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
        self.status_label.setText(f"↻ RECONNECTING   {port_name}")

    def _set_connected_ui(self, connected):
        self.port_combo.setEnabled(not connected)
        self.baud_combo.setEnabled(not connected)
        self.refresh_button.setEnabled(not connected)

        if connected:
            port_name = self.serial.portName()
            baud = self.serial.baudRate()

            self.connect_button.setText("Disconnect")
            self.status_label.setText(f"● CONNECTED   {port_name} @ {baud} 8N1")
        else:
            self.connect_button.setText("Connect")
            self.status_label.setText("○ DISCONNECTED")

    def on_ready_read(self):
        data = bytes(self.serial.readAll())
        if not data:
            return

        self.rx_bytes += len(data)
        self.rx_count_label.setText(f"RX: {self._format_bytes(self.rx_bytes)}")

        # Keep UART content as-is as much as possible.
        # errors="replace" prevents invalid UTF-8 bytes from crashing the UI.
        text = data.decode("utf-8", errors="replace")

        cursor = self.log_view.textCursor()
        cursor.movePosition(QTextCursor.End)
        cursor.insertText(text)

        if self.auto_scroll_checkbox.isChecked():
            self.log_view.setTextCursor(cursor)
            self.log_view.ensureCursorVisible()

    def on_serial_error(self, error):
        if error == QSerialPort.NoError:
            return

        if error == QSerialPort.ResourceError:
            # UART was unplugged/lost. Close it and retry the same saved port.
            self.disconnect_serial(manual=False)

    def clear_log(self):
        self.log_view.clear()
        self.rx_bytes = 0

        # Remember the selected UART configuration for automatic reconnect.
        self.saved_port_name = None
        self.saved_baud_rate = None
        self.auto_reconnect_enabled = False

        self.reconnect_timer = QTimer(self)
        self.reconnect_timer.setInterval(1000)
        self.reconnect_timer.timeout.connect(self.try_reconnect)
        self.rx_count_label.setText("RX: 0 B")

    def save_log(self):
        default_path = str(Path.home() / "uart_debug.log")

        file_name, _ = QFileDialog.getSaveFileName(
            self,
            "Save UART Log",
            default_path,
            "Log files (*.log);;Text files (*.txt);;All files (*.*)",
        )

        if not file_name:
            return

        try:
            Path(file_name).write_text(
                self.log_view.toPlainText(),
                encoding="utf-8",
                newline="",
            )
        except OSError as exc:
            QMessageBox.critical(
                self,
                "Save Log",
                f"Không lưu được file.\n\n{exc}",
            )

    @staticmethod
    def _format_bytes(value):
        if value < 1024:
            return f"{value} B"
        if value < 1024 * 1024:
            return f"{value / 1024:.1f} KB"
        return f"{value / (1024 * 1024):.2f} MB"

    def closeEvent(self, event):
        self.auto_reconnect_enabled = False
        self.reconnect_timer.stop()

        if self.serial.isOpen():
            self.serial.close()

        event.accept()


def main():
    app = QApplication(sys.argv)
    app.setApplicationName("UART Debug Viewer")

    window = MainWindow()
    window.show()

    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
