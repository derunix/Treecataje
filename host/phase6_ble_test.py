#!/usr/bin/env python3
"""Phase 6 BLE auth test — proves the over-the-air radio-control lock on real HW.

Part A: NO token set + BLE on -> a BLE central is REFUSED (token-required-for-ble).
        Recovers by hard-resetting the chip (esptool) since we can't 'ble off'
        over an unauthenticated BLE link.
Part B: token set + BLE on -> wrong token rejected; correct token authenticates
        and can issue a command; then 'ble off' returns the device to USB.

Usage: host/.venv/bin/python host/phase6_ble_test.py --port /dev/ttyACM0
"""
import sys
import time
import asyncio
import argparse
import subprocess

from companion_proto import Companion
from companion_ble import BleCompanion

ESPTOOL = "/home/derunix/.platformio/packages/tool-esptoolpy/esptool.py"
PYBIN = "/home/derunix/.platformio/penv/bin/python"
SECRET = "ble-phase6-secret"


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))
    return cond


def hard_reset(port):
    subprocess.run([PYBIN, ESPTOOL, "--chip", "esp32s3", "--port", port,
                    "--after", "hard_reset", "--before", "default_reset", "flash_id"],
                   capture_output=True, timeout=40)
    time.sleep(9)  # let firmware boot


def usb(port, token="", timeout=12):
    """Open USB and HELLO (retry while the device boots). Pass the token if the
    device has one configured (challenge-response is enforced on USB too)."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            c = Companion(port)
            if c.hello(token).get("ok"):
                return c
            c.close()
        except Exception as e:  # noqa: BLE001
            last = e
        time.sleep(1)
    raise RuntimeError(f"USB HELLO failed (token={'set' if token else 'open'}): {last}")


async def ble_hello(token, timeout=6.0):
    """Connect over BLE, attempt the handshake with `token`, return (info, ble)."""
    b = BleCompanion(name="Bruc")
    await b.connect()
    info = await b.hello(token, timeout=timeout)
    return info, b


async def part_a(port):
    print("== Part A: NO token -> BLE refused ==")
    ok = True
    c = usb(port)
    c.request("companion token clear")
    ok &= check("token cleared (USB)",
                any("token_set=false" in l for l in c.request("companion token status").lines))
    r = c.request("companion ble on")
    ok &= check("ble on (USB)", r.ok, " ".join(r.lines))
    c.close()
    time.sleep(3)  # BLE stack comes up

    b = None
    try:
        info, b = await ble_hello("")  # no token
        ok &= check("BLE HELLO refused without token",
                    (not info.get("ok")) and "token-required" in (info.get("error") or ""),
                    info.get("error"))
    except Exception as e:  # noqa: BLE001
        ok &= check("BLE HELLO refused without token", False, f"exc {e}")
    finally:
        if b:
            await b.close()
    print("  .. recovering via hard reset (can't 'ble off' unauthenticated)")
    hard_reset(port)
    return ok


async def part_b(port):
    print("\n== Part B: token set -> BLE challenge-response ==")
    ok = True
    c = usb(port)
    r = c.request(f"companion token set {SECRET}")
    ok &= check("token set (USB)", r.ok)
    r = c.request("companion ble on")
    ok &= check("ble on (USB)", r.ok, " ".join(r.lines))
    c.close()
    time.sleep(3)

    # wrong token rejected
    b = None
    try:
        info, b = await ble_hello("wrong-token")
        ok &= check("BLE wrong token rejected", not info.get("ok"), info.get("error"))
    finally:
        if b:
            await b.close()
    time.sleep(1)

    # correct token authenticates + can issue a command, then ble off
    b = None
    try:
        info, b = await ble_hello(SECRET)
        ok &= check("BLE correct token authenticates", info.get("ok"),
                    f"auth={info.get('auth')} err={info.get('error')}")
        if info.get("ok"):
            r = await b.request("companion ping")
            ok &= check("authed BLE command works", r.ok and any("pong" in l for l in r.lines),
                        " ".join(r.lines))
            r = await b.request("companion ble off")  # return to USB
            # The END ack races the serialDevice switch to USB; accept the RSP.
            ok &= check("ble off over BLE", r.ok or any("ble=off" in l for l in r.lines),
                        " ".join(r.lines))
    finally:
        if b:
            await b.close()
    time.sleep(3)

    # back on USB: token is still set, so authenticate with it, then clear.
    try:
        c = usb(port, token=SECRET)
        c.request("companion token clear")
        ok &= check("token cleared after test (USB)",
                    any("token_set=false" in l for l in c.request("companion token status").lines))
        c.close()
    except Exception as e:  # noqa: BLE001
        ok &= check("token cleared after test (USB)", False, f"{e} (try: hard reset)")
        hard_reset(port)
    return ok


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    args = ap.parse_args()
    a = await part_a(args.port)
    b = await part_b(args.port)
    ok = a and b
    print("\n" + ("ALL PASS (phase6 BLE)" if ok else "SOME FAILURES (phase6 BLE)"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
