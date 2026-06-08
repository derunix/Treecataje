# Companion host tooling

Linux-side tooling for the Treecataje companion device mode. See `../docs/companion/` for the full spec.

Current state: **Phases 0–5 done** — framed protocol over USB **and** BLE, file transfer, async streaming, host-compute analysis, **MCP server**, **TUI**, and **GUI**. Remaining: Phase 6 (token auth + larger BLE MTU), real wifi/nrf stream kinds. See `../docs/companion/roadmap.md`.

## Layout

| File | What |
|---|---|
| `companion_proto.py` | Pure-Python client of the wire protocol (USB). Frame codec + request/response + HELLO. Robust to unframed (raw `Serial`) lines. |
| `phase0_probe.py` | Phase 0 bring-up: talks to the legacy CLI (`# ` prompt) to confirm the command surface. No firmware deps. |
| `phase1_test.py` | Phase 1 acceptance test: HELLO → caps, status round-trip with clean `END`, companion verbs. |
| `mcp_server.py` | MCP server (stdio) exposing the device to Claude: `device_connect/info/status/caps/busy/run/file_get/file_put/analyze/disconnect`. |
| `companion_ble.py` | BLE GATT-central transport (bleak); same protocol as USB. |
| `phase2_ble_test.py` / `phase3_file_test.py` | BLE handshake test / file-transfer (get+put round-trip) test. |
| `tui.py` | Interactive TUI (Textual): connect (USB **or** BLE via `--transport ble`), status panel, a **function tree** (16 groups / ~90 commands + **IR dictionary** signals & **RFID keys** deploy — select to run or prefill args) + smart console (`:get`/`:put`/`:status`/cmd). Run: `python tui.py --port /dev/ttyACM0` or `--transport ble`. |
| `tui_test.py` | Headless TUI smoke test (Textual Pilot). |
| `companion_commands.py` | Shared catalog of ~90 device CLI commands in 16 capability groups (label, args, kind). One source of truth for the GUI buttons and TUI menu items. `build_command()` assembles the line. |
| `companion_dicts.py` + `dictionaries/` | Curated, device-compatible reference data: IR signal DBs (`ir/*.ir`, Flipper format), MIFARE key dicts (`rfid/*.keys`), sub-GHz captures (`subghz/*.sub`). Parsed for the GUI **Dictionaries** tab and MCP (`device_dict_list`/`device_ir_send`/`device_deploy_keys`). IR sends via `ir tx`; files deploy to `/BruceIR`·`/BruceRF`·`/BruceRFID/keys.conf`. Bulk-import a Flipper-IRDB clone: `python companion_dicts.py --import-ir <dir>`. GUI tree has a search filter. |
| `gui.py` | Desktop GUI (PySide6) over the same core: connection bar (usb/ble + token), device/status panels, tabs **Functions / Console / Files / Stream / Analyze**. Functions = group list + a button (with arg fields) for every command; danger commands confirm. Files = a **device file browser** (navigate dirs, download/view/delete) + manual get/put. Stream = telemetry/wifi/nrf/rf with live analysis + **Save/Load** captures. Console has **↑/↓ history**; status panel has an **auto-refresh (2s)** toggle. All device I/O on one worker thread. Run: `python gui.py --port /dev/ttyACM0`. |
| `launchers/` | App-menu launchers (`.desktop` + wrappers) for the GUI/TUI. `bash launchers/install.sh` adds **Treecataje Companion (GUI/TUI)** to the menu and `companion-gui`/`companion-tui` to `~/.local/bin` (auto-detect the serial port). |
| `gui_test.py` | Headless GUI smoke test (Qt `offscreen`) — drives the real worker/signal path against the device. `--no-device` for UI-only. |
| `companion_compute.py` | Host-compute: analyze device captures (NRF24 scan, battery CSV, pcap, generic) with ASCII viz. `python companion_compute.py --pull /nrf_scan.log`. |
| `requirements.txt` / `.venv/` | deps: `pyserial`, `bleak`, `mcp`, `textual`, `PySide6`. |

## Quick use

```bash
# Phase 0 (legacy CLI probe)
python3 phase0_probe.py --port /dev/ttyACM1

# Phase 1 (framed protocol)
python3 phase1_test.py --port /dev/ttyACM1 --debug

# one-off command via the protocol
python3 -c "from companion_proto import Companion; c=Companion('/dev/ttyACM1'); print(c.hello()); print(c.request('free').lines)"
```

## Device port

The ESP32-S3 target is the port that answers `HELLO` (VID:PID `0x303a:0x1001`). It usually enumerates as `/dev/ttyACM1`, but the index can flip to `/dev/ttyACM0` after a replug/reboot, while the uConsole's own MCU takes the other index. If a tool gets silence, try the other port (or grep `dmesg` for the latest `ttyACM* USB ACM device`). Quick check: the target replies to `REQ 1 HELLO proto=1 token=`.

## MCP server

Registered in `../.mcp.json` (project scope). Claude Code launches it over stdio; tools appear after Claude reconnects (and approves the project MCP server). The server connects to the device **lazily** (first tool call), so it won't grab the serial port until used. Don't run the test scripts and the MCP server against the device at the same time — the serial port is exclusive.

Auth token: empty by default (open/lab). Set `COMPANION_TOKEN` in `.mcp.json` and on the device once token auth is enforced (see `../docs/companion/security.md`).

**Transport over BLE:** `device_connect(transport="ble")`. The device must be advertising first — enable it over USB once:
`device_connect(transport="usb")` → `device_run("companion ble on")` → `device_disconnect()`, then `device_connect(transport="ble")`. Over BLE use a small `chunk` (≤192) for `device_file_get/put` (notify size). `device_run("companion ble off")` (over BLE) returns the device to USB.

## Roadmap toward the chosen stack

The chosen architecture is a **Rust core** (`companion_core`, via pyo3) consumed by Python TUI/GUI/MCP. Right now the core is the pure-Python `companion_proto` (fast to iterate, proves the protocol). The MCP server and future front-ends import the core through one seam, so swapping in the Rust core later is localized. Building the Rust core needs a Rust toolchain (not yet installed).
