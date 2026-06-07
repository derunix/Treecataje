# Companion host tooling

Linux-side tooling for the Treecataje companion device mode. See `../docs/companion/` for the full spec.

Current state: **Phase 1** (framed protocol over USB) + **MCP server**. BLE transport, TUI, GUI come later (see `../docs/companion/roadmap.md`).

## Layout

| File | What |
|---|---|
| `companion_proto.py` | Pure-Python client of the wire protocol (USB). Frame codec + request/response + HELLO. Robust to unframed (raw `Serial`) lines. |
| `phase0_probe.py` | Phase 0 bring-up: talks to the legacy CLI (`# ` prompt) to confirm the command surface. No firmware deps. |
| `phase1_test.py` | Phase 1 acceptance test: HELLO → caps, status round-trip with clean `END`, companion verbs. |
| `mcp_server.py` | MCP server (stdio) exposing the device to Claude: `device_connect/info/status/caps/busy/run/file_get/file_put/analyze/disconnect`. |
| `companion_ble.py` | BLE GATT-central transport (bleak); same protocol as USB. |
| `phase2_ble_test.py` / `phase3_file_test.py` | BLE handshake test / file-transfer (get+put round-trip) test. |
| `tui.py` | Interactive TUI (Textual): connect, status panel, smart console (`:get`/`:put`/`:status`/cmd). Run: `python tui.py --port /dev/ttyACM1`. |
| `tui_test.py` | Headless TUI smoke test (Textual Pilot). |
| `companion_compute.py` | Host-compute: analyze device captures (NRF24 scan, battery CSV, pcap, generic) with ASCII viz. `python companion_compute.py --pull /nrf_scan.log`. |
| `requirements.txt` / `.venv/` | deps: `pyserial`, `bleak`, `mcp`, `textual`. |

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

`/dev/ttyACM1` = the ESP32-S3 target (VID:PID `0x303a:0x1001`). `/dev/ttyACM0` is the uConsole's own MCU — leave it alone.

## MCP server

Registered in `../.mcp.json` (project scope). Claude Code launches it over stdio; tools appear after Claude reconnects (and approves the project MCP server). The server connects to the device **lazily** (first tool call), so it won't grab the serial port until used. Don't run the test scripts and the MCP server against the device at the same time — the serial port is exclusive.

Auth token: empty by default (open/lab). Set `COMPANION_TOKEN` in `.mcp.json` and on the device once token auth is enforced (see `../docs/companion/security.md`).

## Roadmap toward the chosen stack

The chosen architecture is a **Rust core** (`companion_core`, via pyo3) consumed by Python TUI/GUI/MCP. Right now the core is the pure-Python `companion_proto` (fast to iterate, proves the protocol). The MCP server and future front-ends import the core through one seam, so swapping in the Rust core later is localized. Building the Rust core needs a Rust toolchain (not yet installed).
