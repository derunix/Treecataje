#!/usr/bin/env python3
"""Shared catalog of device CLI commands, grouped by capability.

One source of truth consumed by BOTH the GUI (buttons) and the TUI (menu items),
so every firmware function is exposed without retyping raw commands. Each command
declares its argument list; the front-ends render inputs and build the final line
via build_command().

Command kinds:
  oneshot      quick, returns immediately
  long         long-running / fixed serial timeout (rx/scan, tone, play)
  blocking     runs until ESC on the device — may not return over the link
  danger       destructive / state-changing → front-ends confirm first
"""
from dataclasses import dataclass, field


@dataclass
class Arg:
    name: str                 # short field label
    placeholder: str = ""     # hint text / example
    default: str = ""
    required: bool = True
    choices: tuple = ()       # non-empty -> dropdown


@dataclass
class Cmd:
    label: str                # button / menu label
    template: str             # base command; full = template + " " + args + suffix
    args: list = field(default_factory=list)
    kind: str = "oneshot"     # oneshot | long | blocking | danger
    desc: str = ""
    timeout: float = 8.0
    suffix: str = ""          # fixed trailing token(s) appended after the args


def build_command(cmd: Cmd, values):
    """Join template + provided arg values (+ suffix) into a full CLI line.
    `values` is a list of strings aligned with cmd.args. Raises ValueError if a
    required arg is empty."""
    parts = [cmd.template]
    for arg, val in zip(cmd.args, values):
        val = (val or "").strip()
        if not val:
            if arg.required:
                raise ValueError(f"'{arg.name}' is required")
            continue
        parts.append(val)
    if cmd.suffix:
        parts.append(cmd.suffix)
    return " ".join(parts)


# ── catalog ────────────────────────────────────────────────────────────────
GROUPS = [
 ("Status / Info", [
    Cmd("System status", "status", desc="Battery, radio, SD, WiFi/BLE"),
    Cmd("Free memory", "free", desc="Heap / PSRAM usage"),
    Cmd("Uptime", "uptime"),
    Cmd("Date / time", "date"),
    Cmd("Device info", "info", desc="FW version, SDK, MAC, IP"),
    Cmd("I2C scan", "i2c", desc="List I2C bus devices"),
    Cmd("Capabilities", "companion caps"),
    Cmd("Radio owner / busy", "companion busy"),
    Cmd("Menu options (JSON)", "optionsJSON"),
 ]),
 ("WiFi", [
    Cmd("WiFi on", "wifi on", kind="long", desc="Connect to known network / AP", timeout=20),
    Cmd("WiFi off", "wifi off"),
    Cmd("Add network", "wifi add",
        [Arg("ssid", "SSID"), Arg("password", "password", required=False)],
        desc="Store a network credential"),
    Cmd("Web UI", "webui", kind="blocking", desc="Start web server (ESC on device to stop)", timeout=12),
    Cmd("Web UI (no AP)", "webui --noAp", kind="blocking", timeout=12),
    Cmd("ARP host scan", "arp", kind="blocking", desc="Scan LAN hosts", timeout=20),
    Cmd("TCP listen", "listen", kind="blocking", timeout=12),
    Cmd("Packet sniffer", "sniffer", kind="blocking", desc="Raw 802.11 sniffer (ESC on device)", timeout=12),
 ]),
 ("Sub-GHz (RF / CC1101)", [
    Cmd("Receive + decode", "rf rx", [Arg("freq MHz", "433.92", "433.92")],
        kind="long", desc="Listen ~10s and decode", timeout=15),
    Cmd("Receive raw", "rf rx", [Arg("freq MHz", "433.92", "433.92")],
        kind="long", desc="Raw capture", timeout=15, suffix="--raw"),
    Cmd("Scan band", "rf scan", [Arg("start MHz", "433", "433"), Arg("stop MHz", "434", "434")],
        kind="long", desc="Scan a frequency range", timeout=15),
    Cmd("Transmit", "rf tx",
        [Arg("key (hex)", "445533"), Arg("freq Hz", "433920000", "433920000"),
         Arg("te", "174", "174"), Arg("count", "10", "10")],
        kind="long", desc="Send a raw key"),
    Cmd("TX from .sub file", "rf tx_from_file", [Arg("path", "/plug1_on.sub")], kind="long"),
    Cmd("RfSend (JSON)", "RfSend",
        [Arg("json", '{"Data":"0x447503","Bits":24,"Protocol":1,"Pulse":174,"Repeat":10}')],
        desc="Tasmota-style send"),
 ]),
 ("Infrared (IR)", [
    Cmd("Receive + decode", "ir rx", kind="long", desc="Listen ~10s and decode", timeout=15),
    Cmd("Receive raw", "ir rx --raw", kind="long", timeout=15),
    Cmd("Transmit", "ir tx",
        [Arg("protocol", "NEC", "NEC"), Arg("address", "04000000"), Arg("command", "08000000")]),
    Cmd("TX raw", "ir tx_raw", [Arg("freq Hz", "38000", "38000"), Arg("samples", "9000 4500 ...")]),
    Cmd("TX from .ir file", "ir tx_from_file", [Arg("path", "/LG_power.ir")], kind="long"),
    Cmd("IRSend (JSON)", "IRSend", [Arg("json", '{"Protocol":"NEC","Bits":32,"Data":"0x20DF10EF"}')]),
    Cmd("IR LED off", "ir"),
 ]),
 ("NRF24 (2.4 GHz)", [
    Cmd("Scan devices", "nrf scan", [Arg("time ms", "3000", "3000", required=False)],
        kind="long", desc="Sniff NRF24 addresses", timeout=15),
    Cmd("Jam sweep", "nrf jam_sweep",
        [Arg("start", "1", "1"), Arg("stop", "80", "80"), Arg("step", "2", "2"),
         Arg("dwell ms", "60", "60"), Arg("noise 0/1", "0", "0")],
        kind="danger", desc="Active jamming — use responsibly/legally"),
 ]),
 ("GPIO", [
    Cmd("Pin mode", "gpio mode",
        [Arg("pin", "25"), Arg("mode", "1", choices=("0", "1", "2", "3", "4", "5"))],
        desc="0=INPUT 1=OUTPUT 2=PULLUP …"),
    Cmd("Write pin", "gpio set", [Arg("pin", "25"), Arg("value", "1", choices=("0", "1"))]),
    Cmd("Read pin", "gpio read", [Arg("pin", "25")]),
 ]),
 ("Storage / Files", [
    Cmd("List dir", "ls", [Arg("path", "/", "/", required=False)]),
    Cmd("Cat file", "cat", [Arg("path", "/bruce.conf")]),
    Cmd("Stat", "storage stat", [Arg("path", "/bruce.conf")]),
    Cmd("Free space", "storage free", [Arg("where", "sd", "sd", choices=("sd", "littlefs"))]),
    Cmd("MD5", "md5", [Arg("path", "/bruce.conf")]),
    Cmd("CRC32", "crc32", [Arg("path", "/bruce.conf")]),
    Cmd("Make dir", "mkdir", [Arg("path", "/newdir")]),
    Cmd("Rename", "storage rename", [Arg("path", "/a.txt"), Arg("newName", "/b.txt")]),
    Cmd("Copy", "storage copy", [Arg("path", "/a.txt"), Arg("newName", "/b.txt")]),
    Cmd("Remove file", "rm", [Arg("path", "/old.txt")], kind="danger"),
    Cmd("Remove dir", "rmdir", [Arg("path", "/olddir")], kind="danger"),
 ]),
 ("GPS / GNSS", [
    Cmd("Fix status", "gps_status"),
    Cmd("Satellites", "gps_sats"),
    Cmd("Device info", "gps_info"),
    Cmd("Source", "gps_source", [Arg("driver", "casic", choices=("legacy", "casic"))]),
    Cmd("Baud", "gps_baud", [Arg("baud", "9600", "9600")]),
    Cmd("Update rate", "gps_rate", [Arg("ms", "1000", "1000")]),
    Cmd("GNSS system", "gps_system", [Arg("mode", "3", choices=tuple(str(i) for i in range(1, 8)))]),
    Cmd("NMEA version", "gps_nmea", [Arg("ver", "41")]),
    Cmd("Reset", "gps_reset", [Arg("type", "warm", choices=("hot", "warm", "cold", "factory"))], kind="danger"),
    Cmd("Save config", "gps_save"),
    Cmd("Logging", "gps_log", [Arg("state", "on", choices=("on", "off"))]),
    Cmd("Monitor (gpsmon)", "gpsmon", [Arg("state", "on", choices=("on", "off"), required=False)]),
    Cmd("Web viewer", "gps_web", [Arg("state", "status", choices=("on", "off", "status"))], kind="long"),
 ]),
 ("Crypto", [
    Cmd("Decrypt file", "crypto decrypt_from_file", [Arg("path", "/secret.enc"), Arg("password", "pwd")]),
    Cmd("Type from file (HID)", "crypto type_from_file", [Arg("path", "/secret.enc"), Arg("password", "pwd")],
        kind="long", desc="Decrypt and type over USB HID"),
 ]),
 ("BadUSB", [
    Cmd("Run Ducky file", "badusb run_from_file", [Arg("path", "/HelloWorld.txt")],
        kind="long", desc="Execute a Ducky script"),
 ]),
 ("Scripts (JS)", [
    Cmd("Run JS file", "js run_from_file", [Arg("path", "/script.js")], kind="long"),
 ]),
 ("Sound", [
    Cmd("Tone / beep", "tone", [Arg("freq Hz", "1000", "1000"), Arg("ms", "300", "300")], kind="long"),
    Cmd("Play file / RTTTL", "play", [Arg("song", "/boot.wav")], kind="long"),
    Cmd("Say (TTS)", "tts", [Arg("text", "hello")], kind="long"),
 ]),
 ("Screen", [
    Cmd("Show clock", "clock"),
    Cmd("Brightness", "screen brightness", [Arg("0-255", "128", "128")]),
    Cmd("UI color (RGB)", "screen color rgb", [Arg("r", "255"), Arg("g", "0"), Arg("b", "255")]),
    Cmd("UI color (hex)", "screen color hex", [Arg("hex", "ff00ff")]),
 ]),
 ("Navigation (remote control)", [
    Cmd("◀ Prev", "nav prev"),
    Cmd("▶ Next", "nav next"),
    Cmd("▲ Up", "nav up"),
    Cmd("▼ Down", "nav down"),
    Cmd("● Select", "nav select"),
    Cmd("Esc / back", "nav esc"),
    Cmd("Page ▲", "nav prevpage"),
    Cmd("Page ▼", "nav nextpage"),
    Cmd("List apps", "loader list"),
    Cmd("Open app", "loader open", [Arg("app", "badusb")], kind="long"),
    Cmd("Run menu option", "options", [Arg("index", "2", required=False)]),
 ]),
 ("Settings", [
    Cmd("View all settings", "settings"),
    Cmd("View one", "settings", [Arg("name", "priColor")]),
    Cmd("Set value", "settings", [Arg("name", "priColor"), Arg("value", "65535")]),
    Cmd("Companion token: status", "companion token status"),
    Cmd("Companion token: set", "companion token set", [Arg("token", "secret")], kind="danger",
        desc="After this, the token is required on USB and BLE"),
    Cmd("Companion token: clear", "companion token clear", kind="danger"),
    Cmd("Factory reset", "factory_reset", kind="danger", desc="Wipe all settings"),
 ]),
 ("Power", [
    Cmd("Sleep", "sleep", kind="long"),
    Cmd("Reboot", "reboot", kind="danger", desc="Restarts the device (link drops)"),
    Cmd("Power off", "poweroff", kind="danger", desc="Deep sleep — needs hardware reset to wake"),
 ]),
]


def all_groups():
    return GROUPS
