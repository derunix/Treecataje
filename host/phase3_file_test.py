#!/usr/bin/env python3
"""Phase 3 acceptance test — file transfer over USB.

  1. file_get an existing device file (/bruce.conf) -> host, sha256 verified.
  2. file_put a generated blob -> device, then file_get it back and byte-compare.
"""
import os
import sys
import hashlib
import argparse

from companion_proto import Companion


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))
    return cond


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM1")
    ap.add_argument("--remote-get", default="/bruce.conf")
    args = ap.parse_args()

    c = Companion(args.port)
    h = c.hello()
    ok = check("HELLO", h.get("ok"))

    print("\n== file_get (device -> host) ==")
    got = c.file_get(args.remote_get, "/tmp/companion_get.bin")
    ok &= check("download + sha256 ok", got["size"] > 0, f"size={got['size']} sha={got['sha256'][:16]}…")

    print("\n== file_put round-trip (host -> device -> host) ==")
    blob = bytes((i * 37 + 11) & 0xFF for i in range(2500))  # 2500 bytes, non-trivial
    src = "/tmp/companion_put_src.bin"
    with open(src, "wb") as fh:
        fh.write(blob)
    remote = "/companion_rt.bin"
    put = c.file_put(src, remote)
    ok &= check("put ok (device sha verified)", put["ok"], " ".join(put["lines"])[:80])

    back = c.file_get(remote, "/tmp/companion_rt_back.bin")
    ok &= check("round-trip bytes identical",
                back["data"] == blob,
                f"{len(back['data'])} vs {len(blob)} bytes")
    ok &= check("round-trip sha256 matches",
                back["sha256"] == hashlib.sha256(blob).hexdigest())

    # cleanup on device
    c.request(f"rm {remote}")
    c.close()
    print("\n" + ("ALL PASS (files)" if ok else "SOME FAILURES (files)"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
