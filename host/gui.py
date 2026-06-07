#!/usr/bin/env python3
"""Desktop GUI for the Treecataje companion device (Phase 5).

Thin PySide6 front-end over the SAME shared core as the TUI and MCP server
(companion_proto.Companion for USB, mcp_server.BleSync for BLE). All device I/O
runs on a single dedicated worker thread (DeviceWorker living in its own QThread)
so the Qt event loop never blocks; results come back as signals.

Tabs: Console (run any CLI cmd) · Files (get/put) · Stream (telemetry/wifi/nrf)
· Analyze (host-compute on a fetched capture).

Run:  host/.venv/bin/python host/gui.py [--port /dev/ttyACM1]
"""
import os
import sys
import argparse

from PySide6.QtCore import Qt, QObject, QThread, Signal, Slot
from PySide6.QtGui import QFont, QTextCursor
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QLabel, QLineEdit, QPushButton,
    QComboBox, QTextEdit, QPlainTextEdit, QTabWidget, QFileDialog, QSpinBox,
    QHBoxLayout, QVBoxLayout, QFormLayout, QGroupBox, QSplitter,
)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from companion_proto import Companion  # noqa: E402

DL_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "downloads")


class DeviceWorker(QObject):
    """Owns the device connection and performs all blocking I/O on its thread.

    UI calls these @Slot methods across the thread boundary (queued connection);
    every result is reported back through a signal so the GUI thread stays free.
    """
    connected = Signal(dict)
    disconnected = Signal()
    log = Signal(str)            # freeform log line (may contain simple html)
    status = Signal(str)         # status panel body
    response = Signal(str, list, int, str)  # cmd, lines, code, error
    report = Signal(str)         # analyze report text
    stream_done = Signal(list, list)        # start_lines, events
    error = Signal(str)

    def __init__(self):
        super().__init__()
        self.dev = None
        self.transport = "usb"

    @Slot(str, str, str, str)
    def do_connect(self, transport, port, name, token):
        try:
            if self.dev is not None:
                try:
                    self.dev.close()
                except Exception:
                    pass
                self.dev = None
            self.transport = transport
            if transport == "ble":
                from mcp_server import BleSync
                self.log.emit("scanning for BLE device '%s'… (needs 'companion ble on')" % name)
                self.dev = BleSync(name=name, token=token)
                info = self.dev.info
            else:
                self.dev = Companion(port)
                info = self.dev.hello(token)
            if not info.get("ok"):
                raw = info.get("raw")
                self.error.emit("HELLO failed: %s" % (raw.error if raw else info))
                return
            # strip the unpicklable raw Response before crossing the signal
            self.connected.emit({k: v for k, v in info.items() if k != "raw"})
        except Exception as e:  # noqa: BLE001
            self.error.emit("connect failed: %s" % e)

    @Slot()
    def do_disconnect(self):
        if self.dev is not None:
            try:
                self.dev.close()
            except Exception:
                pass
            self.dev = None
        self.disconnected.emit()

    @Slot(str, float)
    def do_request(self, cmd, timeout):
        if self.dev is None:
            self.error.emit("not connected")
            return
        try:
            r = self.dev.request(cmd, timeout=timeout)
            self.response.emit(cmd, list(r.lines), r.code, r.error or "")
        except Exception as e:  # noqa: BLE001
            self.error.emit("error: %s" % e)

    @Slot()
    def do_status(self):
        if self.dev is None:
            return
        try:
            r = self.dev.request("status", timeout=8.0)
            self.status.emit("\n".join(r.lines) if r.lines else "(no status)")
        except Exception as e:  # noqa: BLE001
            self.error.emit("status error: %s" % e)

    @Slot(str, str)
    def do_file_get(self, remote, local):
        if self.dev is None:
            self.error.emit("not connected")
            return
        try:
            chunk = 192 if self.transport == "ble" else 512
            self.log.emit("get %s …" % remote)
            out = self.dev.file_get(remote, local or None, chunk=chunk, timeout=180)
            saved = (" -> %s" % out["path"]) if "path" in out else ""
            self.log.emit("got %d B  sha256=%s%s" % (out["size"], out["sha256"][:16] + "…", saved))
        except Exception as e:  # noqa: BLE001
            self.error.emit("get error: %s" % e)

    @Slot(str, str)
    def do_file_put(self, local, remote):
        if self.dev is None:
            self.error.emit("not connected")
            return
        try:
            chunk = 192 if self.transport == "ble" else 512
            self.log.emit("put %s -> %s …" % (local, remote))
            out = self.dev.file_put(local, remote, chunk=chunk, timeout=180)
            self.log.emit("put ok=%s sha256=%s" % (out["ok"], out["sha256"][:16] + "…"))
        except Exception as e:  # noqa: BLE001
            self.error.emit("put error: %s" % e)

    @Slot(str)
    def do_analyze(self, remote):
        if self.dev is None:
            self.error.emit("not connected")
            return
        try:
            chunk = 192 if self.transport == "ble" else 512
            self.log.emit("fetch+analyze %s …" % remote)
            out = self.dev.file_get(remote, None, chunk=chunk, timeout=180)
            import companion_compute
            self.report.emit(companion_compute.analyze(remote, out["data"]))
        except Exception as e:  # noqa: BLE001
            self.error.emit("analyze error: %s" % e)

    @Slot(str, float)
    def do_stream(self, kind, duration):
        if self.dev is None:
            self.error.emit("not connected")
            return
        try:
            self.log.emit("stream %s for %.1fs …" % (kind, duration))
            out = self.dev.stream(kind, duration=duration)
            self.stream_done.emit(list(out.get("start", [])), list(out.get("events", [])))
        except Exception as e:  # noqa: BLE001
            self.error.emit("stream error: %s" % e)


class MainWindow(QMainWindow):
    # UI -> worker (queued across the thread boundary)
    sig_connect = Signal(str, str, str, str)
    sig_disconnect = Signal()
    sig_request = Signal(str, float)
    sig_status = Signal()
    sig_file_get = Signal(str, str)
    sig_file_put = Signal(str, str)
    sig_analyze = Signal(str)
    sig_stream = Signal(str, float)

    def __init__(self, port="/dev/ttyACM1"):
        super().__init__()
        self.setWindowTitle("Treecataje — Companion")
        self.resize(1000, 640)
        self._connected = False

        self._build_ui(port)
        self._start_worker()
        self._wire()

    # ---------- UI ----------
    def _build_ui(self, port):
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)

        # connection bar
        bar = QHBoxLayout()
        self.cbo_transport = QComboBox()
        self.cbo_transport.addItems(["usb", "ble"])
        self.ed_port = QLineEdit(port)
        self.ed_port.setMaximumWidth(160)
        self.ed_name = QLineEdit("Bruc")
        self.ed_name.setMaximumWidth(90)
        self.ed_token = QLineEdit()
        self.ed_token.setPlaceholderText("token (empty=open)")
        self.ed_token.setMaximumWidth(160)
        self.btn_connect = QPushButton("Connect")
        self.lbl_conn = QLabel("● disconnected")
        self.lbl_conn.setStyleSheet("color:#c0392b;")
        bar.addWidget(QLabel("transport")); bar.addWidget(self.cbo_transport)
        bar.addWidget(QLabel("port")); bar.addWidget(self.ed_port)
        bar.addWidget(QLabel("name")); bar.addWidget(self.ed_name)
        bar.addWidget(self.ed_token)
        bar.addWidget(self.btn_connect)
        bar.addStretch(1)
        bar.addWidget(self.lbl_conn)
        root.addLayout(bar)

        split = QSplitter(Qt.Horizontal)
        root.addWidget(split, 1)

        # left: device info + status
        left = QWidget()
        lv = QVBoxLayout(left)
        gi = QGroupBox("device")
        giv = QVBoxLayout(gi)
        self.lbl_info = QLabel("—")
        self.lbl_info.setWordWrap(True)
        self.lbl_info.setTextInteractionFlags(Qt.TextSelectableByMouse)
        giv.addWidget(self.lbl_info)
        lv.addWidget(gi)
        gs = QGroupBox("status")
        gsv = QVBoxLayout(gs)
        self.txt_status = QPlainTextEdit(readOnly=True)
        self.txt_status.setFont(_mono())
        gsv.addWidget(self.txt_status)
        self.btn_refresh = QPushButton("Refresh status")
        gsv.addWidget(self.btn_refresh)
        lv.addWidget(gs, 1)
        split.addWidget(left)

        # right: tabs
        self.tabs = QTabWidget()
        self.tabs.addTab(self._tab_console(), "Console")
        self.tabs.addTab(self._tab_files(), "Files")
        self.tabs.addTab(self._tab_stream(), "Stream")
        self.tabs.addTab(self._tab_analyze(), "Analyze")
        split.addWidget(self.tabs)
        split.setSizes([320, 680])

        self.statusBar().showMessage("ready")
        self._set_enabled(False)

    def _tab_console(self):
        w = QWidget(); v = QVBoxLayout(w)
        self.txt_log = QTextEdit(readOnly=True)
        self.txt_log.setFont(_mono())
        v.addWidget(self.txt_log, 1)
        row = QHBoxLayout()
        self.ed_cmd = QLineEdit()
        self.ed_cmd.setPlaceholderText("device CLI command, e.g. free · ls / · wifi scan")
        self.btn_send = QPushButton("Send")
        row.addWidget(self.ed_cmd, 1); row.addWidget(self.btn_send)
        v.addLayout(row)
        return w

    def _tab_files(self):
        w = QWidget(); f = QFormLayout(w)
        self.ed_get_remote = QLineEdit("/bruce.conf")
        rget = QHBoxLayout()
        self.btn_get = QPushButton("Download →")
        rget.addWidget(self.ed_get_remote, 1); rget.addWidget(self.btn_get)
        f.addRow("remote get", _wrap(rget))

        self.ed_put_local = QLineEdit()
        self.btn_browse = QPushButton("Browse…")
        rb = QHBoxLayout(); rb.addWidget(self.ed_put_local, 1); rb.addWidget(self.btn_browse)
        f.addRow("local file", _wrap(rb))
        self.ed_put_remote = QLineEdit("/upload.bin")
        rput = QHBoxLayout()
        self.btn_put = QPushButton("Upload ↑")
        rput.addWidget(self.ed_put_remote, 1); rput.addWidget(self.btn_put)
        f.addRow("remote put", _wrap(rput))
        self.lbl_files = QLabel("downloads → %s" % DL_DIR)
        self.lbl_files.setWordWrap(True)
        f.addRow(self.lbl_files)
        return w

    def _tab_stream(self):
        w = QWidget(); v = QVBoxLayout(w)
        row = QHBoxLayout()
        self.cbo_kind = QComboBox(); self.cbo_kind.addItems(["telemetry", "wifi", "nrf"])
        self.spn_dur = QSpinBox(); self.spn_dur.setRange(1, 120); self.spn_dur.setValue(5)
        self.spn_dur.setSuffix(" s")
        self.btn_stream = QPushButton("Start stream")
        row.addWidget(QLabel("kind")); row.addWidget(self.cbo_kind)
        row.addWidget(QLabel("duration")); row.addWidget(self.spn_dur)
        row.addWidget(self.btn_stream); row.addStretch(1)
        v.addLayout(row)
        self.txt_stream = QPlainTextEdit(readOnly=True); self.txt_stream.setFont(_mono())
        v.addWidget(self.txt_stream, 1)
        return w

    def _tab_analyze(self):
        w = QWidget(); v = QVBoxLayout(w)
        row = QHBoxLayout()
        self.ed_an_remote = QLineEdit("/nrf_scan.log")
        self.btn_analyze = QPushButton("Fetch + analyze")
        row.addWidget(QLabel("remote")); row.addWidget(self.ed_an_remote, 1)
        row.addWidget(self.btn_analyze)
        v.addLayout(row)
        self.txt_report = QPlainTextEdit(readOnly=True); self.txt_report.setFont(_mono())
        v.addWidget(self.txt_report, 1)
        return w

    # ---------- worker plumbing ----------
    def _start_worker(self):
        self.thread = QThread(self)
        self.worker = DeviceWorker()
        self.worker.moveToThread(self.thread)
        self.thread.start()

        self.worker.connected.connect(self._on_connected)
        self.worker.disconnected.connect(self._on_disconnected)
        self.worker.log.connect(self._log)
        self.worker.status.connect(self.txt_status.setPlainText)
        self.worker.response.connect(self._on_response)
        self.worker.report.connect(self.txt_report.setPlainText)
        self.worker.stream_done.connect(self._on_stream_done)
        self.worker.error.connect(self._on_error)

        self.sig_connect.connect(self.worker.do_connect)
        self.sig_disconnect.connect(self.worker.do_disconnect)
        self.sig_request.connect(self.worker.do_request)
        self.sig_status.connect(self.worker.do_status)
        self.sig_file_get.connect(self.worker.do_file_get)
        self.sig_file_put.connect(self.worker.do_file_put)
        self.sig_analyze.connect(self.worker.do_analyze)
        self.sig_stream.connect(self.worker.do_stream)

    def _wire(self):
        self.btn_connect.clicked.connect(self._toggle_connect)
        self.ed_cmd.returnPressed.connect(self._send_cmd)
        self.btn_send.clicked.connect(self._send_cmd)
        self.btn_refresh.clicked.connect(lambda: self.sig_status.emit())
        self.btn_get.clicked.connect(self._do_get)
        self.btn_browse.clicked.connect(self._browse)
        self.btn_put.clicked.connect(self._do_put)
        self.btn_stream.clicked.connect(self._do_stream)
        self.btn_analyze.clicked.connect(
            lambda: self.sig_analyze.emit(self.ed_an_remote.text().strip()))

    # ---------- actions ----------
    def _toggle_connect(self):
        if self._connected:
            self.sig_disconnect.emit()
        else:
            self.lbl_conn.setText("● connecting…")
            self.lbl_conn.setStyleSheet("color:#d35400;")
            self.btn_connect.setEnabled(False)
            self.sig_connect.emit(
                self.cbo_transport.currentText(), self.ed_port.text().strip(),
                self.ed_name.text().strip() or "Bruc", self.ed_token.text())

    def _send_cmd(self):
        cmd = self.ed_cmd.text().strip()
        if not cmd:
            return
        self.ed_cmd.clear()
        self._log("<b>&gt; %s</b>" % _esc(cmd))
        self.sig_request.emit(cmd, 8.0)

    def _do_get(self):
        remote = self.ed_get_remote.text().strip()
        if not remote:
            return
        os.makedirs(DL_DIR, exist_ok=True)
        local = os.path.join(DL_DIR, os.path.basename(remote))
        self.sig_file_get.emit(remote, local)

    def _browse(self):
        path, _ = QFileDialog.getOpenFileName(self, "Choose file to upload")
        if path:
            self.ed_put_local.setText(path)
            if self.ed_put_remote.text() in ("", "/upload.bin"):
                self.ed_put_remote.setText("/" + os.path.basename(path))

    def _do_put(self):
        local = self.ed_put_local.text().strip()
        remote = self.ed_put_remote.text().strip()
        if local and remote:
            self.sig_file_put.emit(local, remote)

    def _do_stream(self):
        self.txt_stream.clear()
        self.btn_stream.setEnabled(False)
        self.sig_stream.emit(self.cbo_kind.currentText(), float(self.spn_dur.value()))

    # ---------- worker callbacks ----------
    @Slot(dict)
    def _on_connected(self, info):
        self._connected = True
        self.btn_connect.setText("Disconnect")
        self.btn_connect.setEnabled(True)
        self.lbl_conn.setText("● connected")
        self.lbl_conn.setStyleSheet("color:#27ae60;")
        caps = ", ".join(info.get("caps", [])) if isinstance(info.get("caps"), list) else info.get("caps", "")
        self.lbl_info.setText(
            "<b>%s</b><br>%s<br>mtu=%s  proto=%s<br><span style='color:#888'>caps:</span> %s" % (
                _esc(str(info.get("fw"))), _esc(str(info.get("board"))),
                info.get("mtu"), info.get("proto"), _esc(caps)))
        self._log("<span style='color:#27ae60'>connected</span> %s" % _esc(str(info.get("fw"))))
        self._set_enabled(True)
        self.sig_status.emit()

    @Slot()
    def _on_disconnected(self):
        self._connected = False
        self.btn_connect.setText("Connect")
        self.btn_connect.setEnabled(True)
        self.lbl_conn.setText("● disconnected")
        self.lbl_conn.setStyleSheet("color:#c0392b;")
        self._set_enabled(False)
        self._log("<span style='color:#888'>disconnected</span>")

    @Slot(str, list, int, str)
    def _on_response(self, cmd, lines, code, error):
        for ln in lines:
            self._log("  " + _esc(ln))
        color = "#27ae60" if (code == 0 and not error) else "#c0392b"
        tail = (" " + error) if error else ""
        self._log("<span style='color:%s'>· END code=%d%s</span>" % (color, code, _esc(tail)))

    @Slot(list, list)
    def _on_stream_done(self, start, events):
        self.btn_stream.setEnabled(True)
        if start:
            self.txt_stream.appendPlainText("start: " + " ".join(start))
        for e in events:
            self.txt_stream.appendPlainText("EVT " + e)
        self.txt_stream.appendPlainText("— %d event(s) —" % len(events))

    @Slot(str)
    def _on_error(self, msg):
        self.btn_connect.setEnabled(True)
        self.btn_stream.setEnabled(True)
        if not self._connected:
            self.lbl_conn.setText("● disconnected")
            self.lbl_conn.setStyleSheet("color:#c0392b;")
        self._log("<span style='color:#c0392b'>%s</span>" % _esc(msg))
        self.statusBar().showMessage(msg, 6000)

    # ---------- helpers ----------
    def _set_enabled(self, on):
        for wdg in (self.ed_cmd, self.btn_send, self.btn_refresh, self.btn_get,
                    self.btn_put, self.btn_browse, self.btn_stream, self.btn_analyze):
            wdg.setEnabled(on)

    def _log(self, html):
        self.txt_log.append(html)
        self.txt_log.moveCursor(QTextCursor.End)

    def closeEvent(self, ev):
        try:
            self.sig_disconnect.emit()
            self.thread.quit()
            self.thread.wait(2000)
        finally:
            super().closeEvent(ev)


def _mono():
    f = QFont("monospace")
    f.setStyleHint(QFont.Monospace)
    f.setPointSize(9)
    return f


def _wrap(layout):
    w = QWidget(); w.setLayout(layout); return w


def _esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM1")
    args = ap.parse_args()
    app = QApplication(sys.argv)
    win = MainWindow(args.port)
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
