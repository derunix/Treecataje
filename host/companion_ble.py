#!/usr/bin/env python3
"""Companion wire-protocol client over BLE (Nordic-style GATT serial).

Transport-agnostic protocol identical to the USB path (docs/companion/protocol.md);
only the byte pipe differs. Uses bleak (BlueZ) as a GATT central.

Firmware (BLESerialService):
  service char  4371ec0b-3d43-49f9-b731-7c72a4a7bb91
  serial char   d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9  (WRITE | NOTIFY | READ)
  adv name      "Bruc"
One BLE write == one frame; each RSP line arrives as a notification.
"""
import asyncio
import itertools

from bleak import BleakClient, BleakScanner

from companion_proto import Frame, Response

SERVICE_UUID = "4371ec0b-3d43-49f9-b731-7c72a4a7bb91"
CHAR_UUID = "d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"
ADV_NAME = "Bruc"


class BleCompanion:
    def __init__(self, name=ADV_NAME, address=None, debug=False):
        self.name = name
        self.address = address
        self.debug = debug
        self.client: BleakClient | None = None
        self._ids = itertools.count(1)
        self._rxbuf = bytearray()
        self._queue: asyncio.Queue = asyncio.Queue()

    async def _scan(self):
        if self.address:
            return self.address
        if self.debug:
            print(f"[ble] scanning for name={self.name!r} / service={SERVICE_UUID} ...")
        # Match by advertised name first, then by service UUID.
        dev = await BleakScanner.find_device_by_name(self.name, timeout=10.0)
        if dev is None:
            def _flt(d, adv):
                u = [s.lower() for s in (adv.service_uuids or [])]
                return SERVICE_UUID in u or (d.name == self.name)
            dev = await BleakScanner.find_device_by_filter(_flt, timeout=10.0)
        if dev is None:
            raise RuntimeError(f"BLE device {self.name!r} not found (is 'companion ble on' active?)")
        return dev

    def _on_notify(self, _char, data: bytearray):
        self._rxbuf += data
        while b"\n" in self._rxbuf:
            raw, _, self._rxbuf = self._rxbuf.partition(b"\n")
            line = raw.decode(errors="replace")
            if self.debug:
                print(f"  << {line!r}")
            fr = Frame.parse(line)
            if fr:
                self._queue.put_nowait(fr)

    async def connect(self):
        dev = await self._scan()
        self.client = BleakClient(dev)
        await self.client.connect()
        await self.client.start_notify(CHAR_UUID, self._on_notify)
        if self.debug:
            print(f"[ble] connected mtu={getattr(self.client, 'mtu_size', '?')}")

    async def close(self):
        if self.client and self.client.is_connected:
            try:
                await self.client.stop_notify(CHAR_UUID)
            except Exception:
                pass
            await self.client.disconnect()

    async def _write(self, text: str):
        if self.debug:
            print(f"  >> {text!r}")
        await self.client.write_gatt_char(CHAR_UUID, text.encode(), response=True)

    async def request(self, cmd: str, timeout=6.0) -> Response:
        rid = next(self._ids)
        # drain stale frames
        while not self._queue.empty():
            self._queue.get_nowait()
        await self._write(f"REQ {rid} {cmd}\n")
        resp = Response(id=rid, ok=False, code=-1)
        loop = asyncio.get_event_loop()
        deadline = loop.time() + timeout
        while loop.time() < deadline:
            try:
                fr = await asyncio.wait_for(self._queue.get(), timeout=deadline - loop.time())
            except asyncio.TimeoutError:
                break
            if fr.type == "RAW":
                resp.lines.append(fr.payload)
                continue
            if fr.id != rid and fr.type in ("RSP", "END", "ERR"):
                continue
            if fr.type == "RSP":
                resp.lines.append(fr.payload)
            elif fr.type == "EVT":
                resp.events.append(fr.payload)
            elif fr.type == "END":
                resp.ok = True
                try:
                    resp.code = int(fr.payload.split()[0])
                except (ValueError, IndexError):
                    resp.code = 0
                return resp
            elif fr.type == "ERR":
                resp.error = fr.payload
                try:
                    resp.code = int(fr.payload.split()[0])
                except (ValueError, IndexError):
                    resp.code = -1
                return resp
        resp.error = "timeout"
        return resp

    async def hello(self, token="", timeout=6.0) -> dict:
        r = await self.request(f"HELLO proto=1 token={token}", timeout=timeout)
        info = {"ok": r.ok and r.code == 0, "raw": r}
        for line in r.lines:
            for tok in line.split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    info[k] = v
        if "caps" in info:
            info["caps"] = info["caps"].split(",")
        return info
