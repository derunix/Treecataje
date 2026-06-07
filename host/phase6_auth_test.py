#!/usr/bin/env python3
"""Phase 6 auth tests.

Part A (offline): validate the host challenge-response logic against a fake
device that mimics the firmware (open mode, challenge success/failure, BLE lock).
Part B (--device): run the real handshake over USB: open-mode HELLO, set a token,
verify challenge-response works and a wrong token is rejected, then clear it.
"""
import sys
import time
import hashlib
import argparse

from companion_proto import Response, hello_via, auth_digest, Companion


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))
    return cond


# ---------- Part A: fake device mirroring companion.cpp ----------
class FakeDevice:
    def __init__(self, token="", over_ble=False):
        self.token = token
        self.over_ble = over_ble
        self.nonce = None
        self.authed = False

    def request(self, cmd, timeout=4.0):
        rid = 1
        if cmd.startswith("HELLO"):
            if not self.token:
                if self.over_ble:
                    return Response(rid, False, 7, error="7 AUTH token-required-for-ble")
                self.authed = True
                return Response(rid, True, 0,
                                lines=["fw=Treecataje/dev proto=1 board=T_EMBED_CC1101 "
                                       "mtu=512 name=Bruc auth=open", "caps=wifi,rf,nrf"])
            self.nonce = "0123456789abcdef0123456789abcdef"
            return Response(rid, True, 0,
                            lines=["fw=Treecataje/dev proto=1 mtu=512 name=Bruc auth=required",
                                   f"nonce={self.nonce}"])
        if cmd.startswith("AUTH"):
            resp = cmd.split("resp=", 1)[1].strip()
            expect = hashlib.sha256(f"{self.token}:{self.nonce}".encode()).hexdigest()
            self.nonce = None
            if resp.lower() == expect.lower():
                self.authed = True
                return Response(rid, True, 0, lines=["ok auth=ok", "caps=wifi,rf,nrf"])
            return Response(rid, False, 7, error="7 AUTH")
        return Response(rid, False, 3, error="3 UNSUPPORTED")


def part_a():
    print("== Part A: offline host logic ==")
    ok = True
    # digest must match an independent computation
    ok &= check("auth_digest formula", auth_digest("sekret", "abcd") ==
                hashlib.sha256(b"sekret:abcd").hexdigest())

    # open mode over USB
    info = hello_via(FakeDevice(token="", over_ble=False).request, "")
    ok &= check("open USB authed", info["ok"] and info.get("auth") == "open", str(info.get("auth")))
    ok &= check("open USB caps parsed", info.get("caps") == ["wifi", "rf", "nrf"])

    # open mode over BLE -> refused
    info = hello_via(FakeDevice(token="", over_ble=True).request, "")
    ok &= check("open BLE refused", not info["ok"] and "token-required" in (info.get("error") or ""),
                info.get("error"))

    # challenge success
    info = hello_via(FakeDevice(token="hunter2").request, "hunter2")
    ok &= check("challenge correct token", info["ok"] and info.get("caps") == ["wifi", "rf", "nrf"])

    # challenge wrong token
    info = hello_via(FakeDevice(token="hunter2").request, "WRONG")
    ok &= check("challenge wrong token rejected", not info["ok"], info.get("error"))
    return ok


# ---------- Part B: real device over USB ----------
def part_b(port):
    print(f"\n== Part B: real device over USB ({port}) ==")
    ok = True
    TOKEN = "phase6-test-token"
    c = Companion(port)

    info = c.hello("")
    ok &= check("open-mode HELLO (no token set)", info.get("ok"), str(info.get("auth")))
    ok &= check("token initially unset",
                any("token_set=false" in l for l in c.request("companion token status").lines))

    # set a token
    r = c.request(f"companion token set {TOKEN}")
    ok &= check("token set ok", r.ok and r.code == 0, " ".join(r.lines))

    # re-handshake now requires challenge-response (same USB session lost auth?
    # no — g_authed stays until reconnect; but a NEW HELLO now issues a challenge)
    info = c.hello(TOKEN)
    ok &= check("challenge handshake with correct token", info.get("ok"),
                f"auth={info.get('auth')} err={info.get('error')}")

    # wrong token must fail
    info_bad = c.hello("definitely-wrong")
    ok &= check("wrong token rejected", not info_bad.get("ok"), info_bad.get("error"))

    # re-auth correctly, then clear the token to leave the device open again
    info = c.hello(TOKEN)
    ok &= check("re-auth before cleanup", info.get("ok"))
    r = c.request("companion token clear")
    ok &= check("token cleared", r.ok and r.code == 0, " ".join(r.lines))
    ok &= check("token unset after clear",
                any("token_set=false" in l for l in c.request("companion token status").lines))

    c.close()
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", action="store_true", help="also run Part B against real USB device")
    ap.add_argument("--port", default="/dev/ttyACM0")
    args = ap.parse_args()
    ok = part_a()
    if args.device:
        ok = part_b(args.port) and ok
    print("\n" + ("ALL PASS (phase6 auth)" if ok else "SOME FAILURES (phase6 auth)"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
