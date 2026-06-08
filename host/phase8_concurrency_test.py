#!/usr/bin/env python3
"""Sprint A acceptance — USB stays alive after BLE is enabled (concurrency) +
anti-brute AUTH lockout. USB-only checks (BLE-over-the-air is validated
separately by phase6_ble_test once a token is set).

  host/.venv/bin/python host/phase8_concurrency_test.py --port /dev/ttyACMx
"""
import sys
import time
import argparse

from companion_proto import Companion, auth_digest


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))
    return cond


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    args = ap.parse_args()
    c = Companion(args.port)
    ok = True
    for _ in range(4):
        if c.hello("").get("ok"):
            break
        time.sleep(1)

    print("== baseline ==")
    ok &= check("USB HELLO open", c.hello("").get("ok"))
    ok &= check("free works", c.request("free").ok)

    print("\n== concurrency: USB alive after 'companion ble on' ==")
    r = c.request("companion ble on")
    ok &= check("ble on ack", r.ok and any("ble=on" in l for l in r.lines), " ".join(r.lines))
    time.sleep(2)  # BLE stack comes up
    # THE key check: before Sprint A this timed out (USB went silent).
    r2 = c.request("free", timeout=8)
    ok &= check("USB STILL responds after ble on", r2.ok and any("heap" in l.lower() for l in r2.lines),
                " ".join(r2.lines[:1]) or r2.error)
    r3 = c.request("companion ble status")
    ok &= check("ble reports on", any("ble=on" in l for l in r3.lines), " ".join(r3.lines))
    c.request("companion ble off")
    time.sleep(1)
    ok &= check("USB works after ble off", c.request("uptime").ok)

    print("\n== anti-brute AUTH lockout ==")
    TOKEN = "phase8-tok"
    ok &= check("set token", c.request("companion token set " + TOKEN).ok)
    locked = False
    for i in range(6):
        h = c.hello("")  # triggers a fresh challenge (token set) then wrong auth inside hello
        # hello() with wrong token sends AUTH with a bad digest -> failure
        if h.get("error") and "locked" in (h.get("error") or ""):
            locked = True
            break
    ok &= check("locks out after repeated bad AUTH", locked, "got lock response")

    # cleanup: lockout is ~30s; wait it out, then auth correctly and clear token
    print("  (waiting out lockout to clean up token…)")
    time.sleep(31)
    cleared = False
    for _ in range(3):
        if c.hello(TOKEN).get("ok"):
            cleared = c.request("companion token clear").ok
            break
        time.sleep(1)
    ok &= check("token cleared after unlock", cleared,
                "if FAIL: esptool reset + 'companion token clear'")

    c.close()
    print("\n" + ("ALL PASS (concurrency+anti-brute)" if ok else "SOME FAILURES"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
