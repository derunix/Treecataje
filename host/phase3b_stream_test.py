#!/usr/bin/env python3
"""Phase 3 part 2 acceptance test — async EVT streaming over USB.

Starts a 'telemetry' stream, collects EVT for a few seconds, checks the device
emitted increasing ticks while we did nothing else, then stops cleanly.
"""
import sys
import argparse

from companion_proto import Companion


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))
    return cond


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM1")
    ap.add_argument("--seconds", type=float, default=4.5)
    args = ap.parse_args()

    c = Companion(args.port)
    ok = check("HELLO", c.hello().get("ok"))

    busy_before = c.request("companion busy")
    ok &= check("busy=none before", any("owner=none" in l for l in busy_before.lines),
                " ".join(busy_before.lines))

    print(f"\n== telemetry stream ({args.seconds}s) ==")
    out = c.stream("telemetry", duration=args.seconds)
    evs = out["events"]
    for e in evs:
        print("   EVT", e)
    ok &= check("got >=2 events", len(evs) >= 2, f"{len(evs)} events")
    seqs = [int(t.split("=")[1]) for e in evs for t in e.split() if t.startswith("seq=")]
    ok &= check("seq increasing", seqs == sorted(seqs) and len(set(seqs)) == len(seqs), str(seqs))
    ok &= check("events carry heap=", all("heap=" in e for e in evs))

    busy_after = c.request("companion busy")
    ok &= check("busy=none after stop", any("owner=none" in l for l in busy_after.lines),
                " ".join(busy_after.lines))

    c.close()
    print("\n" + ("ALL PASS (streaming)" if ok else "SOME FAILURES (streaming)"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
