import sys
import asyncio
import csv
import math
import struct
from collections import deque

from bleak import BleakScanner, BleakClient
from PyQt5 import QtWidgets, QtCore
import pyqtgraph as pg


DEVICE_NAME = "ESP32S3_ADS1292_EMG"

CHAR_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
CTRL_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

BUFFER_SIZE = 3000

Y_MIN = -5.0
Y_MAX = 5.0

RECT_Y_MIN = 0.0
RECT_Y_MAX = 5.0

RMS_Y_MIN = 0.0
RMS_Y_MAX = 5.0

SAMPLE_RATE = 1000.0
HPF_CUTOFF = 20.0
NOTCH_FREQ = 50.0
NOTCH_Q = 30.0
RMS_WINDOW = 100

LIGHT_BLUE = (0, 180, 255)

pg.setConfigOption("background", "w")
pg.setConfigOption("foreground", "k")
pg.setConfigOptions(antialias=True)


class HighPassFilter:
    def __init__(self, fs, cutoff):
        dt = 1.0 / fs
        rc = 1.0 / (2.0 * math.pi * cutoff)
        self.alpha = rc / (rc + dt)
        self.prev_x = 0.0
        self.prev_y = 0.0

    def process(self, x):
        y = self.alpha * (self.prev_y + x - self.prev_x)
        self.prev_x = x
        self.prev_y = y
        return y


class NotchFilter:
    def __init__(self, fs, f0, q):
        w0 = 2.0 * math.pi * f0 / fs
        alpha = math.sin(w0) / (2.0 * q)

        b0 = 1.0
        b1 = -2.0 * math.cos(w0)
        b2 = 1.0

        a0 = 1.0 + alpha
        a1 = -2.0 * math.cos(w0)
        a2 = 1.0 - alpha

        self.b0 = b0 / a0
        self.b1 = b1 / a0
        self.b2 = b2 / a0
        self.a1 = a1 / a0
        self.a2 = a2 / a0

        self.x1 = 0.0
        self.x2 = 0.0
        self.y1 = 0.0
        self.y2 = 0.0

    def process(self, x):
        y = (
            self.b0 * x +
            self.b1 * self.x1 +
            self.b2 * self.x2 -
            self.a1 * self.y1 -
            self.a2 * self.y2
        )

        self.x2 = self.x1
        self.x1 = x
        self.y2 = self.y1
        self.y1 = y

        return y


class BLEWorker(QtCore.QThread):
    data_received = QtCore.pyqtSignal(float)
    status = QtCore.pyqtSignal(str)
    connected_changed = QtCore.pyqtSignal(bool)

    def __init__(self):
        super().__init__()
        self.client = None
        self.loop = None

    def run(self):
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        self.loop.run_until_complete(self.ble_task())

    def send_command(self, command):
        if self.loop is None:
            return

        asyncio.run_coroutine_threadsafe(
            self._send_command_async(command),
            self.loop
        )

    async def _send_command_async(self, command):
        if self.client is None:
            return

        try:
            await self.client.write_gatt_char(
                CTRL_UUID,
                command.encode("utf-8"),
                response=False
            )
        except Exception:
            pass

    async def ble_task(self):
        self.status.emit("Ieškomas BLE...")

        device = None
        devices = await BleakScanner.discover(timeout=10)

        for d in devices:
            if d.name and DEVICE_NAME in d.name:
                device = d
                break

        if device is None:
            self.status.emit("BLE nerastas")
            self.connected_changed.emit(False)
            return

        try:
            async with BleakClient(device.address) as client:
                self.client = client
                self.connected_changed.emit(True)
                self.status.emit("Prisijungta prie EMG_ads1292")

                try:
                    await client.request_mtu(517)
                except Exception:
                    pass

                def notification_handler(sender, data):
                    try:
                        if len(data) % 4 != 0:
                            return

                        for i in range(0, len(data), 4):
                            ch1_uv, ch2_uv = struct.unpack_from("<hh", data, i)
                            ch1_mv = ch1_uv / 1000.0
                            self.data_received.emit(ch1_mv)

                    except Exception:
                        pass

                await client.start_notify(CHAR_UUID, notification_handler)

                while True:
                    await asyncio.sleep(0.1)

        except Exception:
            self.status.emit("BLE ryšys nutruko")
            self.connected_changed.emit(False)
            self.client = None


class EMGWindow(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()

        self.setWindowTitle("EMG - 1 kanalas")
        self.resize(1200, 850)

        self.measurement_running = False
        self.csv_recording = False
        self.sample_counter = 0

        self.ch1_data = deque([0.0] * BUFFER_SIZE, maxlen=BUFFER_SIZE)
        self.ch1_rect_data = deque([0.0] * BUFFER_SIZE, maxlen=BUFFER_SIZE)
        self.ch1_rms_data = deque([0.0] * BUFFER_SIZE, maxlen=BUFFER_SIZE)

        self.ch1_rms_window = deque(maxlen=RMS_WINDOW)

        self.recorded_data = []

        self.ch1_hpf = HighPassFilter(SAMPLE_RATE, HPF_CUTOFF)
        self.ch1_notch = NotchFilter(SAMPLE_RATE, NOTCH_FREQ, NOTCH_Q)

        central = QtWidgets.QWidget()
        self.setCentralWidget(central)

        main_layout = QtWidgets.QVBoxLayout()
        central.setLayout(main_layout)

        self.plot_ch1 = pg.PlotWidget()
        self.plot_ch1.setBackground("w")
        self.plot_ch1.showGrid(x=True, y=True, alpha=0.25)
        self.plot_ch1.setYRange(Y_MIN, Y_MAX)
        self.plot_ch1.setTitle("Neapdorotas signalas")
        self.plot_ch1.setLabel("left", "Amplitudė", units="mV")
        self.plot_ch1.setLabel("bottom", "Atskaitos")
        self.curve_ch1 = self.plot_ch1.plot(
            pen=pg.mkPen(color=LIGHT_BLUE, width=2)
        )
        main_layout.addWidget(self.plot_ch1)

        self.plot_rect = pg.PlotWidget()
        self.plot_rect.setBackground("w")
        self.plot_rect.showGrid(x=True, y=True, alpha=0.25)
        self.plot_rect.setYRange(RECT_Y_MIN, RECT_Y_MAX)
        self.plot_rect.setTitle("Signalo išlyginimas")
        self.plot_rect.setLabel("left", "Amplitudė", units="mV")
        self.plot_rect.setLabel("bottom", "Atskaitos")
        self.curve_rect = self.plot_rect.plot(
            pen=pg.mkPen(color=LIGHT_BLUE, width=2)
        )
        main_layout.addWidget(self.plot_rect)

        self.plot_rms = pg.PlotWidget()
        self.plot_rms.setBackground("w")
        self.plot_rms.showGrid(x=True, y=True, alpha=0.25)
        self.plot_rms.setYRange(RMS_Y_MIN, RMS_Y_MAX)
        self.plot_rms.setTitle("Signalo RMS vertė")
        self.plot_rms.setLabel("left", "Amplitudė", units="mV")
        self.plot_rms.setLabel("bottom", "Atskaitos")
        self.curve_rms = self.plot_rms.plot(
            pen=pg.mkPen(color=LIGHT_BLUE, width=2)
        )
        main_layout.addWidget(self.plot_rms)

        buttons_layout = QtWidgets.QHBoxLayout()

        self.startMeasureButton = QtWidgets.QPushButton("Pradeti matavimą")
        self.stopMeasureButton = QtWidgets.QPushButton("Sustabdyti matavimą")
        self.startCsvButton = QtWidgets.QPushButton("Pradeti irašymą")
        self.stopCsvButton = QtWidgets.QPushButton("Sustabdyti irašymą")
        self.saveButton = QtWidgets.QPushButton("Išsaugoti duomenis")
        self.clearButton = QtWidgets.QPushButton("Ištrinti grafikus")

        buttons_layout.addWidget(self.startMeasureButton)
        buttons_layout.addWidget(self.stopMeasureButton)
        buttons_layout.addWidget(self.startCsvButton)
        buttons_layout.addWidget(self.stopCsvButton)
        buttons_layout.addWidget(self.saveButton)
        buttons_layout.addWidget(self.clearButton)

        main_layout.addLayout(buttons_layout)

        self.statusLabel = QtWidgets.QLabel("Ieškomas BLE...")
        self.statusLabel.setStyleSheet("font-size: 12pt; padding: 5px;")
        main_layout.addWidget(self.statusLabel)

        self.startMeasureButton.clicked.connect(self.start_measurement)
        self.stopMeasureButton.clicked.connect(self.stop_measurement)
        self.startCsvButton.clicked.connect(self.start_csv_recording)
        self.stopCsvButton.clicked.connect(self.stop_csv_recording)
        self.saveButton.clicked.connect(self.save_csv)
        self.clearButton.clicked.connect(self.clear_all)

        self.worker = BLEWorker()
        self.worker.data_received.connect(self.add_data)
        self.worker.status.connect(self.update_status)
        self.worker.connected_changed.connect(self.on_connected_changed)
        self.worker.start()

        self.timer = QtCore.QTimer()
        self.timer.timeout.connect(self.update_plot)
        self.timer.start(60)

        self.startMeasureButton.setEnabled(False)
        self.stopMeasureButton.setEnabled(False)
        self.startCsvButton.setEnabled(False)
        self.stopCsvButton.setEnabled(False)

    def on_connected_changed(self, connected):
        self.startMeasureButton.setEnabled(connected)
        self.stopMeasureButton.setEnabled(False)

    def start_measurement(self):
        self.measurement_running = True
        self.worker.send_command("START")

        self.startMeasureButton.setEnabled(False)
        self.stopMeasureButton.setEnabled(True)
        self.startCsvButton.setEnabled(True)

    def stop_measurement(self):
        self.measurement_running = False
        self.csv_recording = False
        self.worker.send_command("STOP")

        self.startMeasureButton.setEnabled(True)
        self.stopMeasureButton.setEnabled(False)
        self.startCsvButton.setEnabled(False)
        self.stopCsvButton.setEnabled(False)

    def start_csv_recording(self):
        if not self.measurement_running:
            return

        self.csv_recording = True
        self.startCsvButton.setEnabled(False)
        self.stopCsvButton.setEnabled(True)

    def stop_csv_recording(self):
        self.csv_recording = False
        self.startCsvButton.setEnabled(True)
        self.stopCsvButton.setEnabled(False)

    def add_data(self, ch1_raw):
        ch1_hpf = self.ch1_hpf.process(ch1_raw)
        ch1_filtered = self.ch1_notch.process(ch1_hpf)

        ch1_rectified = abs(ch1_filtered)

        self.ch1_rms_window.append(ch1_filtered * ch1_filtered)

        if len(self.ch1_rms_window) > 0:
            ch1_rms = math.sqrt(
                sum(self.ch1_rms_window) / len(self.ch1_rms_window)
            )
        else:
            ch1_rms = 0.0

        self.ch1_data.append(ch1_filtered)
        self.ch1_rect_data.append(ch1_rectified)
        self.ch1_rms_data.append(ch1_rms)

        if self.csv_recording:
            self.recorded_data.append([
                self.sample_counter,
                ch1_filtered,
                ch1_rectified,
                ch1_rms,
                ch1_raw
            ])

        self.sample_counter += 1

    def update_plot(self):
        self.curve_ch1.setData(list(self.ch1_data))
        self.curve_rect.setData(list(self.ch1_rect_data))
        self.curve_rms.setData(list(self.ch1_rms_data))

    def update_status(self, text):
        self.statusLabel.setText(text)

    def clear_all(self):
        self.ch1_data.clear()
        self.ch1_rect_data.clear()
        self.ch1_rms_data.clear()
        self.ch1_rms_window.clear()

        self.ch1_data.extend([0.0] * BUFFER_SIZE)
        self.ch1_rect_data.extend([0.0] * BUFFER_SIZE)
        self.ch1_rms_data.extend([0.0] * BUFFER_SIZE)

        self.recorded_data.clear()
        self.sample_counter = 0

    def save_csv(self):
        filename, _ = QtWidgets.QFileDialog.getSaveFileName(
            self,
            "Save EMG Data",
            "ads1292_emg_1ch_rectified_rms.csv",
            "CSV Files (*.csv)"
        )

        if filename:
            with open(filename, "w", newline="") as file:
                writer = csv.writer(file)
                writer.writerow([
                    "sample",
                    "ch1_filtered_mV",
                    "ch1_rectified_mV",
                    "ch1_rms_mV",
                    "ch1_raw_mV"
                ])

                for row in self.recorded_data:
                    writer.writerow(row)

    def closeEvent(self, event):
        try:
            self.worker.send_command("STOP")
        except Exception:
            pass

        event.accept()


if __name__ == "__main__":
    app = QtWidgets.QApplication(sys.argv)

    window = EMGWindow()
    window.show()

    sys.exit(app.exec_())