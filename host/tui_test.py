#!/usr/bin/env python3
"""Headless smoke test of the TUI via Textual's Pilot (no interactive terminal).

Mounts the app, waits for connect + status, checks the function Tree is built,
runs a command via the console AND via a tree menu item, verifies both.
"""
import sys
import asyncio
import argparse
from types import SimpleNamespace

from textual.widgets import Tree

from tui import CompanionTUI


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    args = ap.parse_args()

    app = CompanionTUI(args.port)
    ok = True
    async with app.run_test() as pilot:
        for _ in range(60):
            await pilot.pause(0.25)
            if "T_EMBED" in app.last_devinfo:
                break
        for _ in range(40):
            await pilot.pause(0.25)
            if "Battery" in app.last_status:
                break

        # function tree built + populated
        tree = app.query_one("#cmds", Tree)
        groups = list(tree.root.children)
        leaves = sum(len(g.children) for g in groups)
        print(f"tree: {len(groups)} groups, {leaves} commands")
        ok = ok and len(groups) >= 10 and leaves >= 50

        # run a command through the smart console
        app.run_command("free")
        await pilot.pause(3.0)
        ok = ok and app.last_cmd_result and app.last_cmd_result[1] == 0
        print("console 'free' ->", app.last_cmd_result)

        # run a no-arg command by selecting its tree leaf ("Free memory" -> free)
        app.last_cmd_result = None
        target = None
        for g in groups:
            for leaf in g.children:
                if getattr(leaf.data, "template", None) == "free":
                    target = leaf
        ok = ok and target is not None
        app.on_tree_node_selected(SimpleNamespace(node=target))
        await pilot.pause(3.0)
        ok = ok and app.last_cmd_result and app.last_cmd_result[1] == 0
        print("tree 'Free memory' ->", app.last_cmd_result)

        devinfo, status, caps = app.last_devinfo, app.last_status, app.last_caps
        print("devinfo:", repr(devinfo[:80]))
        print("status :", repr(status[:80]))
        print("caps   :", repr(caps[:80]))
        ok = ok and ("T_EMBED" in devinfo) and ("Battery" in status) and ("wifi" in caps)
        print("\nTUI smoke:", "PASS" if ok else "FAIL")
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
