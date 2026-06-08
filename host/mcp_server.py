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
