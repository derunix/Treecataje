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


# ---------------- live stream analyzers (EVT payload lists) ----------------
def analyze_wifi_stream(events):
    nets, sweeps = {}, 0
    for e in events:
        if e.startswith("wifi seq="):
            sweeps += 1
            continue
        if not e.startswith("wifi net "):
            continue
        si = e.find(" ssid=")  # leading space avoids matching inside "bssid="
        ssid = e[si + 6:].strip() if si >= 0 else ""
        head = e[:si] if si >= 0 else e
        d = dict(t.split("=", 1) for t in head.split() if "=" in t)
        try:
            rssi = int(d.get("rssi", "-100"))
        except ValueError:
            rssi = -100
        bssid = d.get("bssid", ssid or "?")
        cur = nets.get(bssid)
        if cur is None or rssi > cur["rssi"]:
            nets[bssid] = {"ssid": ssid or "<hidden>", "ch": d.get("ch", "?"),
                           "enc": d.get("enc", "?"), "rssi": rssi, "bssid": bssid}
    try:
        import companion_dicts
        for n in nets.values():
            n["vendor"] = companion_dicts.lookup_oui(n["bssid"])
    except Exception:
        pass
    return {"sweeps": sweeps, "nets": sorted(nets.values(), key=lambda n: -n["rssi"])}


def report_wifi_stream(a):
    out = [f"WiFi stream: {len(a['nets'])} unique APs over {a['sweeps']} sweep(s)", ""]
    out.append(f"  {'RSSI':>5}  {'CH':>3}  {'ENC':<9} SSID")
    for n in a["nets"]:
        bars = _bar(n["rssi"] + 100, 70, 16)  # -100..-30 -> bar
        vend = f" {{{n['vendor']}}}" if n.get("vendor") else ""
        out.append(f"  {n['rssi']:>5}  {n['ch']:>3}  {n['enc']:<9} {n['ssid']}  [{n['bssid']}]{vend} {bars}")
    chans = {}
    for n in a["nets"]:
        chans[n["ch"]] = chans.get(n["ch"], 0) + 1
    if chans:
        out += ["", "  channel usage:"]
        mx = max(chans.values())
        for ch in sorted(chans, key=lambda c: (len(c), c)):
            out.append(f"   ch{ch:>3} {chans[ch]:>2} {_bar(chans[ch], mx, 30)}")
    return "\n".join(out)


def analyze_nrf_stream(events):
    chan, sweeps = {}, 0
    for e in events:
        if not e.startswith("nrf seq="):
            continue
        sweeps += 1
        m = re.search(r"active=([0-9:,]+)", e)
        if m:
            for pair in m.group(1).split(","):
                if ":" in pair:
                    c, h = pair.split(":")
                    chan[int(c)] = chan.get(int(c), 0) + int(h)
    return {"sweeps": sweeps, "chan": chan}


def report_nrf_stream(a):
    out = [f"NRF24 stream: {a['sweeps']} sweep(s), {len(a['chan'])} active channels", ""]
    if not a["chan"]:
        out.append("  (no RPD activity detected)")
        return "\n".join(out)
    mx = max(a["chan"].values())
    for ch in sorted(a["chan"]):
        mhz = 2400 + ch
        out.append(f"  ch{ch:>3} ({mhz} MHz) {a['chan'][ch]:>4} {_bar(a['chan'][ch], mx)}")
    return "\n".join(out)


def analyze_rf_stream(events):
    acc, n = None, 0
    f0 = f1 = step = None
    peak = (-200, None)
    sweeps = 0
    for e in events:
        if not e.startswith("rf seq="):
            continue
        sweeps += 1
        d = dict(t.split("=", 1) for t in e.split() if "=" in t and not t.startswith("rssi="))
        try:
            f0, f1, step = float(d["f0"]), float(d["f1"]), float(d["step"])
            pk = int(d.get("peak", "-200"))
            if pk > peak[0]:
                peak = (pk, float(d.get("peak_f", 0)))
        except (KeyError, ValueError):
            pass
        m = re.search(r"rssi=([-\d,]+)", e)
        vals = [int(x) for x in m.group(1).split(",")] if m else []
        if acc is None and vals:
            acc = [0.0] * len(vals)
        if vals and acc and len(vals) == len(acc):
            for i, v in enumerate(vals):
                acc[i] += v
            n += 1
    avg = [v / n for v in acc] if (acc and n) else []
    return {"sweeps": sweeps, "f0": f0, "f1": f1, "step": step, "avg": avg, "peak": peak}


def report_rf_stream(a):
    if not a["avg"]:
        return f"RF stream: {a['sweeps']} sweep(s), no spectrum data"
    out = [f"Sub-GHz spectrum: {a['f0']:.2f}-{a['f1']:.2f} MHz, {len(a['avg'])} bins, "
           f"{a['sweeps']} sweep(s)"]
    if a["peak"][1]:
        out.append(f"  peak: {a['peak'][0]} dBm @ {a['peak'][1]:.2f} MHz")
    out += ["", "  " + _sparkline(a["avg"], width=len(a["avg"]))]
    # top 5 strongest bins
    idx = sorted(range(len(a["avg"])), key=lambda i: -a["avg"][i])[:5]
    out.append("  strongest bins:")
    for i in sorted(idx):
        f = a["f0"] + a["step"] * i
        out.append(f"   {f:7.2f} MHz  {a['avg'][i]:6.1f} dBm  {_bar(int(a['avg'][i]) + 120, 90, 24)}")
    return "\n".join(out)


def save_stream(kind, events, directory):
    """Persist a collected stream to a timestamped text file (header + one EVT
    payload per line). Returns the path. `directory` is created if needed."""
    import os
    import time
    os.makedirs(directory, exist_ok=True)
    base = (kind or "stream").split()[0]
    path = os.path.join(directory, f"{base}-{time.strftime('%Y%m%d-%H%M%S')}.txt")
    with open(path, "w") as fh:
        fh.write(f"# kind: {kind}\n")
        fh.write(f"# saved: {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        for e in events:
            fh.write(e + "\n")
    return path


def analyze_stream_file(path):
    """Read a capture saved by save_stream() and analyze it."""
    kind, events = "telemetry", []
    with open(path) as fh:
        for line in fh:
            line = line.rstrip("\n")
            if line.startswith("# kind:"):
                kind = line.split(":", 1)[1].strip()
            elif line.startswith("#") or not line.strip():
                continue
            else:
                events.append(line)
    return analyze_stream(kind, events)


def run_recon(stream_fn, wifi_s=6.0, nrf_s=4.0, rf_s=4.0, rf_band=(433.0, 434.8)):
    """Sweep wifi -> nrf -> rf using stream_fn(kind, seconds)->events. Returns a
    list of (kind, events). stream_fn is supplied by the caller (device-agnostic)."""
    results = []
    results.append(("wifi", stream_fn("wifi", wifi_s)))
    results.append(("nrf", stream_fn("nrf", nrf_s)))
    rfk = "rf %g %g" % rf_band
    results.append((rfk, stream_fn(rfk, rf_s)))
    return results


def recon_report(results, when=""):
    """Combine per-stream analyses into one markdown report."""
    out = ["# Companion recon report"]
    if when:
        out.append(when)
    for kind, events in results:
        out += ["", "## " + kind, "", analyze_stream(kind, events)]
    return "\n".join(out) + "\n"


def analyze_telemetry_stream(events):
    heaps, mss = [], []
    for e in events:
        if not e.startswith("tick"):
            continue
        d = dict(t.split("=", 1) for t in e.split() if "=" in t)
        if d.get("heap", "").isdigit():
            heaps.append(int(d["heap"]))
        if d.get("ms", "").lstrip("-").isdigit():
            mss.append(int(d["ms"]))
    return {"ticks": len(heaps), "heaps": heaps, "span_ms": (mss[-1] - mss[0]) if len(mss) > 1 else 0}


def report_telemetry_stream(a):
    if not a["heaps"]:
        return "telemetry: no ticks"
    lo, hi = min(a["heaps"]), max(a["heaps"])
    out = [f"Telemetry: {a['ticks']} ticks over {a['span_ms']/1000:.1f}s",
           f"  free heap: now {a['heaps'][-1]} B, min {lo}, max {hi}, delta {a['heaps'][-1]-a['heaps'][0]:+d}",
           "  " + _sparkline(a["heaps"], width=len(a["heaps"]))]
    return "\n".join(out)


def analyze_stream(kind, events):
    """Report on a collected list of EVT payloads from companion stream <kind>."""
    base = (kind or "").split()[0] if kind else ""
    if base == "wifi":
        return report_wifi_stream(analyze_wifi_stream(events))
    if base == "nrf":
        return report_nrf_stream(analyze_nrf_stream(events))
    if base == "rf":
        return report_rf_stream(analyze_rf_stream(events))
    if base == "telemetry":
        return report_telemetry_stream(analyze_telemetry_stream(events))
    body = "\n".join("  " + e for e in events[-30:]) or "  (no events)"
    return f"{kind} stream: {len(events)} events\n{body}"


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
    ap.add_argument("--stream", help="analyze a saved stream capture file (from save_stream)")
    ap.add_argument("--port", default="/dev/ttyACM1")
    args = ap.parse_args()
    if args.stream:
        print(analyze_stream_file(args.stream))
    elif args.pull:
        print(fetch_and_analyze(args.pull, args.port))
    elif args.target:
        with open(args.target, "rb") as fh:
            print(analyze(args.target, fh.read()))
    else:
        ap.error("give a local file or --pull <remote>")


if __name__ == "__main__":
    main()
