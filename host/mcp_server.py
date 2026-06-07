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

_lock = threading.Lock()
_dev: Companion | None = None
_info: dict = {}
_port = DEFAULT_PORT


def _ensure(port: str | None = None, token: str | None = None) -> dict:
    """Connect (if needed) and run HELLO. Returns the HELLO info dict."""
    global _dev, _info, _port
    if port:
        _port = port
    tok = DEFAULT_TOKEN if token is None else token
    if _dev is None:
        _dev = Companion(_port)
        _info = _dev.hello(tok)
        if not _info.get("ok"):
            # keep the connection but surface the auth/handshake problem
            raise RuntimeError(f"HELLO failed: {_info.get('raw').error if _info.get('raw') else _info}")
    return _info


def _fmt(resp) -> str:
    body = "\n".join(resp.lines) if resp.lines else "(no output)"
    if resp.ok:
        return f"{body}\n[END code={resp.code}]"
    return f"ERROR code={resp.code}: {resp.error}\n{body}"


@mcp.tool()
def device_connect(port: str = DEFAULT_PORT, token: str = "") -> str:
    """Open the transport to the device and perform the HELLO handshake.

    port: serial device (default /dev/ttyACM1 = the ESP32-S3 target).
    token: companion auth token (empty = open/lab mode).
    """
    global _dev, _info
    with _lock:
        try:
            if _dev is not None:
                _dev.close()
                _dev = None
            info = _ensure(port, token)
        except Exception as e:  # noqa: BLE001
            return f"connect failed: {e}"
    caps = ",".join(info.get("caps", []))
    return (f"connected port={port} fw={info.get('fw')} board={info.get('board')} "
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
