#!/usr/bin/env python3
"""Sprint B acceptance — capture-to-file on the device.

Verifies: start a capture, it logs sweeps to SD, stop reports a path/bytes/
sha256, the file fetches with a matching sha256, and the host analyzer parses
it. Also checks a capture survives a (simulated) idle gap with no host reads.

  host/.venv/bin/python host/phase9_capture_test.py --port /dev/ttyACMx [--kind telemetry]
"""
import sys
import time
import argparse

from companion_proto import Companion
import companion_compute


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))
    return cond


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--kind", default="telemetry",
                    help="telemetry|wifi|nrf|rf (rf can be 'rf 433 435')")
    ap.add_argument("--duration", type=float, default=8.0)
    args = ap.parse_args()
    c = Companion(args.port)
    ok = True
    for _ in range(4):
        if c.hello("").get("ok"):
            break
        time.sleep(1)

    print("== baseline ==")
    ok &= check("USB HELLO open", c.hello("").get("ok"))
    st = c.capture_status()
    ok &= check("capture idle at start", any("capturing=none" in l for l in st.lines),
                " ".join(st.lines))

    print(f"\n== capture {args.kind} for {args.duration}s ==")
    r = c.capture_start(args.kind, interval=1000)
    ok &= check("capture start ack", r.ok and any("capturing=" in l for l in r.lines),
                " ".join(r.lines) or r.error)
    time.sleep(2)
    mid = c.capture_status()
    ok &= check("status reports active mid-capture",
                any("capturing=" in l and "none" not in l for l in mid.lines),
                " ".join(mid.lines))
    # let it run unattended (no EVT reads) — capture must keep logging to file
    time.sleep(max(0.0, args.duration - 2.0))
    s = c.capture_stop()
    from companion_proto import _kv
    sm = _kv(s.lines)
    ok &= check("capture stop reports path+bytes+sha256",
                bool(s.ok and sm.get("path") and int(sm.get("bytes", 0)) > 0 and sm.get("sha256")),
                f"path={sm.get('path')} bytes={sm.get('bytes')} samples={sm.get('samples')}")

    print("\n== fetch + verify + analyze ==")
    path = sm.get("path", "")
    if path:
        got = c.file_get(path, local_path=f"/tmp/cap_test_{int(time.time())}.txt")
        ok &= check("fetched file sha256 matches device", got.get("sha256") == sm.get("sha256"),
                    f"dev={sm.get('sha256','')[:12]} got={got.get('sha256','')[:12]}")
        analysis = companion_compute.analyze_stream_file(got["path"])
        ok &= check("host analyzer parses capture", bool(analysis) and "error" not in analysis.lower(),
                    analysis.splitlines()[0] if analysis else "")
        print("  --- analysis ---")
        for ln in analysis.splitlines()[:8]:
            print("  " + ln)
    else:
        ok = False

    print("\n== cleanup ==")
    c.request(f"rm {path}") if path else None
    ok &= check("capture idle after stop",
                any("capturing=none" in l for l in c.capture_status().lines), "")

    c.close()
    print("\n" + ("ALL PASS (capture-to-file)" if ok else "SOME FAILURES"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
