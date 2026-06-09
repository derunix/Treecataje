#!/usr/bin/env python3
"""Wrappers around real password-cracking tools (aircrack-ng / hashcat) plus
wordlist discovery. The GUI/TUI/orchestrator use this so cracking runs on the
fast, battle-tested crackers when available, falling back to the pure-Python
wpa_crack only if none are installed.

A "cracker run" streams progress events to an on_event(dict) callback and can be
cancelled via a threading.Event. It returns dict(ok, key, tested, tool, error?).
"""
import os
import re
import shutil
import subprocess
import threading

import wpa_crack as wc

_HERE = os.path.dirname(os.path.abspath(__file__))
WORDLIST_DIRS = [
    os.path.join(_HERE, "captures"),            # extracted rockyou.txt etc.
    os.path.join(_HERE, "dictionaries", "wordlists"),
    "/usr/share/wordlists",
    os.path.expanduser("~/wordlists"),
]

_ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
_KEY_RE = re.compile(r"KEY FOUND!\s*\[\s*(.*?)\s*\]")
_HC_KEY_RE = re.compile(r"^[0-9a-f*]+:(.+)$")            # hashcat hash:plain (potfile/--show)
_PROG_RE = re.compile(r"\]\s*([\d]+)\s*keys tested.*?\(([\d.]+)\s*k/s\)")
_PASS_RE = re.compile(r"Current passphrase:\s*(.+?)\s*$")


# ── discovery ──────────────────────────────────────────────────────────────────
def have(tool):
    return shutil.which(tool) is not None


def available_tools():
    """Ordered by preference for WPA on this box (aircrack is the reliable CPU
    cracker; hashcat needs a working OpenCL backend; python is the fallback)."""
    tools = []
    if have("aircrack-ng"):
        tools.append("aircrack-ng")
    if have("hashcat"):
        tools.append("hashcat")
    tools.append("python")
    return tools


def list_wordlists():
    """Return [(label, path, size_bytes)] of candidate wordlists found on disk."""
    out, seen = [], set()
    for d in WORDLIST_DIRS:
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            p = os.path.realpath(os.path.join(d, name))
            if p in seen or not os.path.isfile(p):
                continue
            low = name.lower()
            if low.endswith((".txt", ".lst", ".dic")) or "rockyou" in low or "wordlist" in low:
                try:
                    sz = os.path.getsize(p)
                except OSError:
                    continue
                seen.add(p)
                out.append((f"{name} ({_human(sz)})", p, sz))
    return out


def _human(n):
    for u in ("B", "K", "M", "G"):
        if n < 1024:
            return f"{n:.0f}{u}"
        n /= 1024
    return f"{n:.0f}T"


def detect_bssid(pcap):
    """Pick the BSSID of the best crackable handshake in a pcap (for aircrack -b)."""
    try:
        target, _ = wc.select_target(pcap)
        if target and target.ap:
            return target.ap.hex(":")
    except Exception:  # noqa: BLE001
        pass
    return ""


# ── line streaming (aircrack repaints with \r + ANSI) ──────────────────────────
def _stream_lines(proc, cancel):
    buf = ""
    while True:
        if cancel is not None and cancel.is_set():
            proc.terminate()
            break
        chunk = proc.stdout.read(128)
        if not chunk:
            break
        buf += chunk
        parts = re.split(r"[\r\n]", buf)
        buf = parts.pop()
        for ln in parts:
            ln = _ANSI.sub("", ln).strip()
            if ln:
                yield ln
    if buf:
        yield _ANSI.sub("", buf).strip()


# ── aircrack-ng ────────────────────────────────────────────────────────────────
def aircrack_wordlist(pcap, wordlist, bssid="", on_event=None, cancel=None):
    if not bssid:
        bssid = detect_bssid(pcap)
    cmd = ["aircrack-ng", "-w", wordlist]
    if bssid:
        cmd += ["-b", bssid]
    cmd += [pcap]
    return _run_aircrack(cmd, "aircrack-ng:wordlist", on_event, cancel)


def aircrack_brute(pcap, charset="0123456789", length=8, bssid="", on_event=None, cancel=None):
    """Brute via `crunch <len> <len> <charset> | aircrack-ng -w -`."""
    if not bssid:
        bssid = detect_bssid(pcap)
    crunch = subprocess.Popen(["crunch", str(length), str(length), charset],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    cmd = ["aircrack-ng", "-w", "-"]
    if bssid:
        cmd += ["-b", bssid]
    cmd += [pcap]
    proc = subprocess.Popen(cmd, stdin=crunch.stdout, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, universal_newlines=True)
    crunch.stdout.close()
    res = _consume_aircrack(proc, "aircrack-ng:brute", on_event, cancel)
    try:
        crunch.terminate()
    except Exception:  # noqa: BLE001
        pass
    return res


def _run_aircrack(cmd, tool, on_event, cancel):
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            universal_newlines=True)
    return _consume_aircrack(proc, tool, on_event, cancel)


def _consume_aircrack(proc, tool, on_event, cancel):
    key, tested, rate = None, 0, 0.0
    for ln in _stream_lines(proc, cancel):
        m = _KEY_RE.search(ln)
        if m:
            key = m.group(1)
            break
        p = _PROG_RE.search(ln)
        if p:
            tested, rate = int(p.group(1)), float(p.group(2))
            if on_event:
                on_event({"type": "progress", "tested": tested, "rate": rate, "tool": tool})
        cp = _PASS_RE.search(ln)
        if cp and on_event:
            on_event({"type": "candidate", "passphrase": cp.group(1), "tool": tool})
    try:
        proc.terminate()
    except Exception:  # noqa: BLE001
        pass
    return {"ok": key is not None, "key": key, "tested": tested, "tool": tool,
            "cancelled": bool(cancel and cancel.is_set())}


# ── hashcat (uses the .hc22000; needs a working OpenCL backend) ────────────────
def hashcat_run(hc22000, wordlist="", mask="", on_event=None, cancel=None, extra=None):
    mode = "0" if wordlist else "3"
    cmd = ["hashcat", "-m", "22000", "-a", mode, hc22000,
           (wordlist or mask), "--potfile-disable", "--status", "--status-timer", "10",
           "--quiet"]
    if extra:
        cmd += extra
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            universal_newlines=True)
    key, speed = None, 0
    for ln in _stream_lines(proc, cancel):
        if "Speed.#" in ln and on_event:
            mm = re.search(r"([\d.]+)\s*([kMG]?)H/s", ln)
            if mm:
                on_event({"type": "progress", "rate": _hps(mm), "tool": "hashcat"})
        m = _HC_KEY_RE.match(ln)
        if m and ":" in ln and "Speed" not in ln and "Status" not in ln:
            key = m.group(1)
            break
    try:
        proc.terminate()
    except Exception:  # noqa: BLE001
        pass
    return {"ok": key is not None, "key": key, "tool": "hashcat",
            "cancelled": bool(cancel and cancel.is_set())}


def _hps(m):
    v = float(m.group(1))
    return v * {"": 1, "k": 1e3, "M": 1e6, "G": 1e9}[m.group(2)]


# ── unified entry points ───────────────────────────────────────────────────────
def crack_wordlist(pcap, wordlist, bssid="", tool="auto", hc22000="",
                   on_event=None, cancel=None):
    """Dictionary attack. tool: auto|aircrack-ng|hashcat|python."""
    if tool == "auto":
        tool = available_tools()[0]
    if tool == "aircrack-ng" and have("aircrack-ng"):
        return aircrack_wordlist(pcap, wordlist, bssid, on_event, cancel)
    if tool == "hashcat" and have("hashcat") and hc22000:
        return hashcat_run(hc22000, wordlist=wordlist, on_event=on_event, cancel=cancel)
    # python fallback
    res = wc.crack_file(pcap, wordlist)
    return {"ok": res["ok"], "key": res["key"], "tested": res.get("tried", 0), "tool": "python"}


def crack_brute(pcap, charset="0123456789", length=8, bssid="", tool="auto",
                hc22000="", on_event=None, cancel=None):
    """Brute-force attack over a charset/length."""
    if tool == "auto":
        tool = available_tools()[0]
    if tool == "aircrack-ng" and have("aircrack-ng") and have("crunch"):
        return aircrack_brute(pcap, charset, length, bssid, on_event, cancel)
    if tool == "hashcat" and have("hashcat") and hc22000:
        cls = {"0123456789": "?d"}.get(charset, "?a")
        return hashcat_run(hc22000, mask=cls * length, on_event=on_event, cancel=cancel)
    # python fallback (digits only via mask)
    mask = "?d" * length if charset == "0123456789" else None
    if mask:
        res = wc.brute_file(pcap, mask)
        return {"ok": res["ok"], "key": res["key"], "tested": res.get("tried", 0), "tool": "python"}
    return {"ok": False, "key": None, "tool": "python", "error": "charset unsupported in fallback"}


if __name__ == "__main__":
    import argparse, sys
    ap = argparse.ArgumentParser(description="crack a WPA handshake with the best available tool")
    ap.add_argument("pcap")
    ap.add_argument("-w", "--wordlist")
    ap.add_argument("--brute", action="store_true", help="brute digits instead of wordlist")
    ap.add_argument("--length", type=int, default=8)
    ap.add_argument("--bssid", default="")
    ap.add_argument("--tool", default="auto")
    a = ap.parse_args()
    print("tools:", available_tools())
    ev = lambda e: print("  " + str(e), flush=True) if e.get("type") == "progress" else None
    if a.brute:
        r = crack_brute(a.pcap, length=a.length, bssid=a.bssid, tool=a.tool, on_event=ev)
    else:
        r = crack_wordlist(a.pcap, a.wordlist, a.bssid, a.tool, on_event=ev)
    print("RESULT:", r)
    sys.exit(0 if r["ok"] else 1)
