#!/usr/bin/env python3
"""
Download IEEE OUI registries and produce a single lookup file for Bruce.

Output format:
    AABBCC,Vendor Name
"""
from __future__ import annotations

import csv
import pathlib
import sys
import urllib.request

SOURCES = [
    "https://standards-oui.ieee.org/oui/oui.csv",
    "https://standards-oui.ieee.org/oui28/mam.csv",
    "https://standards-oui.ieee.org/oui36/oui36.csv",
]


def normalize_prefix(raw: str) -> str:
    out = "".join(ch for ch in raw.upper() if ch in "0123456789ABCDEF")
    return out[:6] if len(out) >= 6 else ""


def read_csv_from_url(url: str) -> list[dict[str, str]]:
    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": "Mozilla/5.0 (Bruce OUI fetcher)",
            "Accept": "text/csv,text/plain,*/*",
        },
    )
    with urllib.request.urlopen(req, timeout=60) as resp:
        data = resp.read().decode("utf-8", errors="replace")
    return list(csv.DictReader(data.splitlines()))


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[1]
    out_dir = root / "sd_files" / "wifi"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_file = out_dir / "oui.csv"

    vendor_by_prefix: dict[str, str] = {}

    for src in SOURCES:
        print(f"[fetch] {src}")
        rows = read_csv_from_url(src)
        for row in rows:
            assignment = row.get("Assignment", "")
            org = row.get("Organization Name", "").strip()
            prefix = normalize_prefix(assignment)
            if not prefix or not org:
                continue
            if prefix not in vendor_by_prefix:
                vendor_by_prefix[prefix] = org

    lines = [f"{p},{vendor_by_prefix[p]}" for p in sorted(vendor_by_prefix.keys())]
    out_file.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"[ok] wrote {len(lines)} entries to {out_file}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
