#!/usr/bin/env python3
"""Phase 0 bring-up probe.

Talks to the *existing* Bruce serial CLI over USB-CDC (legacy mode, no firmware
changes). Confirms the command surface on real hardware before any protocol work.

The legacy CLI (src/core/serialcmds.cpp:47-53):
  - reads a line terminated by '\n'
  - echoes "COMMAND: <line>" on Serial
  - runs the command (output via serialDevice->println)
  - prints "# " as a prompt, then forces a menu redraw (backToMenu)

So we send "<cmd>\n" and read until we see the "# " prompt (or a timeout).
"""
import sys
import time
import argparse

import serial  # pyserial


def read_until_prompt(ser, timeout=3.0, prompt=b"# "):
    """Read bytes until the trailing prompt appears or timeout elapses."""
    deadline = time.time() + timeout
    buf = bytearray()
    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            # Prompt is printed without newline at the very end of a response.
            if buf.endswith(prompt) or buf.endswith(prompt + b"\r\n"):
                break
        else:
            # brief idle; keep waiting until deadline
            time.sleep(0.02)
    return bytes(buf)


def send_cmd(ser, cmd, timeout=3.0):
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    ser.flush()
    return read_until_prompt(ser, timeout=timeout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM1")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--cmds", nargs="*", default=["uptime", "free", "status"])
    ap.add_argument("--timeout", type=float, default=3.0)
    args = ap.parse_args()

    print(f"[probe] opening {args.port} @ {args.baud}")
    # ESP32-S3 USB-Serial-JTAG: opening does not reset the chip.
    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    time.sleep(0.5)
    ser.reset_input_buffer()

    # Nudge with a bare newline to elicit a prompt / clear any partial state.
    ser.write(b"\n")
    ser.flush()
    banner = read_until_prompt(ser, timeout=1.0)
    if banner.strip():
        print("[probe] initial:\n" + banner.decode(errors="replace"))

    for cmd in args.cmds:
        print(f"\n===== > {cmd} =====")
        out = send_cmd(ser, cmd, timeout=args.timeout)
        print(out.decode(errors="replace"))

    ser.close()
    print("\n[probe] done")


if __name__ == "__main__":
    sys.exit(main())
