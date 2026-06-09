#!/usr/bin/env python3
"""Offline acceptance for the WPA cracker (no device, no network).

1. PMK against the published IEEE 802.11i test vectors (independent check of the
   PBKDF2 path — not self-referential).
2. Forge a cryptographically-valid WPA2 4-way handshake (msg1+msg2) into a
   DLT_105 pcap, then prove the parser + PTK/MIC + wordlist crack recover it.

  host/.venv/bin/python host/phase10_wpa_test.py
"""
import os
import struct
import sys
import tempfile

import wpa_crack as wc


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))
    return cond


# ── pcap forging helpers ───────────────────────────────────────────────────────
def _pcap(path, frames):
    with open(path, "wb") as fh:
        fh.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 105))
        for i, fr in enumerate(frames):
            fh.write(struct.pack("<IIII", i, 0, len(fr), len(fr)))
            fh.write(fr)


def _beacon(bssid, ssid):
    hdr = b"\x80\x00" + b"\x00\x00" + b"\xff" * 6 + bssid + bssid + b"\x00\x00"
    fixed = b"\x00" * 8 + b"\x64\x00" + b"\x31\x04"
    ssid_ie = bytes([0, len(ssid)]) + ssid.encode()
    return hdr + fixed + ssid_ie


def _eapol_key(key_info, nonce, mic=b"\x00" * 16, key_data=b""):
    body = (b"\x02"                                   # descriptor type = RSN
            + struct.pack(">H", key_info)
            + struct.pack(">H", 16)                   # key length
            + b"\x00" * 8                             # replay counter
            + nonce                                   # 32
            + b"\x00" * 16                            # key IV
            + b"\x00" * 8                             # key RSC
            + b"\x00" * 8                             # key ID
            + mic                                     # 16
            + struct.pack(">H", len(key_data)) + key_data)
    return b"\x02\x03" + struct.pack(">H", len(body)) + body


def _data_frame(fromds, ap, sta, eapol):
    if fromds:                       # AP -> STA
        fc = b"\x08\x02"; a1, a2, a3 = sta, ap, ap
    else:                            # STA -> AP
        fc = b"\x08\x01"; a1, a2, a3 = ap, sta, ap
    hdr = fc + b"\x00\x00" + a1 + a2 + a3 + b"\x00\x00"
    return hdr + wc.EAPOL_LLC + eapol


def forge(path, ssid, passphrase, ap, sta):
    anonce = bytes(range(32))
    snonce = bytes(range(32, 64))
    # msg1: AP->STA, ANonce, ACK, pairwise, version 2
    m1 = _eapol_key(0x008A, anonce)
    # msg2: STA->AP, SNonce, MIC+pairwise, version 2 — MIC computed over the frame
    m2_nomic = _eapol_key(0x010A, snonce, mic=b"\x00" * 16)
    p = wc.pmk(passphrase, ssid)
    t = wc.ptk(p, ap, sta, anonce, snonce)
    the_mic = wc.mic(t[:16], m2_nomic, 2)
    m2 = _eapol_key(0x010A, snonce, mic=the_mic)
    _pcap(path, [_beacon(ap, ssid),
                 _data_frame(True, ap, sta, m1),
                 _data_frame(False, ap, sta, m2)])
    return the_mic


def main():
    ok = True
    print("== 1. PMK vs published IEEE 802.11i vectors ==")
    vectors = [
        ("password", "IEEE", "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"),
        ("ThisIsAPassword", "ThisIsASSID",
         "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af"),
    ]
    for pw, ssid, expect in vectors:
        got = wc.pmk(pw, ssid).hex()
        ok &= check(f"PMK({pw!r},{ssid!r})", got == expect, got[:24] + "…")

    print("\n== 2. forge + parse + crack a WPA2 handshake ==")
    ap = bytes.fromhex("001122334455")
    sta = bytes.fromhex("aabbccddeeff")
    ssid, secret = "TestNet5", "correct horse"   # 13 chars, in range
    tmp = os.path.join(tempfile.gettempdir(), "wpa_forged.pcap")
    forge(tmp, ssid, secret, ap, sta)

    ssids, hss = wc.parse(tmp)
    ok &= check("beacon SSID parsed", ssid in ssids.values(), str(list(ssids.values())))
    ok &= check("one handshake found", len(hss) == 1, f"{len(hss)} found")
    if hss:
        h = hss[0]
        ok &= check("msgs 1+2 captured", {1, 2}.issubset(h.msgs), str(sorted(h.msgs)))
        ok &= check("handshake crackable", h.crackable(), h.label())
        ok &= check("correct passphrase verifies", h.verify(secret))
        ok &= check("wrong passphrase rejected", not h.verify("wrongpass123"))

    # wordlist crack (decoys + the secret)
    wl = os.path.join(tempfile.gettempdir(), "wpa_words.txt")
    with open(wl, "w") as fh:
        fh.write("\n".join(["password", "12345678", "letmein0", secret, "trailing"]))
    res = wc.crack_file(tmp, wl)
    ok &= check("wordlist crack finds secret", res["ok"] and res["key"] == secret,
                f"key={res.get('key')!r} tried={res.get('tried')}")

    # negative: wordlist without the secret
    wl2 = os.path.join(tempfile.gettempdir(), "wpa_words_neg.txt")
    with open(wl2, "w") as fh:
        fh.write("password\n12345678\nletmein0\n")
    res2 = wc.crack_file(tmp, wl2)
    ok &= check("crack fails without secret in wordlist", not res2["ok"], str(res2.get("key")))

    print("\n== 3. brute-force by mask ==")
    tmp2 = os.path.join(tempfile.gettempdir(), "wpa_pin.pcap")
    forge(tmp2, "PinNet", "00000042", ap, sta)   # 8-digit PIN, early in ?d*8 order
    ok &= check("mask keyspace ?d*8", wc.mask_keyspace("?d?d?d?d?d?d?d?d") == 100_000_000)
    res3 = wc.brute_file(tmp2, "?d?d?d?d?d?d?d?d", limit=1000)  # secret at position 43
    ok &= check("brute finds 8-digit PIN", res3["ok"] and res3["key"] == "00000042",
                f"key={res3.get('key')!r} tried={res3.get('tried')}")
    ok &= check("short mask yields nothing (WPA min 8)", not list(wc.mask_candidates("?d?d?d")))

    print("\n== 4. full attack orchestration (mock device) ==")

    class MockDev:
        """Stand-in for a connected Companion: scan finds the AP, capture returns
        a forged handshake pcap. Exercises wifi_attack.run_attack end-to-end."""
        def __init__(self, pcap):
            self._pcap = pcap

        def find_ap(self, ssid, scan_secs=4.0):
            return {"bssid": "00:11:22:33:44:55", "ch": 6, "ssid": ssid, "rssi": -42}

        def deauth(self, bssid, sta="broadcast", ch=0, count=8, timeout=8.0):
            class R: ok = True; lines = ["deauth sent"]; error = ""
            return R()

        def capture_handshake(self, bssid="", ch=0, secs=20.0, deauth_count=8,
                              rounds=3, local_path=None):
            import shutil
            shutil.copy(self._pcap, local_path)
            return {"path": "/BruceCapture/hs.pcap", "bytes": os.path.getsize(local_path),
                    "samples": 3, "sha256": "", "local": local_path, "verified": False}

    import wifi_attack
    forged = os.path.join(tempfile.gettempdir(), "orch.pcap")
    forge(forged, "OrchNet", "letmein99", ap, sta)
    wl3 = os.path.join(tempfile.gettempdir(), "orch_wl.txt")
    with open(wl3, "w") as fh:
        fh.write("nope0000\nletmein99\nother123\n")
    res4 = wifi_attack.run_attack(MockDev(forged), ssid="OrchNet", wordlist=wl3,
                                  local_dir=tempfile.gettempdir(), log=lambda *_: None)
    ok &= check("orchestration recovers key via wordlist",
                res4["ok"] and res4["key"] == "letmein99" and res4["method"] == "wordlist",
                f"key={res4.get('key')!r} method={res4.get('method')}")

    print("\n" + ("ALL PASS (wpa cracker)" if ok else "SOME FAILURES"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
