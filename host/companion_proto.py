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
        r = self.request(f"HELLO proto=1 token={token}", timeout=timeout)
        info = {"ok": r.ok and r.code == 0, "raw": r}
        for line in r.lines:
            for tok in line.split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    info[k] = v
        if "caps" in info:
            info["caps"] = info["caps"].split(",")
        return info

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
