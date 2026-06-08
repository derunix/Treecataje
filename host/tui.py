#!/usr/bin/env python3
"""Interactive TUI for the Treecataje companion device (Phase 4).

Thin front-end over the shared core (companion_proto). USB transport for now;
BLE is a drop-in via companion_ble later.

Smart console (single input box):
  <anything>             run it as a device CLI command (REQ)
  :status                refresh the status panel
  :caps                  show capabilities
  :get <remote> [local]  download a device file (sha256-verified)
  :put <local> <remote>  upload a host file
  :ls [path]             list a device directory (alias for "ls <path>")
Keys: ctrl+r refresh status · ctrl+l clear log · ctrl+q quit
"""
import os
import asyncio
import argparse

from textual import work
from textual.app import App, ComposeResult
from textual.containers import Horizontal, Vertical
from textual.widgets import Header, Footer, Input, RichLog, Static, Tree

from companion_proto import Companion
import companion_commands as cc

DL_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "downloads")


class CompanionTUI(App):
    CSS = """
    #left { width: 42%; border-right: solid $primary; padding: 1; }
    #devinfo { color: $success; height: auto; }
    #status { color: $accent; height: auto; margin-bottom: 1; }
    #cmds { height: 1fr; border: round $primary; }
    #log { border: round $primary; }
    #cmd { dock: bottom; }
    """
    BINDINGS = [
        ("ctrl+q", "quit", "Quit"),
        ("ctrl+r", "refresh", "Status"),
        ("ctrl+l", "clear", "Clear log"),
    ]

    def __init__(self, port: str, token: str = "", transport: str = "usb", name: str = "Bruc"):
        super().__init__()
        self.port = port
        self.token = token
        self.transport = transport
        self.ble_name = name
        self.dev = None
        self._lock = asyncio.Lock()
        # last-rendered panel text (also handy for headless testing)
        self.last_devinfo = ""
        self.last_caps = ""
        self.last_status = ""
        self.last_cmd_result = None  # (ok, code, lines) — for headless testing

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        with Horizontal():
            with Vertical(id="left"):
                yield Static("connecting…", id="devinfo")
                yield Static("", id="status")
                yield Tree("Functions", id="cmds")
            with Vertical():
                yield RichLog(id="log", highlight=True, markup=True, wrap=True)
                yield Input(placeholder="pick a function ↖ or type a command…", id="cmd")
        yield Footer()

    def on_mount(self) -> None:
        self.query_one("#log", RichLog).write("[dim]opening %s…[/dim]" % self.port)
        self._build_tree()
        self.connect()

    def _build_tree(self) -> None:
        tree = self.query_one("#cmds", Tree)
        tree.root.expand()
        tree.show_root = False
        for name, cmds in cc.all_groups():
            node = tree.root.add(name)
            for cmd in cmds:
                node.add_leaf(cmd.label, data=cmd)
        self._add_dict_nodes(tree)

    def _add_dict_nodes(self, tree) -> None:
        """Append the host dictionaries (IR signals, RFID keys) as tree items."""
        try:
            import companion_dicts as cdi
        except Exception:
            return
        by_brand = {}
        for e in cdi.ir_entries():
            by_brand.setdefault(e["brand"], []).append(e)
        if by_brand:
            ir = tree.root.add("IR dictionary")
            for brand in sorted(by_brand):
                bnode = ir.add(brand)
                for e in by_brand[brand]:
                    line = cdi.ir_tx_line(e)
                    if line:
                        bnode.add_leaf(e["name"], data=cc.Cmd(f"{brand} {e['name']}", line,
                                                              desc="IR dictionary send"))
        if cdi.key_files():
            rf = tree.root.add("RFID keys")
            rf.add_leaf("Deploy default keys.conf", data={"action": "deploy_keys"})

    def on_tree_node_selected(self, event: Tree.NodeSelected) -> None:
        cmd = event.node.data
        if cmd is None:  # a group header: toggle expand
            return
        if isinstance(cmd, dict):  # special dictionary action
            if cmd.get("action") == "deploy_keys":
                self.deploy_keys()
            return
        inp = self.query_one("#cmd", Input)
        if not cmd.args and cmd.kind != "danger":
            # safe, no-arg command -> run immediately
            self.write_log(f"[dim]{cmd.desc}[/dim]" if cmd.desc else "")
            self.run_command(cmd.template + (" " + cmd.suffix if cmd.suffix else ""))
            return
        # needs args or is destructive -> prefill the input, let the user confirm
        defaults = " ".join(a.default for a in cmd.args).strip()
        line = cmd.template + (" " + defaults if defaults else " ")
        if cmd.suffix and not cmd.args:
            line = cmd.template + " " + cmd.suffix
        inp.value = line
        inp.focus()
        argnames = " ".join(f"<{a.name}>" for a in cmd.args)
        warn = "[red]⚠ destructive — [/red]" if cmd.kind == "danger" else ""
        self.write_log(f"{warn}[cyan]{cmd.label}[/cyan]: {argnames or '(no args)'}  "
                       f"— Enter to run" + (f"  [dim]({cmd.desc})[/dim]" if cmd.desc else ""))

    def write_log(self, msg: str) -> None:
        self.query_one("#log", RichLog).write(msg)

    @work(exclusive=True, group="dev")
    async def connect(self) -> None:
        try:
            async with self._lock:
                if self.transport == "ble":
                    from mcp_server import BleSync
                    self.write_log("[dim]scanning BLE '%s'… (needs 'companion ble on')[/dim]" % self.ble_name)
                    self.dev = await asyncio.to_thread(lambda: BleSync(name=self.ble_name, token=self.token))
                    info = self.dev.info
                else:
                    self.dev = await asyncio.to_thread(Companion, self.port)
                    info = await asyncio.to_thread(self.dev.hello, self.token)
            if not info.get("ok"):
                self.query_one("#devinfo", Static).update("[red]HELLO failed[/red]")
                self.write_log("[red]HELLO failed[/red]")
                return
            self.last_caps = ", ".join(info.get("caps", []))
            self.last_devinfo = (f"[b]{info.get('fw')}[/b]\n{info.get('board')}  mtu={info.get('mtu')}\n"
                                 f"[dim]caps:[/dim] {self.last_caps}")
            self.query_one("#devinfo", Static).update(self.last_devinfo)
            self.write_log("[green]connected[/green] " + str(info.get("fw")))
            self.action_refresh()
        except Exception as e:  # noqa: BLE001
            self.write_log(f"[red]connect error:[/red] {e}")

    async def _request(self, cmd: str, timeout=8.0):
        async with self._lock:
            return await asyncio.to_thread(self.dev.request, cmd, timeout)

    @work(group="dev")
    async def action_refresh(self) -> None:
        if not self.dev:
            return
        r = await self._request("status")
        self.last_status = "[b]status[/b]\n" + ("\n".join(r.lines) if r.lines else "(none)")
        self.query_one("#status", Static).update(self.last_status)

    def action_clear(self) -> None:
        self.query_one("#log", RichLog).clear()

    @work(group="dev")
    async def run_command(self, text: str) -> None:
        if not self.dev:
            self.write_log("[red]not connected[/red]")
            return
        try:
            if text in (":status",):
                self.action_refresh()
                return
            if text in (":caps",):
                r = await self._request("companion caps")
                self.write_log("[cyan]caps[/cyan] " + " ".join(r.lines))
                return
            if text.startswith(":get "):
                parts = text.split()
                remote = parts[1]
                os.makedirs(DL_DIR, exist_ok=True)
                local = parts[2] if len(parts) > 2 else os.path.join(DL_DIR, os.path.basename(remote))
                self.write_log(f"[yellow]get[/yellow] {remote} …")
                out = await asyncio.to_thread(self.dev.file_get, remote, local, 512, 60)
                self.write_log(f"[green]saved[/green] {local} ({out['size']} B, sha {out['sha256'][:12]}…)")
                return
            if text.startswith(":put "):
                parts = text.split()
                local, remote = parts[1], parts[2]
                self.write_log(f"[yellow]put[/yellow] {local} -> {remote} …")
                out = await asyncio.to_thread(self.dev.file_put, local, remote, 512, 60)
                self.write_log(f"[green]put ok={out['ok']}[/green] sha {out['sha256'][:12]}…")
                return
            if text.startswith(":ls"):
                text = "ls " + (text[3:].strip() or "/")
            # default: device command
            self.write_log(f"[bold]> {text}[/bold]")
            r = await self._request(text)
            for line in r.lines:
                self.write_log("  " + line)
            tag = "green" if r.ok and r.code == 0 else "red"
            self.write_log(f"[{tag}]· END code={r.code}{' ' + r.error if r.error else ''}[/{tag}]")
            self.last_cmd_result = (r.ok, r.code, list(r.lines))
        except Exception as e:  # noqa: BLE001
            self.write_log(f"[red]error:[/red] {e}")

    @work(group="dev")
    async def deploy_keys(self) -> None:
        if not self.dev:
            self.write_log("[red]not connected[/red]")
            return
        try:
            import companion_dicts as cdi
            import tempfile
            text = cdi.build_keys_conf(cdi.key_files())
            tmp = os.path.join(tempfile.gettempdir(), "keys.conf")
            with open(tmp, "w") as fh:
                fh.write(text)
            n = sum(1 for ln in text.splitlines() if not ln.startswith("//"))
            self.write_log(f"[yellow]deploy[/yellow] {n} keys -> {cdi.DEV_RFID_KEYS} …")
            async with self._lock:
                out = await asyncio.to_thread(self.dev.file_put, tmp, cdi.DEV_RFID_KEYS, 512, 60)
            self.write_log(f"[green]keys.conf deployed[/green] ok={out['ok']}")
        except Exception as e:  # noqa: BLE001
            self.write_log(f"[red]deploy error:[/red] {e}")

    def on_input_submitted(self, event: Input.Submitted) -> None:
        text = event.value.strip()
        event.input.value = ""
        if text:
            self.run_command(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM1")
    ap.add_argument("--token", default=os.environ.get("COMPANION_TOKEN", ""),
                    help="companion auth token (required if the device has one set)")
    ap.add_argument("--transport", default="usb", choices=("usb", "ble"),
                    help="usb (serial) or ble (enable it on the device first via 'companion ble on')")
    ap.add_argument("--name", default="Bruc", help="BLE advertised name")
    args = ap.parse_args()
    CompanionTUI(args.port, args.token, args.transport, args.name).run()


if __name__ == "__main__":
    main()
