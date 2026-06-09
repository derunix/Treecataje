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

    print("\n" + ("ALL PASS (wpa cracker)" if ok else "SOME FAILURES"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
