#!/usr/bin/env python3
"""Headless smoke test of the TUI via Textual's Pilot (no interactive terminal).

Mounts the app, waits for the device to connect + status to populate, runs a
command, and checks the panels updated.
"""
import sys
import asyncio

from tui import CompanionTUI


async def main():
    app = CompanionTUI("/dev/ttyACM1")
    async with app.run_test() as pilot:
        for _ in range(60):
            await pilot.pause(0.25)
            if "T_EMBED" in app.last_devinfo:
                break
        for _ in range(40):
            await pilot.pause(0.25)
            if "Battery" in app.last_status:
                break
        # run a command through the smart console
        app.run_command("free")
        await pilot.pause(3.0)

        devinfo, status, caps = app.last_devinfo, app.last_status, app.last_caps
        print("devinfo:", devinfo.replace("\n", " | "))
        print("status :", status.replace("\n", " | "))
        ok = ("T_EMBED" in devinfo) and ("Battery" in status) and ("wifi" in caps)
        print("\nTUI smoke:", "PASS" if ok else "FAIL")
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
