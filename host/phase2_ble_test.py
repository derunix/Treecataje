#!/usr/bin/env python3
"""Phase 2 acceptance test — same framed protocol over BLE.

Prereq: BLE API must be ON on the device. Enable it over USB first, e.g.:
    python3 -c "from companion_proto import Companion; c=Companion('/dev/ttyACM1'); c.hello(); print(c.request('companion ble on').lines)"
(after that the USB CLI goes quiet — serialDevice switches to BLE).

Then:  python3 phase2_ble_test.py --debug
"""
import sys
import asyncio
import argparse

from companion_ble import BleCompanion


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))
    return cond


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--token", default="")
    ap.add_argument("--name", default="Bruc")
    ap.add_argument("--debug", action="store_true")
    args = ap.parse_args()

    dev = BleCompanion(name=args.name, debug=args.debug)
    print("[ble] connecting (ensure 'companion ble on' was run over USB) ...")
    await dev.connect()
    ok = True

    print("\n== HELLO over BLE ==")
    info = dev_info = await dev.hello(args.token)
    ok &= check("HELLO ok", info.get("ok"), str(info.get("raw").error if not info.get("ok") else ""))
    ok &= check("board", info.get("board") == "T_EMBED_CC1101", info.get("board", ""))
    ok &= check("caps", bool(info.get("caps")), ",".join(info.get("caps", []))[:70])
    ok &= check("mtu present", "mtu" in info, info.get("mtu", ""))

    print("\n== status / free / verbs over BLE ==")
    r = await dev.request("status")
    ok &= check("status", r.ok and r.code == 0 and len(r.lines) > 0, f"{len(r.lines)} lines")
    for line in r.lines:
        print(f"      {line}")
    f = await dev.request("free")
    ok &= check("free", f.ok and len(f.lines) >= 2, "; ".join(f.lines)[:70])
    p = await dev.request("companion ping")
    ok &= check("ping", p.ok and any("pong" in l for l in p.lines))

    await dev.close()
    print("\n" + ("ALL PASS (BLE)" if ok else "SOME FAILURES (BLE)"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
