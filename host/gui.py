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

from PySide6.QtCore import Qt, QObject, QThread, Signal, Slot, QTimer
from PySide6.QtGui import QFont, QTextCursor
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QLabel, QLineEdit, QPushButton,
    QComboBox, QTextEdit, QPlainTextEdit, QTabWidget, QFileDialog, QSpinBox,
    QHBoxLayout, QVBoxLayout, QFormLayout, QGroupBox, QSplitter,
    QListWidget, QStackedWidget, QScrollArea, QMessageBox, QFrame, QCheckBox,
    QTreeWidget, QTreeWidgetItem,
)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from companion_proto import Companion  # noqa: E402
import companion_commands as cc  # noqa: E402

DL_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "downloads")
CAP_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "captures")


class HistoryLineEdit(QLineEdit):
    """A command input with Up/Down history recall."""
    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self._hist = []
        self._idx = 0

    def remember(self, text):
        if text and (not self._hist or self._hist[-1] != text):
            self._hist.append(text)
        self._idx = len(self._hist)

    def keyPressEvent(self, e):
        if e.key() == Qt.Key_Up and self._hist:
            self._idx = max(0, self._idx - 1)
            self.setText(self._hist[self._idx])
            return
        if e.key() == Qt.Key_Down and self._hist:
            self._idx = min(len(self._hist), self._idx + 1)
            self.setText(self._hist[self._idx] if self._idx < len(self._hist) else "")
            return
        super().keyPressEvent(e)


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
    listing = Signal(str, list)  # path, [(name, is_dir, size)]
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

    @Slot(str)
    def do_list(self, path):
        if self.dev is None:
            self.error.emit("not connected")
            return
        try:
            r = self.dev.request("ls " + path, timeout=12.0)
            entries = []
            for ln in r.lines:
                if "\t" not in ln:
                    continue
                name, _, rest = ln.partition("\t")
                rest = rest.strip()
                is_dir = (rest == "<DIR>")
                size = -1 if is_dir else (int(rest) if rest.isdigit() else 0)
                if name.strip():
                    entries.append((name.strip(), is_dir, size))
            self.listing.emit(path, entries)
        except Exception as e:  # noqa: BLE001
            self.error.emit("list error: %s" % e)

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
    sig_list = Signal(str)

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
        rrow = QHBoxLayout()
        self.btn_refresh = QPushButton("Refresh status")
        self.chk_auto = QCheckBox("auto (2s)")
        rrow.addWidget(self.btn_refresh); rrow.addWidget(self.chk_auto)
        gsv.addLayout(rrow)
        lv.addWidget(gs, 1)
        self._auto_timer = QTimer(self)
        self._auto_timer.setInterval(2000)
        self._auto_timer.timeout.connect(self._auto_tick)
        split.addWidget(left)

        # right: tabs
        self.tabs = QTabWidget()
        self.tabs.addTab(self._tab_functions(), "Functions")
        self.tabs.addTab(self._tab_console(), "Console")
        self.tabs.addTab(self._tab_files(), "Files")
        self.tabs.addTab(self._tab_stream(), "Stream")
        self.tabs.addTab(self._tab_analyze(), "Analyze")
        self.tabs.addTab(self._tab_dicts(), "Dictionaries")
        split.addWidget(self.tabs)
        split.setSizes([300, 700])

        self.statusBar().showMessage("ready")
        self._set_enabled(False)

    def _tab_functions(self):
        """Every firmware function as a button (with arg fields), driven by the
        shared companion_commands catalog. Left = group list, right = command
        panel for the selected group, bottom = shared output."""
        w = QWidget(); v = QVBoxLayout(w)
        split = QSplitter(Qt.Horizontal)
        self.fn_groups = QListWidget()
        self.fn_groups.setMaximumWidth(190)
        self.fn_stack = QStackedWidget()
        for name, cmds in cc.all_groups():
            self.fn_groups.addItem(name)
            self.fn_stack.addWidget(self._group_panel(cmds))
        self.fn_groups.currentRowChanged.connect(self.fn_stack.setCurrentIndex)
        self.fn_groups.setCurrentRow(0)
        split.addWidget(self.fn_groups)
        split.addWidget(self.fn_stack)
        split.setSizes([190, 560])
        v.addWidget(split, 1)
        self.txt_fn_out = QPlainTextEdit(readOnly=True)
        self.txt_fn_out.setFont(_mono())
        self.txt_fn_out.setMaximumHeight(150)
        v.addWidget(self.txt_fn_out)
        return w

    def _group_panel(self, cmds):
        area = QScrollArea(); area.setWidgetResizable(True)
        inner = QWidget(); col = QVBoxLayout(inner)
        for cmd in cmds:
            col.addWidget(self._cmd_row(cmd))
        col.addStretch(1)
        area.setWidget(inner)
        return area

    def _cmd_row(self, cmd):
        frame = QFrame(); frame.setFrameShape(QFrame.StyledPanel)
        row = QHBoxLayout(frame); row.setContentsMargins(6, 3, 6, 3)
        btn = QPushButton(cmd.label)
        btn.setMinimumWidth(150)
        if cmd.kind == "danger":
            btn.setStyleSheet("QPushButton{color:#c0392b;font-weight:bold;}")
        tip = cmd.desc + (f"  [{cmd.kind}]" if cmd.kind != "oneshot" else "")
        if tip.strip():
            btn.setToolTip(tip); frame.setToolTip(tip)
        row.addWidget(btn)
        inputs = []
        for arg in cmd.args:
            if arg.choices:
                cb = QComboBox(); cb.setEditable(True)
                cb.addItems(list(arg.choices))
                if arg.default:
                    cb.setCurrentText(arg.default)
                cb.setMinimumWidth(80)
                inputs.append(("combo", cb)); row.addWidget(cb)
            else:
                ed = QLineEdit(arg.default)
                ed.setPlaceholderText(arg.name + ("" if arg.required else " (opt)"))
                inputs.append(("edit", ed)); row.addWidget(ed, 1)
        if not cmd.args:
            row.addStretch(1)
        btn.clicked.connect(lambda _=False, c=cmd, w=inputs: self._run_catalog(c, w))
        return frame

    def _run_catalog(self, cmd, inputs):
        if not self._connected:
            self._fn_log("[not connected]"); return
        vals = [(w.currentText() if kind == "combo" else w.text()) for kind, w in inputs]
        try:
            line = cc.build_command(cmd, vals)
        except ValueError as e:
            self._fn_log(f"[{e}]"); self.statusBar().showMessage(str(e), 4000); return
        if cmd.kind == "danger":
            r = QMessageBox.question(self, "Confirm", f"Run:\n\n  {line}\n\n{cmd.desc}",
                                     QMessageBox.Yes | QMessageBox.No, QMessageBox.No)
            if r != QMessageBox.Yes:
                return
        self._fn_log(f"> {line}")
        self._log(f"<b>&gt; {_esc(line)}</b>")
        self.sig_request.emit(line, float(cmd.timeout))

    def _fn_log(self, text):
        self.txt_fn_out.appendPlainText(text)

    def _tab_console(self):
        w = QWidget(); v = QVBoxLayout(w)
        self.txt_log = QTextEdit(readOnly=True)
        self.txt_log.setFont(_mono())
        v.addWidget(self.txt_log, 1)
        row = QHBoxLayout()
        self.ed_cmd = HistoryLineEdit()
        self.ed_cmd.setPlaceholderText("device CLI command (↑/↓ history) e.g. free · ls / · wifi scan")
        self.btn_send = QPushButton("Send")
        row.addWidget(self.ed_cmd, 1); row.addWidget(self.btn_send)
        v.addLayout(row)
        return w

    def _tab_files(self):
        w = QWidget(); v = QVBoxLayout(w)
        # --- device file browser ---
        nav = QHBoxLayout()
        self.btn_fb_up = QPushButton("↑ Up")
        self.btn_fb_refresh = QPushButton("⟳")
        self.lbl_cwd = QLineEdit("/"); self.lbl_cwd.setReadOnly(True)
        nav.addWidget(self.btn_fb_up); nav.addWidget(self.btn_fb_refresh)
        nav.addWidget(QLabel("path")); nav.addWidget(self.lbl_cwd, 1)
        v.addLayout(nav)
        self.fb_list = QListWidget(); self.fb_list.setFont(_mono())
        v.addWidget(self.fb_list, 1)
        acts = QHBoxLayout()
        self.btn_fb_dl = QPushButton("Download")
        self.btn_fb_view = QPushButton("View")
        self.btn_fb_del = QPushButton("Delete")
        self.btn_fb_del.setStyleSheet("QPushButton{color:#c0392b;}")
        acts.addWidget(self.btn_fb_dl); acts.addWidget(self.btn_fb_view)
        acts.addWidget(self.btn_fb_del); acts.addStretch(1)
        v.addLayout(acts)
        self._cwd = "/"
        self._fb_entries = []  # aligned with fb_list rows: (name, is_dir, size)

        # --- manual get/put (kept) ---
        form = QFormLayout()
        self.ed_get_remote = QLineEdit("/bruce.conf")
        rget = QHBoxLayout()
        self.btn_get = QPushButton("Download →")
        rget.addWidget(self.ed_get_remote, 1); rget.addWidget(self.btn_get)
        form.addRow("remote get", _wrap(rget))
        self.ed_put_local = QLineEdit()
        self.btn_browse = QPushButton("Browse…")
        rb = QHBoxLayout(); rb.addWidget(self.ed_put_local, 1); rb.addWidget(self.btn_browse)
        form.addRow("local file", _wrap(rb))
        self.ed_put_remote = QLineEdit("/upload.bin")
        rput = QHBoxLayout()
        self.btn_put = QPushButton("Upload ↑")
        rput.addWidget(self.ed_put_remote, 1); rput.addWidget(self.btn_put)
        form.addRow("remote put", _wrap(rput))
        self.lbl_files = QLabel("downloads → %s" % DL_DIR)
        self.lbl_files.setWordWrap(True)
        form.addRow(self.lbl_files)
        v.addLayout(form)
        return w

    # ---------- file browser ----------
    def _fb_refresh(self):
        if self._connected:
            self.lbl_cwd.setText(self._cwd)
            self.sig_list.emit(self._cwd)

    def _fb_up(self):
        if self._cwd not in ("/", ""):
            self._cwd = self._cwd.rsplit("/", 1)[0] or "/"
            self._fb_refresh()

    def _fb_join(self, name):
        return (self._cwd.rstrip("/") + "/" + name) if self._cwd != "/" else "/" + name

    def _fb_selected(self):
        row = self.fb_list.currentRow()
        if 0 <= row < len(self._fb_entries):
            return self._fb_entries[row]
        return None

    def _fb_activate(self, item):
        e = self._fb_selected()
        if not e:
            return
        name, is_dir, _ = e
        if is_dir:
            self._cwd = self._fb_join(name)
            self._fb_refresh()
        else:
            self._fb_download(self._fb_join(name))

    def _fb_download(self, remote):
        os.makedirs(DL_DIR, exist_ok=True)
        local = os.path.join(DL_DIR, os.path.basename(remote))
        self.sig_file_get.emit(remote, local)

    def _fb_dl_clicked(self):
        e = self._fb_selected()
        if e and not e[1]:
            self._fb_download(self._fb_join(e[0]))

    def _fb_view_clicked(self):
        e = self._fb_selected()
        if e and not e[1]:
            self.sig_request.emit("cat " + self._fb_join(e[0]), 12.0)
            self.tabs.setCurrentIndex(1)  # show Console where output lands

    def _fb_del_clicked(self):
        e = self._fb_selected()
        if not e:
            return
        name, is_dir, _ = e
        path = self._fb_join(name)
        kind = "directory" if is_dir else "file"
        if QMessageBox.question(self, "Delete", f"Delete {kind}:\n  {path} ?",
                                QMessageBox.Yes | QMessageBox.No, QMessageBox.No) != QMessageBox.Yes:
            return
        self.sig_request.emit(("rmdir " if is_dir else "rm ") + path, 8.0)
        QTimer.singleShot(700, self._fb_refresh)

    @Slot(str, list)
    def _on_listing(self, path, entries):
        self.lbl_cwd.setText(path)
        self.fb_list.clear()
        self._fb_entries = sorted(entries, key=lambda e: (not e[1], e[0].lower()))
        for name, is_dir, size in self._fb_entries:
            label = f"📁 {name}/" if is_dir else f"   {name}   ({_human(size)})"
            self.fb_list.addItem(label)

    # ---------- dictionaries ----------
    def _tab_dicts(self):
        w = QWidget(); v = QVBoxLayout(w)
        self.dict_tree = QTreeWidget(); self.dict_tree.setHeaderHidden(True)
        self.dict_tree.setFont(_mono())
        v.addWidget(self.dict_tree, 1)
        acts = QHBoxLayout()
        self.btn_dict_send = QPushButton("Send (ir tx)")
        self.btn_dict_deploy = QPushButton("Deploy to device")
        self.btn_dict_tx = QPushButton("Upload + TX file")
        self.btn_dict_reload = QPushButton("⟳ Reload")
        for b in (self.btn_dict_send, self.btn_dict_deploy, self.btn_dict_tx, self.btn_dict_reload):
            acts.addWidget(b)
        acts.addStretch(1)
        v.addLayout(acts)
        self.txt_dict = QPlainTextEdit(readOnly=True); self.txt_dict.setMaximumHeight(120)
        self.txt_dict.setFont(_mono())
        v.addWidget(self.txt_dict)
        self._build_dict_tree()
        return w

    def _build_dict_tree(self):
        import companion_dicts as cd
        self.dict_tree.clear()
        # IR
        ir_root = QTreeWidgetItem(["IR signals"])
        self.dict_tree.addTopLevelItem(ir_root)
        by_brand = {}
        for e in cd.ir_entries():
            by_brand.setdefault(e["brand"], []).append(e)
        for brand in sorted(by_brand):
            bnode = QTreeWidgetItem([brand])
            bnode.setData(0, Qt.UserRole, {"kind": "ir_file", "path": by_brand[brand][0]["path"]})
            ir_root.addChild(bnode)
            for e in by_brand[brand]:
                sline = cd.ir_tx_line(e) or "(raw)"
                leaf = QTreeWidgetItem([f"{e['name']}  [{e.get('protocol','?')}]  {sline}"])
                leaf.setData(0, Qt.UserRole, {"kind": "ir_sig", "sig": e})
                bnode.addChild(leaf)
        # RFID keys
        rfid_root = QTreeWidgetItem(["RFID key dictionaries"])
        self.dict_tree.addTopLevelItem(rfid_root)
        for p in cd.key_files():
            n = len(cd.parse_keys(p))
            it = QTreeWidgetItem([f"{os.path.basename(p)}  ({n} keys)"])
            it.setData(0, Qt.UserRole, {"kind": "keys", "path": p})
            rfid_root.addChild(it)
        # sub-GHz
        sub_root = QTreeWidgetItem(["Sub-GHz captures"])
        self.dict_tree.addTopLevelItem(sub_root)
        subs = cd.sub_files()
        if not subs:
            sub_root.addChild(QTreeWidgetItem(["(drop .sub files in dictionaries/subghz/)"]))
        for p in subs:
            it = QTreeWidgetItem([os.path.basename(p)])
            it.setData(0, Qt.UserRole, {"kind": "sub", "path": p})
            sub_root.addChild(it)
        ir_root.setExpanded(True); rfid_root.setExpanded(True)

    def _dict_data(self):
        it = self.dict_tree.currentItem()
        return it.data(0, Qt.UserRole) if it else None

    def _dlog(self, msg):
        self.txt_dict.appendPlainText(msg)

    def _dict_send(self):
        import companion_dicts as cd
        d = self._dict_data()
        if not d or d.get("kind") != "ir_sig":
            self._dlog("select an IR signal to Send"); return
        line = cd.ir_tx_line(d["sig"])
        if not line:
            self._dlog("raw IR — use Upload + TX file"); return
        self._dlog("> " + line)
        self.sig_request.emit(line, 8.0)

    def _dict_deploy(self):
        import companion_dicts as cd
        d = self._dict_data()
        if not d:
            self._dlog("select an item to deploy"); return
        kind = d["kind"]
        try:
            if kind in ("ir_sig", "ir_file"):
                local = d["sig"]["path"] if kind == "ir_sig" else d["path"]
                remote = cd.deploy_remote("ir", local)
                self._dlog(f"upload {os.path.basename(local)} -> {remote}")
                self.sig_file_put.emit(local, remote)
            elif kind == "keys":
                os.makedirs(CAP_DIR, exist_ok=True)
                tmp = os.path.join(CAP_DIR, "keys.conf")
                with open(tmp, "w") as fh:
                    fh.write(cd.build_keys_conf([d["path"]]))
                self._dlog(f"upload merged keys.conf -> {cd.DEV_RFID_KEYS}")
                self.sig_file_put.emit(tmp, cd.DEV_RFID_KEYS)
            elif kind == "sub":
                remote = cd.deploy_remote("subghz", d["path"])
                self._dlog(f"upload {os.path.basename(d['path'])} -> {remote}")
                self.sig_file_put.emit(d["path"], remote)
        except Exception as e:  # noqa: BLE001
            self._dlog(f"deploy error: {e}")

    def _dict_tx(self):
        """Upload the file then transmit it from the device (IR/sub)."""
        import companion_dicts as cd
        d = self._dict_data()
        if not d:
            return
        kind = d["kind"]
        if kind in ("ir_sig", "ir_file"):
            local = d["sig"]["path"] if kind == "ir_sig" else d["path"]
            remote = cd.deploy_remote("ir", local)
            self.sig_file_put.emit(local, remote)
            self._dlog(f"upload+tx {remote}")
            QTimer.singleShot(1500, lambda: self.sig_request.emit(f"ir tx_from_file {remote}", 15.0))
        elif kind == "sub":
            remote = cd.deploy_remote("subghz", d["path"])
            self.sig_file_put.emit(d["path"], remote)
            self._dlog(f"upload+tx {remote}")
            QTimer.singleShot(1500, lambda: self.sig_request.emit(f"rf tx_from_file {remote}", 15.0))
        else:
            self._dlog("Upload + TX applies to IR / sub-GHz files")

    def _tab_stream(self):
        w = QWidget(); v = QVBoxLayout(w)
        row = QHBoxLayout()
        self.cbo_kind = QComboBox(); self.cbo_kind.addItems(["telemetry", "wifi", "nrf", "rf"])
        self.cbo_kind.currentTextChanged.connect(self._stream_kind_changed)
        self.spn_dur = QSpinBox(); self.spn_dur.setRange(1, 120); self.spn_dur.setValue(5)
        self.spn_dur.setSuffix(" s")
        self.btn_stream = QPushButton("Start stream")
        self.btn_stream_an = QPushButton("Analyze last")
        row.addWidget(QLabel("kind")); row.addWidget(self.cbo_kind)
        row.addWidget(QLabel("dur")); row.addWidget(self.spn_dur)
        # rf band (MHz), shown only for kind=rf
        self.lbl_rf = QLabel("band MHz")
        self.ed_rf0 = QLineEdit("433.0"); self.ed_rf0.setMaximumWidth(70)
        self.ed_rf1 = QLineEdit("434.8"); self.ed_rf1.setMaximumWidth(70)
        for x in (self.lbl_rf, self.ed_rf0, self.ed_rf1):
            row.addWidget(x); x.setVisible(False)
        self.btn_stream_save = QPushButton("Save")
        self.btn_stream_load = QPushButton("Load…")
        row.addWidget(self.btn_stream); row.addWidget(self.btn_stream_an)
        row.addWidget(self.btn_stream_save); row.addWidget(self.btn_stream_load)
        row.addStretch(1)
        v.addLayout(row)
        self.txt_stream = QPlainTextEdit(readOnly=True); self.txt_stream.setFont(_mono())
        v.addWidget(self.txt_stream, 1)
        self._last_stream = ("", [])  # (kind, events) for "Analyze last"/"Save"
        return w

    def _save_stream(self):
        kind, events = self._last_stream
        if not events:
            self.txt_stream.appendPlainText("\n(no stream to save — run one first)")
            return
        try:
            import companion_compute
            path = companion_compute.save_stream(kind, events, CAP_DIR)
            self.txt_stream.appendPlainText(f"\nsaved {len(events)} events -> {path}")
        except Exception as e:  # noqa: BLE001
            self.txt_stream.appendPlainText(f"\nsave error: {e}")

    def _load_stream(self):
        os.makedirs(CAP_DIR, exist_ok=True)
        path, _ = QFileDialog.getOpenFileName(self, "Load capture", CAP_DIR, "Captures (*.txt);;All (*)")
        if not path:
            return
        try:
            import companion_compute
            rep = companion_compute.analyze_stream_file(path)
        except Exception as e:  # noqa: BLE001
            rep = f"load error: {e}"
        self.txt_stream.appendPlainText(f"\n──── {os.path.basename(path)} ────\n" + rep)

    def _stream_kind_changed(self, kind):
        rf = (kind == "rf")
        for x in (self.lbl_rf, self.ed_rf0, self.ed_rf1):
            x.setVisible(rf)

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
        self.worker.listing.connect(self._on_listing)
        self.worker.error.connect(self._on_error)

        self.sig_connect.connect(self.worker.do_connect)
        self.sig_disconnect.connect(self.worker.do_disconnect)
        self.sig_request.connect(self.worker.do_request)
        self.sig_status.connect(self.worker.do_status)
        self.sig_file_get.connect(self.worker.do_file_get)
        self.sig_file_put.connect(self.worker.do_file_put)
        self.sig_analyze.connect(self.worker.do_analyze)
        self.sig_stream.connect(self.worker.do_stream)
        self.sig_list.connect(self.worker.do_list)

    def _wire(self):
        self.btn_connect.clicked.connect(self._toggle_connect)
        self.ed_cmd.returnPressed.connect(self._send_cmd)
        self.btn_send.clicked.connect(self._send_cmd)
        self.btn_refresh.clicked.connect(lambda: self.sig_status.emit())
        self.btn_get.clicked.connect(self._do_get)
        self.btn_browse.clicked.connect(self._browse)
        self.btn_put.clicked.connect(self._do_put)
        self.btn_stream.clicked.connect(self._do_stream)
        self.btn_stream_an.clicked.connect(self._analyze_last_stream)
        self.btn_fb_up.clicked.connect(self._fb_up)
        self.btn_fb_refresh.clicked.connect(self._fb_refresh)
        self.fb_list.itemDoubleClicked.connect(self._fb_activate)
        self.btn_fb_dl.clicked.connect(self._fb_dl_clicked)
        self.btn_fb_view.clicked.connect(self._fb_view_clicked)
        self.btn_fb_del.clicked.connect(self._fb_del_clicked)
        self.btn_stream_save.clicked.connect(self._save_stream)
        self.btn_stream_load.clicked.connect(self._load_stream)
        self.chk_auto.toggled.connect(self._toggle_auto)
        self.btn_dict_send.clicked.connect(self._dict_send)
        self.btn_dict_deploy.clicked.connect(self._dict_deploy)
        self.btn_dict_tx.clicked.connect(self._dict_tx)
        self.btn_dict_reload.clicked.connect(self._build_dict_tree)
        self.dict_tree.itemDoubleClicked.connect(lambda *_: self._dict_send())

    def _toggle_auto(self, on):
        if on and self._connected:
            self._auto_timer.start()
        else:
            self._auto_timer.stop()

    def _auto_tick(self):
        if self._connected:
            self.sig_status.emit()
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
        kind = self.cbo_kind.currentText()
        if kind == "rf":
            a = self.ed_rf0.text().strip() or "433.0"
            b = self.ed_rf1.text().strip() or "434.8"
            kind = f"rf {a} {b}"
        self._stream_kind_base = self.cbo_kind.currentText()
        self.sig_stream.emit(kind, float(self.spn_dur.value()))

    def _analyze_last_stream(self):
        kind, events = self._last_stream
        if not events:
            self.txt_stream.appendPlainText("\n(no stream to analyze — run one first)")
            return
        try:
            import companion_compute
            rep = companion_compute.analyze_stream(kind, events)
        except Exception as e:  # noqa: BLE001
            rep = f"analyze error: {e}"
        self.txt_stream.appendPlainText("\n──── analysis ────\n" + rep)

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
        self._fb_refresh()  # populate the device file browser
        if self.chk_auto.isChecked():
            self._auto_timer.start()

    @Slot()
    def _on_disconnected(self):
        self._connected = False
        self._auto_timer.stop()
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
        # mirror into the Functions output box (so button results are visible there)
        if hasattr(self, "txt_fn_out"):
            body = "\n".join("  " + l for l in lines) if lines else "  (no output)"
            self.txt_fn_out.appendPlainText(body + f"\n· END code={code}{tail}")

    @Slot(list, list)
    def _on_stream_done(self, start, events):
        self.btn_stream.setEnabled(True)
        if start:
            self.txt_stream.appendPlainText("start: " + " ".join(start))
        for e in events:
            self.txt_stream.appendPlainText("EVT " + e)
        self.txt_stream.appendPlainText("— %d event(s) —" % len(events))
        self._last_stream = (getattr(self, "_stream_kind_base", "telemetry"), list(events))
        # auto-analyze radio streams for an instant readable summary
        if self._last_stream[0] in ("wifi", "nrf", "rf") and events:
            self._analyze_last_stream()

    @Slot(str)
    def _on_error(self, msg):
        self.btn_connect.setEnabled(True)
        self.btn_stream.setEnabled(True)
        if not self._connected:
            self.lbl_conn.setText("● disconnected")
            self.lbl_conn.setStyleSheet("color:#c0392b;")
        self._log("<span style='color:#c0392b'>%s</span>" % _esc(msg))
        if hasattr(self, "txt_fn_out"):
            self.txt_fn_out.appendPlainText(msg)
        self.statusBar().showMessage(msg, 6000)

    # ---------- helpers ----------
    def _set_enabled(self, on):
        for wdg in (self.ed_cmd, self.btn_send, self.btn_refresh, self.btn_get,
                    self.btn_put, self.btn_browse, self.btn_stream, self.btn_analyze,
                    self.btn_fb_up, self.btn_fb_refresh, self.btn_fb_dl,
                    self.btn_fb_view, self.btn_fb_del,
                    self.btn_dict_send, self.btn_dict_deploy, self.btn_dict_tx):
            wdg.setEnabled(on)
        # "Analyze last" works offline on already-collected events
        self.btn_stream_an.setEnabled(True)

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


def _human(n):
    if n < 0:
        return ""
    for u in ("B", "K", "M", "G"):
        if n < 1024 or u == "G":
            return f"{n}{u}" if u == "B" else f"{n:.1f}{u}"
        n /= 1024


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
