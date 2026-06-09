#!/usr/bin/env python3
"""Headless test of the TUI WiFi-attack picker (no device): a scan populates the
DataTable and row selection updates the chosen target.

  host/.venv/bin/python host/tui_attack_test.py
"""
import asyncio
import sys
from types import SimpleNamespace

from textual.widgets import DataTable

from tui import CompanionTUI


class FakeDev:
    def scan_aps(self, secs=6.0, rounds=1):
        return [{"bssid": "AA:BB:CC:00:00:01", "ch": 6, "ssid": "HomeNet", "rssi": -40, "enc": "wpa2"},
                {"bssid": "AA:BB:CC:00:00:02", "ch": 11, "ssid": "Cafe", "rssi": -71, "enc": "wpa/wpa2"}]


async def main():
    app = CompanionTUI("/dev/null")
    ok = True
    async with app.run_test() as pilot:
        await pilot.pause(0.3)
        app.dev = FakeDev()
        app.scan(5.0)
        for _ in range(30):
            await pilot.pause(0.1)
            if app._aps:
                break
        tbl = app.query_one("#aps", DataTable)
        ok &= (tbl.row_count == 2)
        print(f"  [{'PASS' if tbl.row_count == 2 else 'FAIL'}] scan populated table ({tbl.row_count} rows)")
        c1 = bool(app._target and app._target["ssid"] == "HomeNet")
        ok &= c1
        print(f"  [{'PASS' if c1 else 'FAIL'}] target auto-selected ({app._target and app._target['ssid']})")
        app.on_data_table_row_highlighted(SimpleNamespace(cursor_row=1))
        c2 = app._target["ssid"] == "Cafe"
        ok &= c2
        print(f"  [{'PASS' if c2 else 'FAIL'}] row selection updates target ({app._target['ssid']})")
    print("\n" + ("ALL PASS (tui attack picker)" if ok else "SOME FAILURES"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
