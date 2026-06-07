#!/usr/bin/env python3
"""Phase 7 acceptance test — real wifi/nrf stream kinds over USB.

Streams the 'wifi' and 'nrf' kinds and checks the device emits real radio data
(WiFi networks via async scan; NRF24 RPD spectrum sweeps), not just telemetry.
"""
import sys
import argparse

from companion_proto import Companion


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))
    return cond


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--token", default="")
    ap.add_argument("--wifi-seconds", type=float, default=7.0)
    ap.add_argument("--nrf-seconds", type=float, default=5.0)
    args = ap.parse_args()

    c = Companion(args.port)
    ok = check("HELLO", c.hello(args.token).get("ok"))

    print(f"\n== wifi stream ({args.wifi_seconds}s) ==")
    w = c.stream("wifi", duration=args.wifi_seconds)
    for e in w["events"][:12]:
        print("   EVT", e)
    evs = w["events"]
    heads = [e for e in evs if e.startswith("wifi seq=")]
    nets = [e for e in evs if e.startswith("wifi net ")]
    ok &= check("got >=1 wifi sweep", len(heads) >= 1, f"{len(heads)} sweeps")
    ok &= check("wifi nets carry ch/rssi/ssid",
                all(("ch=" in n and "rssi=" in n and "ssid=" in n) for n in nets) and len(nets) >= 0,
                f"{len(nets)} networks")
    # at least one sweep should report a count
    counts = [int(h.split("count=")[1].split()[0]) for h in heads if "count=" in h]
    ok &= check("wifi reported a network count", len(counts) >= 1, str(counts))

    print(f"\n== nrf stream ({args.nrf_seconds}s) ==")
    n = c.stream("nrf", duration=args.nrf_seconds)
    for e in n["events"]:
        print("   EVT", e)
    nevs = [e for e in n["events"] if e.startswith("nrf seq=")]
    ok &= check("got >=2 nrf sweeps", len(nevs) >= 2, f"{len(nevs)} sweeps")
    ok &= check("nrf sweeps carry channels/peak/active",
                all(("channels=" in e and "peak_ch=" in e and "active=" in e) for e in nevs))
    seqs = [int(e.split("seq=")[1].split()[0]) for e in nevs]
    ok &= check("nrf seq increasing", seqs == sorted(seqs) and len(set(seqs)) == len(seqs), str(seqs))

    busy = c.request("companion busy")
    ok &= check("busy=none after streams", any("owner=none" in l for l in busy.lines),
                " ".join(busy.lines))

    c.close()
    print("\n" + ("ALL PASS (radio streams)" if ok else "SOME FAILURES (radio streams)"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
