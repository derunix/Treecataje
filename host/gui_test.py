#!/usr/bin/env python3
"""Headless smoke test for gui.py — runs Qt offscreen and drives the real
worker/signal path end to end against the device (USB by default).

  QT_QPA_PLATFORM=offscreen host/.venv/bin/python host/gui_test.py [--port ...]

Checks: window builds, connect() reaches the device, a command round-trips,
status fills in, then clean disconnect. No hardware? it still validates that the
UI constructs and wires without error (connect will just report an error).
"""
import os
import sys
import argparse

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PySide6.QtWidgets import QApplication
from PySide6.QtCore import QTimer, QEventLoop

import gui


def _spin(ms):
    loop = QEventLoop()
    QTimer.singleShot(ms, loop.quit)
    loop.exec()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM1")
    ap.add_argument("--no-device", action="store_true",
                    help="only test UI construction/wiring, skip device I/O")
    args = ap.parse_args()

    app = QApplication([])
    win = gui.MainWindow(args.port)
    win.show()
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and cond
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))

    # construction
    check("window built", win.tabs.count() == 7, f"{win.tabs.count()} tabs")
    check("attack tab present", win.tabs.tabText(4) == "Attack", win.tabs.tabText(4))
    check("functions groups populated", win.fn_groups.count() >= 10, f"{win.fn_groups.count()} groups")
    check("dictionaries populated", win.dict_tree.topLevelItemCount() == 3,
          f"{win.dict_tree.topLevelItemCount()} categories")
    check("console disabled pre-connect", not win.btn_send.isEnabled())

    if args.no_device:
        win.close(); app.processEvents()
        print("\n" + ("UI OK (no-device)" if ok else "UI FAILURES"))
        return 0 if ok else 1

    # connect to the real device through the worker thread
    win.cbo_transport.setCurrentText("usb")
    win.ed_port.setText(args.port)
    win._toggle_connect()
    for _ in range(60):           # up to ~6s
        _spin(100)
        if win._connected:
            break
    check("connected to device", win._connected)

    if win._connected:
        check("console enabled post-connect", win.btn_send.isEnabled())
        check("device info populated", "fw" not in (win.lbl_info.text() or "—")
              and len(win.lbl_info.text()) > 3, win.lbl_info.text()[:40])
        # status auto-refreshes on connect; give it a moment
        for _ in range(40):
            _spin(100)
            if win.txt_status.toPlainText().strip():
                break
        check("status populated", bool(win.txt_status.toPlainText().strip()),
              win.txt_status.toPlainText().splitlines()[:1])
        # run a command
        win.ed_cmd.setText("free")
        win._send_cmd()
        for _ in range(40):
            _spin(100)
            if "END code=" in win.txt_log.toPlainText():
                break
        check("command round-tripped", "END code=" in win.txt_log.toPlainText())

        # run a catalog command via the Functions tab (Status group -> "Free memory")
        import companion_commands as cc
        free_cmd = next(c for _, cmds in cc.all_groups() for c in cmds if c.template == "free")
        win._run_catalog(free_cmd, [])
        for _ in range(40):
            _spin(100)
            if "END code=" in win.txt_fn_out.toPlainText():
                break
        check("functions button round-tripped", "END code=" in win.txt_fn_out.toPlainText(),
              win.txt_fn_out.toPlainText().splitlines()[-1:] )

        # device file browser: list "/" and check it populated
        win._cwd = "/"
        win._fb_refresh()
        for _ in range(40):
            _spin(100)
            if win.fb_list.count() > 0:
                break
        check("file browser listed /", win.fb_list.count() > 0, f"{win.fb_list.count()} entries")

    # Attack tab: scan results populate the table + selection sets the target
    aps = [{"bssid": "AA:BB:CC:DD:EE:01", "ch": 6, "ssid": "HomeNet", "rssi": -40, "enc": "wpa2"},
           {"bssid": "AA:BB:CC:DD:EE:02", "ch": 11, "ssid": "Other", "rssi": -70, "enc": "wpa/wpa2"}]
    win._on_scan_found(aps)
    check("scan populated AP table", win.tbl_aps.rowCount() == 2, f"{win.tbl_aps.rowCount()} rows")
    check("target auto-selected", "HomeNet" in win.lbl_target.text(), win.lbl_target.text())
    sel = win._selected_ap()
    check("selected AP resolves", sel and sel["bssid"] == "AA:BB:CC:DD:EE:01",
          sel["bssid"] if sel else "none")

    # disconnect
    win.sig_disconnect.emit()
    for _ in range(20):
        _spin(100)
        if not win._connected:
            break
    check("disconnected cleanly", not win._connected)

    win.close()
    app.processEvents()
    print("\n" + ("ALL PASS (gui)" if ok else "SOME FAILURES (gui)"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
