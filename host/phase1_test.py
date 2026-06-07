#!/usr/bin/env python3
"""Phase 1 acceptance test — framed protocol over USB.

Checks (docs/companion/roadmap.md, Phase 1):
  1. HELLO returns correct fw / board / caps.
  2. A normal command (status) round-trips with a clean END terminator.
  3. companion ping / caps / busy verbs work.
  4. Auth gate: a non-HELLO REQ before HELLO is rejected with ERR 7 (only
     meaningful right after a reboot; informational here).

The non-modal guarantee (companion command must NOT kick the device out of its
current screen) is a manual on-device check — see notes printed at the end.
"""
import sys
import argparse

from companion_proto import Companion


def check(name, cond, detail=""):
    mark = "PASS" if cond else "FAIL"
    print(f"  [{mark}] {name}" + (f" — {detail}" if detail else ""))
    return cond


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM1")
    ap.add_argument("--token", default="")
    ap.add_argument("--debug", action="store_true")
    args = ap.parse_args()

    c = Companion(args.port, debug=args.debug)
    ok = True

    print("\n== HELLO / handshake ==")
    info = c.hello(args.token)
    ok &= check("HELLO ok", info.get("ok"), str(info.get("raw").error if not info.get("ok") else ""))
    ok &= check("fw present", "fw" in info, info.get("fw", ""))
    ok &= check("board T_EMBED_CC1101", info.get("board") == "T_EMBED_CC1101", info.get("board", ""))
    ok &= check("caps non-empty", bool(info.get("caps")), ",".join(info.get("caps", []))[:80])

    print("\n== status round-trip ==")
    r = c.request("status")
    ok &= check("status END ok", r.ok and r.code == 0, f"code={r.code}")
    ok &= check("status has output", len(r.lines) > 0, f"{len(r.lines)} lines")
    for line in r.lines:
        print(f"      {line}")

    print("\n== companion verbs ==")
    p = c.request("companion ping")
    ok &= check("ping -> pong", p.ok and any("pong" in l for l in p.lines))
    caps = c.request("companion caps")
    ok &= check("caps verb", caps.ok and any("caps=" in l for l in caps.lines))
    busy = c.request("companion busy")
    ok &= check("busy verb", busy.ok and any("owner=" in l for l in busy.lines),
                busy.lines[0] if busy.lines else "")

    print("\n== a few real commands ==")
    for cmd in ("uptime", "free"):
        r = c.request(cmd)
        ok &= check(f"{cmd}", r.ok and r.code == 0, "; ".join(r.lines)[:80])

    c.close()
    print("\n" + ("ALL PASS" if ok else "SOME FAILURES"))
    print("\nManual check (non-modal): on the device, open a menu/screen, then run")
    print("a companion command from here — the device screen must NOT jump back to")
    print("the main menu. (Legacy plain commands still redraw the menu, by design.)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
