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
from textual.widgets import Header, Footer, Input, RichLog, Static

from companion_proto import Companion

DL_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "downloads")


class CompanionTUI(App):
    CSS = """
    #left { width: 38%; border-right: solid $primary; padding: 1; }
    #devinfo { color: $success; }
    #caps { color: $accent; height: auto; }
    #status { margin-top: 1; }
    #log { border: round $primary; }
    #cmd { dock: bottom; }
    """
    BINDINGS = [
        ("ctrl+q", "quit", "Quit"),
        ("ctrl+r", "refresh", "Status"),
        ("ctrl+l", "clear", "Clear log"),
    ]

    def __init__(self, port: str):
        super().__init__()
        self.port = port
        self.dev: Companion | None = None
        self._lock = asyncio.Lock()
        # last-rendered panel text (also handy for headless testing)
        self.last_devinfo = ""
        self.last_caps = ""
        self.last_status = ""

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        with Horizontal():
            with Vertical(id="left"):
                yield Static("connecting…", id="devinfo")
                yield Static("", id="caps")
                yield Static("", id="status")
                yield Static(
                    "\n[dim]console:[/dim] cmd · :status · :caps\n"
                    ":get <remote> [local] · :put <local> <remote> · :ls [p]",
                    id="hint",
                )
            with Vertical():
                yield RichLog(id="log", highlight=True, markup=True, wrap=True)
                yield Input(placeholder="command…  (e.g. free, ls /, :status)", id="cmd")
        yield Footer()

    def on_mount(self) -> None:
        self.query_one("#log", RichLog).write("[dim]opening %s…[/dim]" % self.port)
        self.connect()

    def write_log(self, msg: str) -> None:
        self.query_one("#log", RichLog).write(msg)

    @work(exclusive=True, group="dev")
    async def connect(self) -> None:
        try:
            async with self._lock:
                self.dev = await asyncio.to_thread(Companion, self.port)
                info = await asyncio.to_thread(self.dev.hello)
            if not info.get("ok"):
                self.query_one("#devinfo", Static).update("[red]HELLO failed[/red]")
                self.write_log("[red]HELLO failed[/red]")
                return
            self.last_devinfo = f"[b]{info.get('fw')}[/b]\n{info.get('board')}  mtu={info.get('mtu')}"
            self.last_caps = "caps: " + ", ".join(info.get("caps", []))
            self.query_one("#devinfo", Static).update(self.last_devinfo)
            self.query_one("#caps", Static).update(self.last_caps)
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
        except Exception as e:  # noqa: BLE001
            self.write_log(f"[red]error:[/red] {e}")

    def on_input_submitted(self, event: Input.Submitted) -> None:
        text = event.value.strip()
        event.input.value = ""
        if text:
            self.run_command(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM1")
    args = ap.parse_args()
    CompanionTUI(args.port).run()


if __name__ == "__main__":
    main()
