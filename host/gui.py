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
import time
import argparse

from PySide6.QtCore import Qt, QObject, QThread, Signal, Slot, QTimer
from PySide6.QtGui import QFont, QTextCursor
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QLabel, QLineEdit, QPushButton,
    QComboBox, QTextEdit, QPlainTextEdit, QTabWidget, QFileDialog, QSpinBox,
    QDoubleSpinBox,
    QHBoxLayout, QVBoxLayout, QFormLayout, QGroupBox, QSplitter,
    QListWidget, QStackedWidget, QScrollArea, QMessageBox, QFrame, QCheckBox,
    QTreeWidget, QTreeWidgetItem, QTableWidget, QTableWidgetItem, QHeaderView,
    QAbstractItemView,
)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from companion_proto import Companion  # noqa: E402
import companion_commands as cc  # noqa: E402

DL_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "downloads")
CAP_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "captures")
WORDLIST_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dictionaries", "wordlists")


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
    capture_done = Signal(dict, str)        # cap-meta dict, host analysis text
    crack_finished = Signal()               # a crack/attack run ended (re-enable UI)
    scan_found = Signal(list)               # [ap dict] from a WiFi scan
    alog = Signal(str)                      # one Attack-tab log line
    nrf_found = Signal(list)                # [nrf device dict]
    nlog = Signal(str)                      # one NRF24-tab log line
    audlog = Signal(str)                    # one Audio-tab log line
    listing = Signal(str, list)  # path, [(name, is_dir, size)]
    heap = Signal(int)           # free heap bytes (live telemetry)
    error = Signal(str)

    def __init__(self):
        super().__init__()
        self.dev = None
        self.transport = "usb"
        self._crack_cancel = None

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

    @Slot()
    def do_heap(self):
        if self.dev is None:
            return
        try:
            r = self.dev.request("free", timeout=6.0)
            for ln in r.lines:
                if "Free heap:" in ln:
                    digits = "".join(ch for ch in ln.split("Free heap:")[1] if ch.isdigit())
                    if digits:
                        self.heap.emit(int(digits))
                    return
        except Exception:  # noqa: BLE001
            pass

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

    @Slot(float)
    def do_recon(self, secs):
        if self.dev is None:
            self.error.emit("not connected")
            return
        try:
            import companion_compute as cc
            import time
            self.log.emit("recon: scanning wifi/nrf/rf …")
            fn = lambda k, s: self.dev.stream(k, duration=s).get("events", [])
            results = cc.run_recon(fn, wifi_s=secs, nrf_s=max(2.0, secs * 0.7),
                                   rf_s=max(2.0, secs * 0.7))
            report = cc.recon_report(results, when=time.strftime("%Y-%m-%d %H:%M:%S"))
            self.report.emit(report)
        except Exception as e:  # noqa: BLE001
            self.error.emit("recon error: %s" % e)

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

    def cancel_crack(self):
        if self._crack_cancel is not None:
            self._crack_cancel.set()

    def do_crack(self, pcap, wordlist, remote, tool, brute):
        """Crack a WPA handshake with a real cracker (aircrack-ng/hashcat/python).
        If `remote` is set, fetch that pcap from the device first."""
        import threading
        self._crack_cancel = threading.Event()
        logs = []

        def log(m):
            logs.append(str(m)); self.report.emit("\n".join(logs))

        def ev(e):
            if e.get("type") == "progress":
                log("  %s tried @ %.0f/s" % (e.get("tested", "?"), e.get("rate", 0)))
        try:
            import crackers as ck
            local = pcap
            if remote:
                if self.dev is None:
                    self.error.emit("not connected (needed to fetch the pcap)")
                    return
                os.makedirs(CAP_DIR, exist_ok=True)
                local = os.path.join(CAP_DIR, os.path.basename(remote) or "hs.pcap")
                log("fetch %s …" % remote)
                got = self.dev.file_get(remote, local, 512, 120)
                log("fetched %s B" % got.get("size"))
            bssid = ck.detect_bssid(local)
            hc = local.rsplit(".", 1)[0] + ".hc22000"
            try:
                import wpa_crack as wcm
                wcm.export_hc22000(local, hc)
            except Exception:  # noqa: BLE001
                hc = ""
            log("cracking %s with %s (bssid %s)…" % (os.path.basename(local), tool, bssid or "?"))
            res = ck.crack_wordlist(local, wordlist, bssid=bssid, tool=tool, hc22000=hc,
                                    on_event=ev, cancel=self._crack_cancel)
            if not res["ok"] and brute and not self._crack_cancel.is_set():
                log("wordlist failed; brute 8-digit via %s …" % tool)
                res = ck.crack_brute(local, "0123456789", 8, bssid=bssid, tool=tool,
                                     hc22000=hc, on_event=ev, cancel=self._crack_cancel)
            if res.get("ok"):
                log("✓ KEY FOUND (%s): %s" % (res.get("tool"), res["key"]))
            elif self._crack_cancel.is_set():
                log("✗ cancelled")
            else:
                log("✗ not found (%s)" % res.get("tool"))
        except Exception as e:  # noqa: BLE001
            self.error.emit("crack error: %s" % e)
        finally:
            self.crack_finished.emit()

    def do_attack(self, ssid, wordlist, tool, brute):
        """Full WPA attack cycle (find→deauth→capture→crack→brute) on the device."""
        if self.dev is None:
            self.error.emit("not connected")
            self.crack_finished.emit()
            return
        import threading
        self._crack_cancel = threading.Event()
        logs = []

        def log(m):
            logs.append(str(m)); self.report.emit("\n".join(logs))
        try:
            import wifi_attack
            os.makedirs(CAP_DIR, exist_ok=True)
            out = wifi_attack.run_attack(self.dev, ssid=ssid, wordlist=wordlist, tool=tool,
                                         brute=brute, local_dir=CAP_DIR, log=log,
                                         cancel=self._crack_cancel)
            self.report.emit("\n".join(logs) + "\n\n" + wifi_attack.format_result(out))
        except Exception as e:  # noqa: BLE001
            self.error.emit("attack error: %s" % e)
        finally:
            self.crack_finished.emit()

    # ---------- Attack tab (scan → select → act) ----------
    def do_scan(self, secs):
        if self.dev is None:
            self.error.emit("not connected"); self.crack_finished.emit(); return
        try:
            self.alog.emit("scanning %.0fs …" % secs)
            aps = self.dev.scan_aps(scan_secs=secs)
            self.alog.emit("found %d APs" % len(aps))
            self.scan_found.emit(aps)
        except Exception as e:  # noqa: BLE001
            self.error.emit("scan error: %s" % e)
        finally:
            self.crack_finished.emit()

    def do_deauth(self, bssid, ch, count):
        if self.dev is None:
            self.error.emit("not connected"); self.crack_finished.emit(); return
        try:
            self.alog.emit("deauth %s ch=%d ×%d …" % (bssid, ch, count))
            r = self.dev.deauth(bssid, "broadcast", ch, count)
            self.alog.emit("  " + (" ".join(r.lines) or r.error or "sent"))
        except Exception as e:  # noqa: BLE001
            self.error.emit("deauth error: %s" % e)
        finally:
            self.crack_finished.emit()

    def do_capture_hs(self, bssid, ch, ssid):
        if self.dev is None:
            self.error.emit("not connected"); self.crack_finished.emit(); return
        import threading
        self._crack_cancel = threading.Event()
        try:
            os.makedirs(CAP_DIR, exist_ok=True)
            local = os.path.join(CAP_DIR, "hs_%s.pcap" % bssid.replace(":", ""))
            self.alog.emit("capture handshake %s (ch=%d, deauth)…" % (ssid or bssid, ch))
            cap = self.dev.capture_handshake(bssid=bssid, ch=ch, secs=25.0,
                                             deauth_count=24, rounds=4, local_path=local)
            self.alog.emit("  %d frames, %d B -> %s (sha %s)" % (
                cap.get("samples", 0), cap.get("bytes", 0), cap.get("local"),
                "ok" if cap.get("verified") else "?"))
            import wpa_crack as wcm
            t, _ = wcm.select_target(cap["local"], ssid)
            if t:
                self.alog.emit("  ✓ handshake: %s" % t.label())
                wcm.export_hc22000(cap["local"], cap["local"].rsplit(".", 1)[0] + ".hc22000", ssid)
                self._last_pcap = cap["local"]
            else:
                self.alog.emit("  ✗ no EAPOL captured (client idle/far — retry)")
        except Exception as e:  # noqa: BLE001
            self.error.emit("capture error: %s" % e)
        finally:
            self.crack_finished.emit()

    def do_crack_pcap(self, pcap, wordlist, tool, brute):
        """Crack an already-captured local pcap, logging to the Attack tab."""
        import threading
        self._crack_cancel = threading.Event()
        try:
            import crackers as ck, wpa_crack as wcm
            bssid = ck.detect_bssid(pcap)
            hc = pcap.rsplit(".", 1)[0] + ".hc22000"
            try:
                wcm.export_hc22000(pcap, hc)
            except Exception:  # noqa: BLE001
                hc = ""

            def ev(e):
                if e.get("type") == "progress":
                    self.alog.emit("  %s tried @ %.0f/s" % (e.get("tested", "?"), e.get("rate", 0)))
            self.alog.emit("cracking %s with %s (bssid %s)…" %
                           (os.path.basename(pcap), tool, bssid or "?"))
            res = ck.crack_wordlist(pcap, wordlist, bssid=bssid, tool=tool, hc22000=hc,
                                    on_event=ev, cancel=self._crack_cancel)
            if not res["ok"] and brute and not self._crack_cancel.is_set():
                self.alog.emit("wordlist failed; brute 8-digit …")
                res = ck.crack_brute(pcap, "0123456789", 8, bssid=bssid, tool=tool,
                                     hc22000=hc, on_event=ev, cancel=self._crack_cancel)
            if res.get("ok"):
                self.alog.emit("✓ KEY FOUND (%s): %s" % (res.get("tool"), res["key"]))
            elif self._crack_cancel.is_set():
                self.alog.emit("✗ cancelled")
            else:
                self.alog.emit("✗ not found (%s)" % res.get("tool"))
        except Exception as e:  # noqa: BLE001
            self.error.emit("crack error: %s" % e)
        finally:
            self.crack_finished.emit()

    def do_attack_target(self, ssid, bssid, ch, wordlist, tool, brute):
        if self.dev is None:
            self.error.emit("not connected"); self.crack_finished.emit(); return
        import threading
        self._crack_cancel = threading.Event()
        try:
            import wifi_attack
            os.makedirs(CAP_DIR, exist_ok=True)
            out = wifi_attack.run_attack(self.dev, ssid=ssid, bssid=bssid, ch=ch,
                                         wordlist=wordlist, tool=tool, brute=brute,
                                         local_dir=CAP_DIR, log=self.alog.emit,
                                         cancel=self._crack_cancel)
            if out.get("pcap"):
                self._last_pcap = out["pcap"]
            self.alog.emit(wifi_attack.format_result(out))
        except Exception as e:  # noqa: BLE001
            self.error.emit("attack error: %s" % e)
        finally:
            self.crack_finished.emit()

    # ---------- NRF24 tab (scan → select → jam/hijack) ----------
    def do_nrf_scan(self, ms):
        if self.dev is None:
            self.error.emit("not connected"); self.crack_finished.emit(); return
        try:
            self.nlog.emit("nrf scan %.0fms …" % ms)
            devs = self.dev.nrf_scan(int(ms))
            self.nlog.emit("found %d NRF24 devices" % len(devs))
            self.nrf_found.emit(devs)
        except Exception as e:  # noqa: BLE001
            self.error.emit("nrf scan error: %s" % e)
        finally:
            self.crack_finished.emit()

    def do_nrf_jam_ch(self, ch, secs):
        if self.dev is None:
            self.error.emit("not connected"); self.crack_finished.emit(); return
        try:
            self.nlog.emit("carrier jam ch=%d for %ds …" % (ch, secs))
            r = self.dev.nrf_jam_channel(ch, secs)
            self.nlog.emit("  " + (" ".join(r.lines) or r.error or "done"))
        except Exception as e:  # noqa: BLE001
            self.error.emit("nrf jam error: %s" % e)
        finally:
            self.crack_finished.emit()

    def do_nrf_jam_preset(self, name):
        if self.dev is None:
            self.error.emit("not connected"); self.crack_finished.emit(); return
        try:
            from companion_proto import NRF_JAM_PRESETS
            p = NRF_JAM_PRESETS.get(name, {})
            self.nlog.emit("jam preset '%s' %s %s …" % (name, p.get("desc", ""),
                                                        p.get("range", "")))
            r = self.dev.nrf_jam_preset(name)
            self.nlog.emit("  " + (" ".join(r.lines) or r.error or "done"))
        except Exception as e:  # noqa: BLE001
            self.error.emit("nrf preset error: %s" % e)
        finally:
            self.crack_finished.emit()

    def do_nrf_jam_sweep(self, start, stop, step, dwell, noise):
        if self.dev is None:
            self.error.emit("not connected"); self.crack_finished.emit(); return
        try:
            self.nlog.emit("sweep jam %d-%d step%d dwell%dms noise=%d …" %
                           (start, stop, step, dwell, noise))
            r = self.dev.nrf_jam_sweep(start, stop, step, dwell, noise)
            self.nlog.emit("  " + (" ".join(r.lines) or r.error or "done"))
        except Exception as e:  # noqa: BLE001
            self.error.emit("nrf sweep error: %s" % e)
        finally:
            self.crack_finished.emit()

    def do_nrf_readkeys(self, addr, ch, secs):
        if self.dev is None:
            self.error.emit("not connected"); self.crack_finished.emit(); return
        try:
            self.nlog.emit("readkeys %s ch=%d for %ds (decoding HID; cleartext + MS-XOR)…"
                           % (addr, ch, secs))
            res = self.dev.nrf_readkeys(addr, ch, secs)
            for ln in res["lines"]:
                if ln.startswith("[KEY") or ln.startswith("[ENC") or ln.startswith("[NRF"):
                    self.nlog.emit("  " + ln)
            if res["text"]:
                self.nlog.emit("── typed: %r" % res["text"])
            elif not any(l.startswith("[KEY") for l in res["lines"]):
                self.nlog.emit("  (no decodable keystrokes — device idle, encrypted, or wrong ch)")
        except Exception as e:  # noqa: BLE001
            self.error.emit("nrf readkeys error: %s" % e)
        finally:
            self.crack_finished.emit()

    def do_nrf_hijack(self, addr, ch, action, arg, proto):
        if self.dev is None:
            self.error.emit("not connected"); self.crack_finished.emit(); return
        try:
            self.nlog.emit("hijack %s ch=%d action=%s arg=%r proto=%s …" %
                           (addr, ch, action, arg, proto))
            r = self.dev.nrf_hijack(addr, ch, action, arg, proto)
            self.nlog.emit("  " + (" ".join(r.lines) or r.error or "done"))
        except Exception as e:  # noqa: BLE001
            self.error.emit("nrf hijack error: %s" % e)
        finally:
            self.crack_finished.emit()

    def do_audio_tx(self, text, file, freq, dev_khz, rate, osr, reps, voice="en"):
        """TTS (text) or audio file -> analog FM over the CC1101 (sigma-delta →
        2-FSK). Logs to the Audio tab via audlog."""
        if self.dev is None:
            self.error.emit("not connected"); self.crack_finished.emit(); return
        try:
            import audio_tx
            src = file or None
            txt = text or None
            self.audlog.emit("audio tx %g MHz dev=%g kHz reps=%d %s …" %
                             (freq, dev_khz, reps, ("file " + file) if src else repr(txt)))
            res = audio_tx.transmit(self.dev, source=src, text=txt, freq=freq,
                                    dev_khz=dev_khz, rate=int(rate), osr=int(osr),
                                    reps=int(reps), voice=voice,
                                    log=lambda m: self.audlog.emit("  " + m))
            self.audlog.emit("%s: %dB %.1fs on air" %
                           ("done" if res["ok"] else "FAILED", res["bytes"], res["secs"]))
        except Exception as e:  # noqa: BLE001
            self.error.emit("audio tx error: %s" % e)
        finally:
            self.crack_finished.emit()

    def do_capture(self, kind, duration, interval):
        """Capture-to-file on the device (survives a slow/dropped link), then
        fetch + verify + analyze. Emits capture_done(meta, analysis)."""
        if self.dev is None:
            self.error.emit("not connected")
            return
        try:
            self.log.emit("capture %s -> device for %.0fs …" % (kind, duration))
            os.makedirs(CAP_DIR, exist_ok=True)
            local = os.path.join(CAP_DIR, "capture-%s.txt" % time.strftime("%Y%m%d-%H%M%S"))
            iv = int(interval) if interval else None
            cap = self.dev.capture_fetch(kind, duration=duration, local_path=local, interval=iv)
            import companion_compute
            analysis = companion_compute.analyze_stream_file(cap["local"])
            self.capture_done.emit(cap, analysis)
        except Exception as e:  # noqa: BLE001
            self.error.emit("capture error: %s" % e)


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
    sig_capture = Signal(str, float, float)
    sig_crack = Signal(str, str, str, str, bool)  # pcap, wordlist, remote, tool, brute
    sig_attack = Signal(str, str, str, bool)       # ssid, wordlist, tool, brute
    sig_scan = Signal(float)                        # scan duration
    sig_deauth = Signal(str, int, int)              # bssid, ch, count
    sig_cap_hs = Signal(str, int, str)              # bssid, ch, ssid
    sig_attack_t = Signal(str, str, int, str, str, bool)  # ssid,bssid,ch,wordlist,tool,brute
    sig_crack_pcap = Signal(str, str, str, bool)           # pcap, wordlist, tool, brute
    sig_nrf_scan = Signal(float)                            # scan ms
    sig_nrf_jam_ch = Signal(int, int)                       # channel, secs
    sig_nrf_jam_sweep = Signal(int, int, int, int, int)     # start,stop,step,dwell,noise
    sig_nrf_preset = Signal(str)                            # jam preset name
    sig_nrf_hijack = Signal(str, int, str, str, str)        # addr,ch,action,arg,proto
    sig_nrf_readkeys = Signal(str, int, int)                # addr,ch,secs
    sig_audio_tx = Signal(str, str, float, float, int, int, int, str)  # text,file,freq,dev,rate,osr,reps,voice
    sig_list = Signal(str)
    sig_recon = Signal(float)
    sig_heap = Signal()

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
        self.lbl_heap = QLabel(""); self.lbl_heap.setFont(_mono())
        self.lbl_heap.setStyleSheet("color:#27ae60;")
        gsv.addWidget(self.lbl_heap)
        rrow = QHBoxLayout()
        self.btn_refresh = QPushButton("Refresh status")
        self.chk_auto = QCheckBox("auto (2s)")
        rrow.addWidget(self.btn_refresh); rrow.addWidget(self.chk_auto)
        gsv.addLayout(rrow)
        lv.addWidget(gs, 1)
        self._heap_hist = []
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
        self.tabs.addTab(self._tab_attack(), "Attack")
        self.tabs.addTab(self._tab_nrf(), "NRF24")
        self.tabs.addTab(self._tab_audio(), "Audio TX")
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
        srow = QHBoxLayout()
        self.ed_dict_search = QLineEdit()
        self.ed_dict_search.setPlaceholderText("filter dictionaries… (brand / signal / key file)")
        srow.addWidget(QLabel("🔍")); srow.addWidget(self.ed_dict_search, 1)
        v.addLayout(srow)
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

    def _dict_filter(self, text):
        """Hide tree items not matching the query (keep a parent if any child matches)."""
        q = text.strip().lower()
        root = self.dict_tree.invisibleRootItem()
        for i in range(root.childCount()):
            top = root.child(i)
            top_any = False
            for j in range(top.childCount()):
                node = top.child(j)
                if node.childCount():  # brand with signals
                    child_any = False
                    for k in range(node.childCount()):
                        leaf = node.child(k)
                        m = (q in leaf.text(0).lower()) or (q in node.text(0).lower())
                        leaf.setHidden(not m)
                        child_any = child_any or m
                    node.setHidden(not child_any)
                    top_any = top_any or child_any
                else:  # direct leaf (key file / sub / placeholder)
                    m = q in node.text(0).lower() or not q
                    node.setHidden(not m)
                    top_any = top_any or m
            top.setHidden(not top_any and bool(q))

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
        self.btn_capture = QPushButton("Capture→device")
        self.btn_capture.setToolTip(
            "Log sweeps to a file on the device's SD (survives a slow/dropped link),\n"
            "then fetch, verify sha256, and analyze. Best for long unattended captures.")
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
        row.addWidget(self.btn_stream); row.addWidget(self.btn_capture)
        row.addWidget(self.btn_stream_an)
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

    def _tab_attack(self):
        import crackers as _ck
        w = QWidget(); v = QVBoxLayout(w)
        # scan row
        r1 = QHBoxLayout()
        self.btn_scan = QPushButton("⟳ Scan WiFi")
        self.spn_scan = QSpinBox(); self.spn_scan.setRange(3, 30); self.spn_scan.setValue(6)
        self.spn_scan.setSuffix(" s")
        self.lbl_target = QLabel("target: —")
        self.lbl_target.setStyleSheet("font-weight:bold;")
        r1.addWidget(self.btn_scan); r1.addWidget(self.spn_scan)
        r1.addSpacing(12); r1.addWidget(self.lbl_target, 1)
        v.addLayout(r1)
        # AP table
        self.tbl_aps = QTableWidget(0, 5)
        self.tbl_aps.setHorizontalHeaderLabels(["SSID", "BSSID", "ch", "RSSI", "enc"])
        self.tbl_aps.setSelectionBehavior(QAbstractItemView.SelectRows)
        self.tbl_aps.setSelectionMode(QAbstractItemView.SingleSelection)
        self.tbl_aps.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.tbl_aps.verticalHeader().setVisible(False)
        hh = self.tbl_aps.horizontalHeader()
        hh.setSectionResizeMode(0, QHeaderView.Stretch)
        for c in (1, 2, 3, 4):
            hh.setSectionResizeMode(c, QHeaderView.ResizeToContents)
        v.addWidget(self.tbl_aps, 1)
        # tool + wordlist row
        r2 = QHBoxLayout()
        self.cbo_atk_tool = QComboBox(); self.cbo_atk_tool.addItems(_ck.available_tools())
        self.cbo_atk_wl = QComboBox()
        for label, path, _sz in _ck.list_wordlists():
            self.cbo_atk_wl.addItem(label, path)
        if self.cbo_atk_wl.count() == 0:
            self.cbo_atk_wl.addItem("(none)", "")
        self.chk_atk_brute = QCheckBox("+brute 8d")
        r2.addWidget(QLabel("tool")); r2.addWidget(self.cbo_atk_tool)
        r2.addWidget(QLabel("wordlist")); r2.addWidget(self.cbo_atk_wl, 1)
        r2.addWidget(self.chk_atk_brute)
        v.addLayout(r2)
        # action row
        r3 = QHBoxLayout()
        self.btn_atk_deauth = QPushButton("Deauth")
        self.btn_atk_capture = QPushButton("Capture handshake")
        self.btn_atk_full = QPushButton("⚡ Full attack")
        self.btn_atk_crackpcap = QPushButton("Crack last pcap")
        self.btn_atk_stop = QPushButton("Stop"); self.btn_atk_stop.setEnabled(False)
        for b in (self.btn_atk_deauth, self.btn_atk_capture, self.btn_atk_full,
                  self.btn_atk_crackpcap, self.btn_atk_stop):
            r3.addWidget(b)
        r3.addStretch(1)
        v.addLayout(r3)
        # log
        self.txt_attack = QPlainTextEdit(readOnly=True); self.txt_attack.setFont(_mono())
        self.txt_attack.setPlaceholderText("Scan → pick a network → Deauth / Capture / Full attack. "
                                           "Authorized testing of your own networks only.")
        v.addWidget(self.txt_attack, 1)
        self._last_pcap = ""
        self._aps = []
        return w

    def _selected_ap(self):
        row = self.tbl_aps.currentRow()
        if row < 0 or row >= len(self._aps):
            self._alog("select a network in the table first")
            return None
        return self._aps[row]

    def _do_scan(self):
        self.tabs.setCurrentWidget(self.txt_attack.parentWidget())
        self.txt_attack.appendPlainText("scanning …")
        self._crack_busy(True)
        self.sig_scan.emit(float(self.spn_scan.value()))

    @Slot(list)
    def _on_scan_found(self, aps):
        self._aps = aps
        self.tbl_aps.setRowCount(len(aps))
        for i, ap in enumerate(aps):
            cells = [ap["ssid"] or "<hidden>", ap["bssid"], str(ap["ch"]),
                     str(ap["rssi"]), ap["enc"]]
            for c, txt in enumerate(cells):
                self.tbl_aps.setItem(i, c, QTableWidgetItem(txt))
        if aps:
            self.tbl_aps.selectRow(0)
            self._on_ap_selected()

    def _on_ap_selected(self):
        ap = self._selected_ap()
        if ap:
            self.lbl_target.setText("target: %s [%s] ch%d %ddBm %s" % (
                ap["ssid"] or "<hidden>", ap["bssid"], ap["ch"], ap["rssi"], ap["enc"]))

    def _alog(self, msg):
        self.txt_attack.appendPlainText(str(msg))

    def _atk_deauth(self):
        ap = self._selected_ap()
        if not ap:
            return
        self._crack_busy(True)
        self.sig_deauth.emit(ap["bssid"], ap["ch"], 24)

    def _atk_capture(self):
        ap = self._selected_ap()
        if not ap:
            return
        self._crack_busy(True)
        self.sig_cap_hs.emit(ap["bssid"], ap["ch"], ap["ssid"])

    def _atk_full(self):
        ap = self._selected_ap()
        if not ap:
            return
        self._crack_busy(True)
        self.sig_attack_t.emit(ap["ssid"], ap["bssid"], ap["ch"],
                               self.cbo_atk_wl.currentData() or "",
                               self.cbo_atk_tool.currentText(), self.chk_atk_brute.isChecked())

    def _atk_crack_pcap(self):
        if not self._last_pcap or not os.path.isfile(self._last_pcap):
            self._alog("no captured pcap yet — Capture handshake first")
            return
        self._crack_busy(True)
        self.sig_crack_pcap.emit(self._last_pcap, self.cbo_atk_wl.currentData() or "",
                                 self.cbo_atk_tool.currentText(), self.chk_atk_brute.isChecked())

    def _tab_nrf(self):
        w = QWidget(); v = QVBoxLayout(w)
        r1 = QHBoxLayout()
        self.btn_nrf_scan = QPushButton("⟳ Scan NRF24")
        self.spn_nrf_ms = QSpinBox(); self.spn_nrf_ms.setRange(1000, 20000)
        self.spn_nrf_ms.setValue(4000); self.spn_nrf_ms.setSingleStep(1000)
        self.spn_nrf_ms.setSuffix(" ms")
        self.lbl_nrf_target = QLabel("target: —"); self.lbl_nrf_target.setStyleSheet("font-weight:bold;")
        r1.addWidget(self.btn_nrf_scan); r1.addWidget(self.spn_nrf_ms)
        r1.addSpacing(12); r1.addWidget(self.lbl_nrf_target, 1)
        v.addLayout(r1)
        self.tbl_nrf = QTableWidget(0, 3)
        self.tbl_nrf.setHorizontalHeaderLabels(["ch", "address", "hits"])
        self.tbl_nrf.setSelectionBehavior(QAbstractItemView.SelectRows)
        self.tbl_nrf.setSelectionMode(QAbstractItemView.SingleSelection)
        self.tbl_nrf.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.tbl_nrf.verticalHeader().setVisible(False)
        hh = self.tbl_nrf.horizontalHeader()
        hh.setSectionResizeMode(1, QHeaderView.Stretch)
        hh.setSectionResizeMode(0, QHeaderView.ResizeToContents)
        hh.setSectionResizeMode(2, QHeaderView.ResizeToContents)
        v.addWidget(self.tbl_nrf, 1)
        # channel + hijack action row (channel auto-fills from the selected device
        # but is editable, so you can jam/inject on an arbitrary channel too)
        r2 = QHBoxLayout()
        self.spn_nrf_ch = QSpinBox(); self.spn_nrf_ch.setRange(0, 125); self.spn_nrf_ch.setValue(50)
        self.spn_nrf_ch.setPrefix("ch ")
        self.cbo_nrf_action = QComboBox()
        self.cbo_nrf_action.addItems(["calc", "cmd", "type", "run", "jam"])
        self.cbo_nrf_proto = QComboBox(); self.cbo_nrf_proto.addItems(["logi", "hid"])
        self.ed_nrf_arg = QLineEdit(); self.ed_nrf_arg.setPlaceholderText("arg (text for type/run, secs for jam)")
        r2.addWidget(QLabel("channel")); r2.addWidget(self.spn_nrf_ch)
        r2.addWidget(QLabel("action")); r2.addWidget(self.cbo_nrf_action)
        r2.addWidget(QLabel("proto")); r2.addWidget(self.cbo_nrf_proto)
        r2.addWidget(self.ed_nrf_arg, 1)
        v.addLayout(r2)
        # jam preset row
        from companion_proto import NRF_JAM_PRESETS
        rp = QHBoxLayout()
        self.cbo_nrf_preset = QComboBox()
        for nm, p in NRF_JAM_PRESETS.items():
            self.cbo_nrf_preset.addItem("%s — %s" % (nm, p["desc"]), nm)
        self.btn_nrf_preset = QPushButton("Jam preset")
        rp.addWidget(QLabel("jam preset")); rp.addWidget(self.cbo_nrf_preset, 1)
        rp.addWidget(self.btn_nrf_preset)
        v.addLayout(rp)
        # action buttons
        r3 = QHBoxLayout()
        self.btn_nrf_jamch = QPushButton("Jam channel 3s")
        self.btn_nrf_hijack = QPushButton("⚡ Hijack")
        self.btn_nrf_readkeys = QPushButton("⌨ Read keys")
        self.btn_nrf_readkeys.setToolTip("Sniff + decode HID keystrokes from the selected device "
                                         "(cleartext + Microsoft XOR; encrypted flagged).")
        self.spn_nrf_secs = QSpinBox(); self.spn_nrf_secs.setRange(3, 120); self.spn_nrf_secs.setValue(15)
        self.spn_nrf_secs.setSuffix(" s")
        self.btn_nrf_stop = QPushButton("Stop"); self.btn_nrf_stop.setEnabled(False)
        for b in (self.btn_nrf_jamch, self.btn_nrf_hijack, self.btn_nrf_readkeys):
            r3.addWidget(b)
        r3.addWidget(self.spn_nrf_secs); r3.addWidget(self.btn_nrf_stop)
        r3.addStretch(1)
        v.addLayout(r3)
        self.txt_nrf = QPlainTextEdit(readOnly=True); self.txt_nrf.setFont(_mono())
        self.txt_nrf.setPlaceholderText("Scan NRF24 → pick a device → Jam / Hijack. "
                                        "Authorized testing of your own devices only.")
        v.addWidget(self.txt_nrf, 1)
        self._nrf_devs = []
        return w

    def _nlog(self, msg):
        self.txt_nrf.appendPlainText(str(msg))

    def _audlog(self, msg):
        self.txt_audio.appendPlainText(str(msg))

    def _selected_nrf(self):
        row = self.tbl_nrf.currentRow()
        if row < 0 or row >= len(self._nrf_devs):
            self._nlog("select a device in the table first")
            return None
        return self._nrf_devs[row]

    def _do_nrf_scan(self):
        self.tabs.setCurrentWidget(self.txt_nrf.parentWidget())
        self.txt_nrf.appendPlainText("scanning NRF24 …")
        self._crack_busy(True)
        self.sig_nrf_scan.emit(float(self.spn_nrf_ms.value()))

    @Slot(list)
    def _on_nrf_found(self, devs):
        self._nrf_devs = devs
        self.tbl_nrf.setRowCount(len(devs))
        for i, d in enumerate(devs):
            for c, txt in enumerate([str(d["ch"]), d["addr"], str(d["hits"])]):
                self.tbl_nrf.setItem(i, c, QTableWidgetItem(txt))
        if devs:
            self.tbl_nrf.selectRow(0)
            self._on_nrf_selected()

    def _on_nrf_selected(self):
        d = self._selected_nrf()
        if d:
            self.spn_nrf_ch.setValue(d["ch"])   # auto-fill the channel field
            self.lbl_nrf_target.setText("target: %s  ch%d  hits=%d" % (d["addr"], d["ch"], d["hits"]))

    def _nrf_jam_ch(self):
        # jams the channel field (no device selection required)
        self._crack_busy(True)
        self.sig_nrf_jam_ch.emit(self.spn_nrf_ch.value(), 3)

    def _nrf_preset(self):
        self.tabs.setCurrentWidget(self.txt_nrf.parentWidget())
        self._crack_busy(True)
        self.sig_nrf_preset.emit(self.cbo_nrf_preset.currentData())

    def _nrf_hijack(self):
        d = self._selected_nrf()
        if not d:
            return
        self._crack_busy(True)
        self.sig_nrf_hijack.emit(d["addr"], self.spn_nrf_ch.value(),
                                 self.cbo_nrf_action.currentText(),
                                 self.ed_nrf_arg.text().strip(), self.cbo_nrf_proto.currentText())

    def _nrf_readkeys(self):
        d = self._selected_nrf()
        if not d:
            return
        self._crack_busy(True)
        self.sig_nrf_readkeys.emit(d["addr"], self.spn_nrf_ch.value(), self.spn_nrf_secs.value())

    # ---- Audio TX tab: TTS / audio broadcast as analog FM over CC1101 ---------
    def _tab_audio(self):
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.addWidget(QLabel(
            "<b>Analog FM voice/audio over CC1101</b> — for analog radios / "
            "walkie-talkies.<br>TTS or an audio file is oversampled to a "
            "sigma-delta bitstream and keyed as 2-FSK FM."))

        # radio params row
        params = QHBoxLayout()
        self.spn_audio_freq = QDoubleSpinBox()
        self.spn_audio_freq.setRange(280.0, 928.0)
        self.spn_audio_freq.setDecimals(3)
        self.spn_audio_freq.setValue(433.0)
        self.spn_audio_freq.setSuffix(" MHz")
        self.spn_audio_dev = QDoubleSpinBox()
        self.spn_audio_dev.setRange(0.5, 100.0)
        self.spn_audio_dev.setDecimals(1)
        self.spn_audio_dev.setValue(2.5)
        self.spn_audio_dev.setSuffix(" kHz dev")
        self.spn_audio_reps = QSpinBox()
        self.spn_audio_reps.setRange(1, 20)
        self.spn_audio_reps.setValue(1)
        self.spn_audio_reps.setPrefix("×")
        self.spn_audio_osr = QSpinBox()
        self.spn_audio_osr.setRange(4, 64)
        self.spn_audio_osr.setValue(16)
        self.spn_audio_osr.setPrefix("osr ")
        self.spn_audio_rate = QSpinBox()
        self.spn_audio_rate.setRange(4000, 22050)
        self.spn_audio_rate.setSingleStep(1000)
        self.spn_audio_rate.setValue(8000)
        self.spn_audio_rate.setSuffix(" Hz")
        for lbl, wdg in (("Freq", self.spn_audio_freq), ("Dev", self.spn_audio_dev),
                         ("Reps", self.spn_audio_reps), ("OSR", self.spn_audio_osr),
                         ("Rate", self.spn_audio_rate)):
            params.addWidget(QLabel(lbl))
            params.addWidget(wdg)
        params.addStretch(1)
        lay.addLayout(params)

        # TTS row
        tts = QHBoxLayout()
        self.ed_audio_text = QLineEdit()
        self.ed_audio_text.setPlaceholderText("text to speak, e.g. break break, radio check")
        self.cbo_audio_voice = QComboBox()
        self.cbo_audio_voice.setEditable(True)
        self.cbo_audio_voice.addItems(["en", "ru", "en+f3", "en+m3", "de", "fr", "es"])
        self.btn_audio_say = QPushButton("🔊 Speak")
        self.btn_audio_say.clicked.connect(self._audio_say)
        tts.addWidget(QLabel("TTS"))
        tts.addWidget(self.ed_audio_text, 1)
        tts.addWidget(QLabel("voice"))
        tts.addWidget(self.cbo_audio_voice)
        tts.addWidget(self.btn_audio_say)
        lay.addLayout(tts)

        # audio file row
        fl = QHBoxLayout()
        self.ed_audio_file = QLineEdit()
        self.ed_audio_file.setPlaceholderText("audio file (wav/mp3/ogg/…) to broadcast")
        self.btn_audio_browse = QPushButton("Browse…")
        self.btn_audio_browse.clicked.connect(self._audio_browse)
        self.btn_audio_file = QPushButton("📡 Transmit file")
        self.btn_audio_file.clicked.connect(self._audio_file)
        fl.addWidget(QLabel("File"))
        fl.addWidget(self.ed_audio_file, 1)
        fl.addWidget(self.btn_audio_browse)
        fl.addWidget(self.btn_audio_file)
        lay.addLayout(fl)

        self.txt_audio = QPlainTextEdit(readOnly=True)
        lay.addWidget(self.txt_audio, 1)
        lay.addWidget(QLabel(
            "<i>Transmit only on frequencies you are permitted to use, to your own "
            "radios. RF transmission is regulated.</i>"))
        return w

    def _audio_browse(self):
        path, _ = QFileDialog.getOpenFileName(
            self, "Audio file", "",
            "Audio (*.wav *.mp3 *.ogg *.flac *.m4a *.aac);;All files (*)")
        if path:
            self.ed_audio_file.setText(path)

    def _audio_say(self):
        text = self.ed_audio_text.text().strip()
        if not text:
            self._audlog("type some text to speak first")
            return
        self.tabs.setCurrentWidget(self.txt_audio.parentWidget())
        self._crack_busy(True)
        self.sig_audio_tx.emit(text, "", self.spn_audio_freq.value(),
                               self.spn_audio_dev.value(), self.spn_audio_rate.value(),
                               self.spn_audio_osr.value(), self.spn_audio_reps.value(),
                               self.cbo_audio_voice.currentText().strip() or "en")

    def _audio_file(self):
        path = self.ed_audio_file.text().strip()
        if not path:
            self._audlog("choose an audio file first")
            return
        self.tabs.setCurrentWidget(self.txt_audio.parentWidget())
        self._crack_busy(True)
        self.sig_audio_tx.emit("", path, self.spn_audio_freq.value(),
                               self.spn_audio_dev.value(), self.spn_audio_rate.value(),
                               self.spn_audio_osr.value(), self.spn_audio_reps.value(), "en")

    def _tab_analyze(self):
        w = QWidget(); v = QVBoxLayout(w)
        row = QHBoxLayout()
        self.ed_an_remote = QLineEdit("/nrf_scan.log")
        self.btn_analyze = QPushButton("Fetch + analyze")
        self.btn_recon = QPushButton("Recon scan")
        self.spn_recon = QSpinBox(); self.spn_recon.setRange(2, 30); self.spn_recon.setValue(6)
        self.spn_recon.setSuffix(" s")
        self.btn_report_save = QPushButton("Save report")
        row.addWidget(QLabel("remote")); row.addWidget(self.ed_an_remote, 1)
        row.addWidget(self.btn_analyze)
        row.addWidget(self.btn_recon); row.addWidget(self.spn_recon)
        row.addWidget(self.btn_report_save)
        v.addLayout(row)
        # WPA cracking row — real tools (aircrack-ng/hashcat) + discovered wordlists
        import crackers as _ck
        row2 = QHBoxLayout()
        self.cbo_tool = QComboBox(); self.cbo_tool.addItems(_ck.available_tools())
        self.cbo_wordlist = QComboBox()
        self._reload_wordlists()
        self.chk_brute = QCheckBox("then brute 8d")
        self.chk_brute.setToolTip("If the wordlist fails, brute all 8-digit numeric "
                                  "passwords (crunch | aircrack-ng).")
        row2.addWidget(QLabel("tool")); row2.addWidget(self.cbo_tool)
        row2.addWidget(QLabel("wordlist")); row2.addWidget(self.cbo_wordlist, 1)
        row2.addWidget(self.chk_brute)
        v.addLayout(row2)
        row3 = QHBoxLayout()
        self.btn_crack_local = QPushButton("Crack local pcap…")
        self.btn_crack_dev = QPushButton("Crack device handshake…")
        self.btn_attack = QPushButton("WPA attack (full cycle)…")
        self.btn_attack.setToolTip("Find AP → deauth → capture handshake → crack.\n"
                                   "Authorized testing of your own network only.")
        self.btn_crack_cancel = QPushButton("Stop"); self.btn_crack_cancel.setEnabled(False)
        self.btn_wordlist = QPushButton("Browse…")
        for b in (self.btn_crack_local, self.btn_crack_dev, self.btn_attack,
                  self.btn_crack_cancel, self.btn_wordlist):
            row3.addWidget(b)
        row3.addStretch(1)
        v.addLayout(row3)
        self.txt_report = QPlainTextEdit(readOnly=True); self.txt_report.setFont(_mono())
        v.addWidget(self.txt_report, 1)
        return w

    def _reload_wordlists(self):
        import crackers as _ck
        self.cbo_wordlist.clear()
        self._wordlists = _ck.list_wordlists()
        for label, path, _sz in self._wordlists:
            self.cbo_wordlist.addItem(label, path)
        if not self._wordlists:
            self.cbo_wordlist.addItem("(none found — Browse…)", "")

    def _cur_wordlist(self):
        return self.cbo_wordlist.currentData() or ""

    def _cur_tool(self):
        return self.cbo_tool.currentText()

    def _pick_wordlist(self):
        path, _ = QFileDialog.getOpenFileName(self, "Wordlist", WORDLIST_DIR,
                                              "Wordlists (*.txt *.lst *.dic);;All (*)")
        if path:
            self.cbo_wordlist.insertItem(0, "%s (%s)" % (os.path.basename(path),
                                          _human_size(os.path.getsize(path))), path)
            self.cbo_wordlist.setCurrentIndex(0)

    def _crack_local(self):
        path, _ = QFileDialog.getOpenFileName(self, "Handshake pcap", CAP_DIR,
                                              "pcap (*.pcap *.cap);;All (*)")
        if not path:
            return
        self.tabs.setCurrentWidget(self.txt_report.parentWidget())
        self.txt_report.setPlainText("cracking %s with %s …" %
                                     (os.path.basename(path), self._cur_tool()))
        self._crack_busy(True)
        self.sig_crack.emit(path, self._cur_wordlist(), "", self._cur_tool(),
                            self.chk_brute.isChecked())

    def _crack_device(self):
        from PySide6.QtWidgets import QInputDialog
        remote, ok = QInputDialog.getText(self, "Device handshake",
                                          "Device pcap path:", text="/BrucePCAP/handshakes/")
        if not ok or not remote.strip():
            return
        self.tabs.setCurrentWidget(self.txt_report.parentWidget())
        self.txt_report.setPlainText("fetching + cracking %s …" % remote)
        self._crack_busy(True)
        self.sig_crack.emit("", self._cur_wordlist(), remote.strip(), self._cur_tool(),
                            self.chk_brute.isChecked())

    def _do_attack(self):
        from PySide6.QtWidgets import QInputDialog
        ssid, ok = QInputDialog.getText(self, "WPA attack",
                                        "Target SSID (authorized testing of your own network only):")
        if not ok or not ssid.strip():
            return
        self.tabs.setCurrentWidget(self.txt_report.parentWidget())
        self.txt_report.setPlainText("attacking %s …" % ssid)
        self._crack_busy(True)
        self.sig_attack.emit(ssid.strip(), self._cur_wordlist(), self._cur_tool(),
                             self.chk_brute.isChecked())

    def _crack_busy(self, busy):
        for b in (self.btn_crack_cancel, self.btn_atk_stop, self.btn_nrf_stop):
            b.setEnabled(busy)
        for b in (self.btn_crack_local, self.btn_crack_dev, self.btn_attack,
                  self.btn_scan, self.btn_atk_deauth, self.btn_atk_capture,
                  self.btn_atk_full, self.btn_atk_crackpcap,
                  self.btn_nrf_scan, self.btn_nrf_jamch, self.btn_nrf_preset, self.btn_nrf_hijack,
                  self.btn_nrf_readkeys, self.btn_audio_say, self.btn_audio_file):
            b.setEnabled(not busy)

    def _cancel_crack(self):
        if hasattr(self.worker, "cancel_crack"):
            self.worker.cancel_crack()
        self.txt_report.appendPlainText("\n(cancelling…)")

    def _do_recon(self):
        self.txt_report.setPlainText("scanning wifi/nrf/rf … (this takes ~%ds)" %
                                     int(self.spn_recon.value() * 2.4))
        self.tabs.setCurrentWidget(self.txt_report.parentWidget())
        self.sig_recon.emit(float(self.spn_recon.value()))

    def _save_report(self):
        text = self.txt_report.toPlainText().strip()
        if not text:
            return
        try:
            import time
            os.makedirs(CAP_DIR, exist_ok=True)
            path = os.path.join(CAP_DIR, "report-%s.md" % time.strftime("%Y%m%d-%H%M%S"))
            with open(path, "w") as fh:
                fh.write(text + "\n")
            self.statusBar().showMessage("saved " + path, 5000)
            self.txt_report.appendPlainText("\n[saved -> %s]" % path)
        except Exception as e:  # noqa: BLE001
            self.statusBar().showMessage("save error: %s" % e, 5000)

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
        self.worker.capture_done.connect(self._on_capture_done)
        self.worker.crack_finished.connect(self._on_crack_finished)
        self.worker.scan_found.connect(self._on_scan_found)
        self.worker.alog.connect(self._alog)
        self.worker.nrf_found.connect(self._on_nrf_found)
        self.worker.nlog.connect(self._nlog)
        self.worker.audlog.connect(self._audlog)
        self.worker.listing.connect(self._on_listing)
        self.worker.heap.connect(self._on_heap)
        self.worker.error.connect(self._on_error)

        self.sig_connect.connect(self.worker.do_connect)
        self.sig_disconnect.connect(self.worker.do_disconnect)
        self.sig_request.connect(self.worker.do_request)
        self.sig_status.connect(self.worker.do_status)
        self.sig_file_get.connect(self.worker.do_file_get)
        self.sig_file_put.connect(self.worker.do_file_put)
        self.sig_analyze.connect(self.worker.do_analyze)
        self.sig_stream.connect(self.worker.do_stream)
        self.sig_capture.connect(self.worker.do_capture)
        self.sig_crack.connect(self.worker.do_crack)
        self.sig_attack.connect(self.worker.do_attack)
        self.sig_scan.connect(self.worker.do_scan)
        self.sig_deauth.connect(self.worker.do_deauth)
        self.sig_cap_hs.connect(self.worker.do_capture_hs)
        self.sig_attack_t.connect(self.worker.do_attack_target)
        self.sig_crack_pcap.connect(self.worker.do_crack_pcap)
        self.sig_nrf_scan.connect(self.worker.do_nrf_scan)
        self.sig_nrf_jam_ch.connect(self.worker.do_nrf_jam_ch)
        self.sig_nrf_jam_sweep.connect(self.worker.do_nrf_jam_sweep)
        self.sig_nrf_preset.connect(self.worker.do_nrf_jam_preset)
        self.sig_nrf_hijack.connect(self.worker.do_nrf_hijack)
        self.sig_nrf_readkeys.connect(self.worker.do_nrf_readkeys)
        self.sig_audio_tx.connect(self.worker.do_audio_tx)
        self.sig_list.connect(self.worker.do_list)
        self.sig_recon.connect(self.worker.do_recon)
        self.sig_heap.connect(self.worker.do_heap)

    def _wire(self):
        self.btn_connect.clicked.connect(self._toggle_connect)
        self.ed_cmd.returnPressed.connect(self._send_cmd)
        self.btn_send.clicked.connect(self._send_cmd)
        self.btn_refresh.clicked.connect(lambda: self.sig_status.emit())
        self.btn_get.clicked.connect(self._do_get)
        self.btn_browse.clicked.connect(self._browse)
        self.btn_put.clicked.connect(self._do_put)
        self.btn_stream.clicked.connect(self._do_stream)
        self.btn_capture.clicked.connect(self._do_capture)
        self.btn_stream_an.clicked.connect(self._analyze_last_stream)
        self.btn_crack_local.clicked.connect(self._crack_local)
        self.btn_crack_dev.clicked.connect(self._crack_device)
        self.btn_attack.clicked.connect(self._do_attack)
        self.btn_crack_cancel.clicked.connect(self._cancel_crack)
        self.btn_wordlist.clicked.connect(self._pick_wordlist)
        # Attack tab
        self.btn_scan.clicked.connect(self._do_scan)
        self.tbl_aps.itemSelectionChanged.connect(self._on_ap_selected)
        self.btn_atk_deauth.clicked.connect(self._atk_deauth)
        self.btn_atk_capture.clicked.connect(self._atk_capture)
        self.btn_atk_full.clicked.connect(self._atk_full)
        self.btn_atk_crackpcap.clicked.connect(self._atk_crack_pcap)
        self.btn_atk_stop.clicked.connect(self._cancel_crack)
        # NRF24 tab
        self.btn_nrf_scan.clicked.connect(self._do_nrf_scan)
        self.tbl_nrf.itemSelectionChanged.connect(self._on_nrf_selected)
        self.btn_nrf_jamch.clicked.connect(self._nrf_jam_ch)
        self.btn_nrf_preset.clicked.connect(self._nrf_preset)
        self.btn_nrf_hijack.clicked.connect(self._nrf_hijack)
        self.btn_nrf_readkeys.clicked.connect(self._nrf_readkeys)
        self.btn_nrf_stop.clicked.connect(self._cancel_crack)
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
        self.ed_dict_search.textChanged.connect(self._dict_filter)

    def _toggle_auto(self, on):
        if on and self._connected:
            self._auto_timer.start()
        else:
            self._auto_timer.stop()

    def _auto_tick(self):
        if self._connected:
            self.sig_status.emit()
            self.sig_heap.emit()

    @Slot(int)
    def _on_heap(self, val):
        self._heap_hist.append(val)
        self._heap_hist = self._heap_hist[-60:]
        try:
            import companion_compute
            spark = companion_compute._sparkline(self._heap_hist, width=len(self._heap_hist))
        except Exception:
            spark = ""
        self.lbl_heap.setText("free heap %s B  %s" % (f"{val:,}", spark))
        self.btn_analyze.clicked.connect(
            lambda: self.sig_analyze.emit(self.ed_an_remote.text().strip()))
        self.btn_recon.clicked.connect(self._do_recon)
        self.btn_report_save.clicked.connect(self._save_report)

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

    def _do_capture(self):
        self.txt_stream.clear()
        self.btn_capture.setEnabled(False)
        self.btn_stream.setEnabled(False)
        kind = self.cbo_kind.currentText()
        if kind == "rf":
            a = self.ed_rf0.text().strip() or "433.0"
            b = self.ed_rf1.text().strip() or "434.8"
            kind = f"rf {a} {b}"
        self.txt_stream.appendPlainText(
            "capturing %s to the device for %ds — this survives a dropped link…"
            % (kind, self.spn_dur.value()))
        self.sig_capture.emit(kind, float(self.spn_dur.value()), 0.0)

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
        self.sig_heap.emit()
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

    @Slot()
    def _on_crack_finished(self):
        self._crack_busy(False)

    @Slot(dict, str)
    def _on_capture_done(self, cap, analysis):
        self.btn_capture.setEnabled(True)
        self.btn_stream.setEnabled(True)
        vr = "✓ sha256 verified" if cap.get("verified") else "⚠ sha256 UNVERIFIED"
        self.txt_stream.appendPlainText(
            "captured %s: %d samples, %d B  [%s]\n  device: %s\n  local:  %s\n"
            % (cap.get("kind", "?"), cap.get("samples", 0), cap.get("bytes", 0), vr,
               cap.get("path", "?"), cap.get("local", "?")))
        self.txt_stream.appendPlainText("──── analysis ────\n" + (analysis or "(empty)"))

    @Slot(str)
    def _on_error(self, msg):
        self.btn_connect.setEnabled(True)
        self.btn_stream.setEnabled(True)
        if hasattr(self, "btn_capture"):
            self.btn_capture.setEnabled(True)
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
                    self.btn_put, self.btn_browse, self.btn_stream, self.btn_capture,
                    self.btn_analyze,
                    self.btn_fb_up, self.btn_fb_refresh, self.btn_fb_dl,
                    self.btn_fb_view, self.btn_fb_del,
                    self.btn_dict_send, self.btn_dict_deploy, self.btn_dict_tx,
                    self.btn_recon):
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


def _human_size(n):
    for u in ("B", "K", "M", "G"):
        if n < 1024:
            return f"{n:.0f}{u}"
        n /= 1024
    return f"{n:.0f}T"


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
