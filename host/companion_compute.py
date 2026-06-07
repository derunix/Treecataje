#!/usr/bin/env python3
"""Host-compute augmentation: analyze device capture artifacts on the host.

The device is the RF/peripheral frontend; heavy parsing/analysis/visualization
runs here (uConsole/x86). Pattern: capture on device -> file_get -> analyze.

Analyzers (auto-detected by name/content), dependency-free (ASCII viz):
  * NRF24 scan log   "CH <ch> <ADDR> hits=N pipe=P len=L"
  * battery CSV      "timestamp,percent,voltage,charging"
  * pcap             classic libpcap (packet count / linktype / 802.11 best-effort)
  * generic         size + text/hex head

Use:
  python companion_compute.py <local_file>
  python companion_compute.py --pull /nrf_scan.log [--port /dev/ttyACM1]
"""
import re
import sys
import struct
import argparse
from collections import Counter

SPARK = "▁▂▃▄▅▆▇█"


def _sparkline(values, width=60):
    if not values:
        return ""
    if len(values) > width:
        step = len(values) / width
        values = [values[int(i * step)] for i in range(width)]
    lo, hi = min(values), max(values)
    if hi == lo:
        return SPARK[0] * len(values)
    return "".join(SPARK[int((v - lo) / (hi - lo) * (len(SPARK) - 1))] for v in values)


def _bar(n, mx, width=40):
    if mx <= 0:
        return ""
    return "█" * max(1, int(n / mx * width)) if n else ""


# ---------------- NRF24 scan ----------------
_NRF_RE = re.compile(r"CH\s+(\d+)\s+([0-9A-Fa-f]+)\s+hits=(\d+)\s+pipe=(\d+)\s+len=(\d+)")


def analyze_nrf_scan(text):
    chan, addr, pipe, length = Counter(), Counter(), Counter(), []
    total = 0
    for line in text.splitlines():
        m = _NRF_RE.search(line)
        if not m:
            continue
        ch, a, hits, p, ln = int(m[1]), m[2].upper(), int(m[3]), int(m[4]), int(m[5])
        chan[ch] += hits
        addr[a] += hits
        pipe[p] += 1
        length.append(ln)
        total += 1
    return {
        "kind": "nrf_scan",
        "records": total,
        "unique_addrs": len(addr),
        "channels": dict(sorted(chan.items())),
        "top_addrs": addr.most_common(10),
        "pipes": dict(sorted(pipe.items())),
        "len_avg": (sum(length) / len(length)) if length else 0,
    }


def report_nrf(a):
    out = [f"NRF24 scan: {a['records']} records, {a['unique_addrs']} unique addresses",
           f"avg payload len {a['len_avg']:.1f} bytes", "", "Channel activity (hits):"]
    mx = max(a["channels"].values(), default=0)
    for ch, n in a["channels"].items():
        out.append(f"  CH{ch:>3} {n:>5} {_bar(n, mx)}")
    out.append("")
    out.append("Busiest channels: " + ", ".join(
        f"CH{c}({n})" for c, n in sorted(a["channels"].items(), key=lambda x: -x[1])[:5]))
    out.append("Top addresses:    " + ", ".join(f"{ad}({n})" for ad, n in a["top_addrs"][:5]))
    out.append("Pipe distribution: " + ", ".join(f"p{p}={n}" for p, n in a["pipes"].items()))
    return "\n".join(out)


# ---------------- battery CSV ----------------
def analyze_battery_csv(text):
    rows = []
    for line in text.splitlines()[1:]:
        p = line.split(",")
        if len(p) >= 4:
            try:
                rows.append((p[0], int(p[1]), float(p[2]), int(p[3])))
            except ValueError:
                pass
    if not rows:
        return {"kind": "battery", "rows": 0}
    pct = [r[1] for r in rows]
    volt = [r[2] for r in rows]
    chg = [r[3] for r in rows]
    return {
        "kind": "battery",
        "rows": len(rows),
        "from": rows[0][0], "to": rows[-1][0],
        "pct_min": min(pct), "pct_max": max(pct),
        "volt_min": min(volt), "volt_max": max(volt), "volt_avg": sum(volt) / len(volt),
        "charging_ratio": sum(chg) / len(chg),
        "pct_series": pct, "volt_series": volt,
    }


def report_battery(a):
    if not a.get("rows"):
        return "battery: no rows"
    return "\n".join([
        f"Battery log: {a['rows']} samples  {a['from']} -> {a['to']}",
        f"percent: {a['pct_min']}..{a['pct_max']}%   charging {a['charging_ratio']*100:.0f}% of the time",
        f"voltage: {a['volt_min']:.3f}..{a['volt_max']:.3f}V (avg {a['volt_avg']:.3f})",
        f"percent  {_sparkline(a['pct_series'])}",
        f"voltage  {_sparkline(a['volt_series'])}",
    ])


# ---------------- pcap (best-effort) ----------------
def analyze_pcap(data):
    if len(data) < 24:
        return {"kind": "pcap", "error": "too short"}
    magic = data[:4]
    if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
        endian = "<"
    elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
        endian = ">"
    else:
        return {"kind": "pcap", "error": f"not a pcap (magic {magic.hex()})"}
    linktype = struct.unpack(endian + "I", data[20:24])[0]
    off, count, total = 24, 0, 0
    eapol = 0
    while off + 16 <= len(data):
        _, _, caplen, _ = struct.unpack(endian + "IIII", data[off:off + 16])
        off += 16
        if off + caplen > len(data):
            break
        pkt = data[off:off + caplen]
        if b"\x88\x8e" in pkt:  # EAPOL ethertype (best-effort)
            eapol += 1
        off += caplen
        count += 1
        total += caplen
    return {"kind": "pcap", "linktype": linktype, "packets": count,
            "bytes": total, "eapol_frames": eapol}


def report_pcap(a):
    if a.get("error"):
        return f"pcap: {a['error']}"
    return "\n".join([
        f"pcap: {a['packets']} packets, {a['bytes']} bytes, linktype={a['linktype']}",
        f"EAPOL frames (handshake candidates): {a['eapol_frames']}"
        + ("  -> possible WPA handshake, crackable on host" if a["eapol_frames"] >= 2 else ""),
    ])


# ---------------- dispatch ----------------
def analyze(name, data: bytes):
    head = data[:64]
    if head[:4] in (b"\xd4\xc3\xb2\xa1", b"\xa1\xb2\xc3\xd4", b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d"):
        return report_pcap(analyze_pcap(data))
    try:
        text = data.decode(errors="replace")
    except Exception:
        text = ""
    if _NRF_RE.search(text):
        return report_nrf(analyze_nrf_scan(text))
    if text.startswith("timestamp,") and "voltage" in text.split("\n", 1)[0]:
        return report_battery(analyze_battery_csv(text))
    # generic
    printable = sum(c in range(9, 127) for c in head) / max(1, len(head))
    if printable > 0.85:
        return f"text file ({len(data)} B). head:\n" + text[:300]
    return f"binary file ({len(data)} B). head hex:\n" + head.hex()


def fetch_and_analyze(remote, port="/dev/ttyACM1"):
    from companion_proto import Companion
    c = Companion(port)
    c.hello()
    out = c.file_get(remote, None, chunk=512, timeout=120)
    c.close()
    return analyze(remote, out["data"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("target", nargs="?", help="local file to analyze")
    ap.add_argument("--pull", help="remote device path to fetch then analyze")
    ap.add_argument("--port", default="/dev/ttyACM1")
    args = ap.parse_args()
    if args.pull:
        print(fetch_and_analyze(args.pull, args.port))
    elif args.target:
        with open(args.target, "rb") as fh:
            print(analyze(args.target, fh.read()))
    else:
        ap.error("give a local file or --pull <remote>")


if __name__ == "__main__":
    main()
