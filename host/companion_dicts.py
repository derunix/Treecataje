#!/usr/bin/env python3
"""Companion dictionaries — curated, device-compatible reference data.

Parses the files under host/dictionaries/ (IR signal DBs, RFID key dicts,
sub-GHz captures) into structured entries for the GUI/TUI/MCP, and builds the
device commands / file uploads to use them. See dictionaries/README.md.
"""
import os
import glob

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dictionaries")
IR_DIR = os.path.join(ROOT, "ir")
RFID_DIR = os.path.join(ROOT, "rfid")
SUBGHZ_DIR = os.path.join(ROOT, "subghz")

# device storage directories (Bruce conventions)
DEV_IR = "/BruceIR"
DEV_RF = "/BruceRF"
DEV_RFID_KEYS = "/BruceRFID/keys.conf"


# ---------------- IR ----------------
def parse_ir_file(path):
    """Parse a Flipper .ir file into a list of signal dicts."""
    sigs, cur = [], {}

    def flush():
        if cur.get("name") and cur.get("type"):
            sigs.append(dict(cur))

    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()
            if line.startswith("#"):
                flush(); cur.clear(); continue
            if ":" not in line:
                continue
            k, _, v = line.partition(":")
            k, v = k.strip().lower(), v.strip()
            if k == "name":
                flush(); cur.clear(); cur["name"] = v
            elif k in ("type", "protocol", "address", "command", "data", "frequency", "bits"):
                cur[k] = v
    flush()
    return sigs


def ir_files():
    return sorted(glob.glob(os.path.join(IR_DIR, "*.ir")))


def ir_entries():
    """All IR signals across all .ir files: list of dicts with brand + fields."""
    out = []
    for path in ir_files():
        brand = os.path.splitext(os.path.basename(path))[0]
        for s in parse_ir_file(path):
            s = dict(s); s["brand"] = brand; s["path"] = path
            out.append(s)
    return out


def _hex8(hexstr):
    """The serial `ir tx` requires 8-char hex address/command (it then uses the
    first byte internally). Strip spaces, pad/truncate to 8 hex chars."""
    h = (hexstr or "").replace(" ", "").upper() or "00"
    return (h + "00000000")[:8]


def ir_tx_line(sig):
    """Direct send command for a parsed IR signal: 'ir tx <proto> <addr> <cmd>'.
    Returns None for raw signals (use deploy + tx_from_file instead)."""
    if sig.get("type") != "parsed":
        return None
    return "ir tx %s %s %s" % (sig.get("protocol", "NEC"),
                               _hex8(sig.get("address")), _hex8(sig.get("command")))


# ---------------- RFID keys ----------------
def key_files():
    return sorted(glob.glob(os.path.join(RFID_DIR, "*.keys")))


def parse_keys(path):
    keys = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("//"):
                continue
            k = line.upper()
            if len(k) == 12 and all(c in "0123456789ABCDEF" for c in k):
                keys.append(k)
    return keys


def build_keys_conf(paths):
    """Merge key files into keys.conf text (deduped, order-preserving)."""
    seen, out = set(), ["// Bruce MIFARE keys.conf — deployed by companion"]
    for p in paths:
        for k in parse_keys(p):
            if k not in seen:
                seen.add(k); out.append(k)
    return "\n".join(out) + "\n"


# ---------------- sub-GHz ----------------
def sub_files():
    return sorted(glob.glob(os.path.join(SUBGHZ_DIR, "*.sub")))


# ---------------- deploy targets ----------------
def deploy_remote(category, local_path):
    """Device path to upload a dictionary file to."""
    base = os.path.basename(local_path)
    if category == "ir":
        return DEV_IR + "/" + base
    if category == "subghz":
        return DEV_RF + "/" + base
    if category == "rfid":
        return DEV_RFID_KEYS
    raise ValueError("unknown category " + category)


def summary():
    irs = ir_entries()
    brands = sorted({e["brand"] for e in irs})
    keyfiles = [(os.path.basename(p), len(parse_keys(p))) for p in key_files()]
    subs = [os.path.basename(p) for p in sub_files()]
    return {"ir_brands": brands, "ir_signals": len(irs), "key_files": keyfiles, "subs": subs}


def import_ir_tree(srcdir, dest=IR_DIR):
    """Bulk-import .ir files (e.g. a cloned Flipper-IRDB) into the IR dictionary.
    Recurses srcdir; names each copy "<ParentDir>__<file>.ir" to keep brands
    distinct and avoid collisions. Returns (imported, skipped)."""
    import shutil
    os.makedirs(dest, exist_ok=True)
    imported = skipped = 0
    for root, _dirs, files in os.walk(srcdir):
        for fn in files:
            if not fn.lower().endswith(".ir"):
                continue
            src = os.path.join(root, fn)
            parent = os.path.basename(root) or "IR"
            safe = "".join(ch if ch.isalnum() or ch in "-_" else "_" for ch in parent)
            out = os.path.join(dest, f"{safe}__{fn}")
            try:
                shutil.copyfile(src, out)
                imported += 1
            except Exception:
                skipped += 1
    return imported, skipped


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="Companion dictionaries")
    ap.add_argument("--import-ir", metavar="DIR", help="bulk-import .ir files from a folder (e.g. Flipper-IRDB)")
    args = ap.parse_args()
    if args.import_ir:
        imp, skip = import_ir_tree(args.import_ir)
        print(f"imported {imp} .ir files, skipped {skip} -> {IR_DIR}")
        s = summary()
        print(f"now {len(s['ir_brands'])} brands, {s['ir_signals']} signals")
    else:
        import json
        print(json.dumps(summary(), indent=2))
