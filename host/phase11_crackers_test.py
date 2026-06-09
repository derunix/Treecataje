#!/usr/bin/env python3
"""Acceptance for the crackers wrapper (real aircrack-ng if installed, else the
pure-Python fallback). Forges a handshake and proves wordlist + brute paths
recover the key through whatever tool is available.

  host/.venv/bin/python host/phase11_crackers_test.py
"""
import os
import sys
import tempfile

import crackers as ck
import phase10_wpa_test as forge_mod


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))
    return cond


def main():
    ok = True
    print("== discovery ==")
    tools = ck.available_tools()
    ok &= check("a tool is available", bool(tools), ", ".join(tools))
    ok &= check("python fallback present", "python" in tools)
    wls = ck.list_wordlists()
    ok &= check("wordlists discovered", len(wls) >= 1, f"{len(wls)} found")

    ap = bytes.fromhex("001122334455")
    sta = bytes.fromhex("aabbccddeeff")
    print("\n== wordlist crack (via best tool) ==")
    pc = os.path.join(tempfile.gettempdir(), "ck_wl.pcap")
    forge_mod.forge(pc, "CkWL", "sunflower", ap, sta)   # 9-char pass
    bssid = ck.detect_bssid(pc)
    ok &= check("bssid detected from pcap", bssid == "00:11:22:33:44:55", bssid)
    wl = os.path.join(tempfile.gettempdir(), "ck_wl.txt")
    with open(wl, "w") as fh:
        fh.write("nope1234\nsunflower\nzzzzzzzz\n")
    res = ck.crack_wordlist(pc, wl, bssid=bssid, tool="auto")
    ok &= check("wordlist recovers key", res["ok"] and res["key"] == "sunflower",
                f"key={res.get('key')!r} tool={res.get('tool')}")

    print("\n== brute (8-digit, early PIN) ==")
    pc2 = os.path.join(tempfile.gettempdir(), "ck_pin.pcap")
    forge_mod.forge(pc2, "CkPIN", "00000007", ap, sta)  # found in ~8 candidates
    res2 = ck.crack_brute(pc2, "0123456789", 8, bssid=ck.detect_bssid(pc2), tool="auto")
    ok &= check("brute recovers early PIN", res2["ok"] and res2["key"] == "00000007",
                f"key={res2.get('key')!r} tool={res2.get('tool')}")

    print("\n" + ("ALL PASS (crackers)" if ok else "SOME FAILURES"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
