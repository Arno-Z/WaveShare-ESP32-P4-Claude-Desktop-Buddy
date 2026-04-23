#!/usr/bin/env python3
"""Fake Claude Desktop buddy — drives the ESP32-P4 face over BLE NUS.

Connects to the first peripheral named "Claude Face" (or the first
device exposing the NUS service), subscribes to TX notifications, and
lets you fire synthetic heartbeat snapshots from a single-key menu.

Run this while Claude Desktop's Hardware Buddy window is disconnected;
the device only accepts one BLE central at a time.

Usage:
    pip install bleak
    python3 tools/fake_buddy.py
"""

from __future__ import annotations

import asyncio
import json
import sys
from typing import Optional

from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_CHAR = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # write
NUS_TX_CHAR = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # notify

DEVICE_NAME = "Claude Face"


def snap(total=1, running=0, waiting=0, msg="", prompt: Optional[dict] = None,
         tokens: int = 0, tokens_today: int = 0, entries=()) -> dict:
    payload = {
        "total": total,
        "running": running,
        "waiting": waiting,
        "msg": msg,
        "entries": list(entries),
        "tokens": tokens,
        "tokens_today": tokens_today,
    }
    if prompt is not None:
        payload["prompt"] = prompt
    return payload


SCENARIOS = {
    "s": ("SLEEP (total=0)",            snap(total=0, msg="nothing open")),
    "i": ("IDLE",                       snap(total=3, msg="3 idle")),
    "b": ("BUSY (running=1)",           snap(total=3, running=1, msg="generating response", tokens=4200)),
    "a": ("ATTENTION + prompt: Bash",   snap(total=3, waiting=1, msg="approve: Bash",
                                             prompt={"id": "req_test01", "tool": "Bash", "hint": "uptime"})),
    "A": ("ATTENTION + prompt: Filesystem", snap(total=3, waiting=1, msg="approve: Filesystem",
                                                 prompt={"id": "req_fs02", "tool": "Filesystem",
                                                         "hint": "/Users/arno/Documents/*"})),
    "o": ("send owner Felix",           {"cmd": "owner", "name": "Felix"}),
    "n": ("send name Clawd",            {"cmd": "name",  "name": "Clawd"}),
    "t": ("send status query",          {"cmd": "status"}),
    "u": ("send unpair",                {"cmd": "unpair"}),
}

HELP = """\
Keys:
  s  → SLEEP snapshot (total=0)
  i  → IDLE snapshot
  b  → BUSY snapshot (running=1)
  a  → ATTENTION + permission prompt (Bash)
  A  → ATTENTION + permission prompt (Filesystem)
  o  → send {"cmd":"owner","name":"Felix"}
  n  → send {"cmd":"name","name":"Clawd"}
  t  → send {"cmd":"status"}
  u  → send {"cmd":"unpair"}
  ?  → show this help
  q  → quit
"""


async def find_device():
    print(f"scanning for '{DEVICE_NAME}' ...")
    devs = await BleakScanner.discover(timeout=6.0, service_uuids=[NUS_SERVICE])
    for d in devs:
        name = d.name or ""
        if name.startswith("Claude") or name == DEVICE_NAME:
            print(f"  found {name}  {d.address}")
            return d
    # Fallback: any device that advertises the service.
    if devs:
        d = devs[0]
        print(f"  nothing named Claude; defaulting to {d.name!r} @ {d.address}")
        return d
    return None


def on_notify(_char: BleakGATTCharacteristic, data: bytearray) -> None:
    try:
        text = bytes(data).decode("utf-8", errors="replace").rstrip("\n")
    except Exception:
        text = repr(bytes(data))
    print(f"  <- TX notify: {text}")


async def send_json(client: BleakClient, obj: dict) -> None:
    line = (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")
    # response=True so Core Bluetooth blocks on the ATT error returned
    # by the peripheral when it needs encryption — that's what makes
    # the macOS pairing dialog appear.
    await client.write_gatt_char(NUS_RX_CHAR, line, response=True)
    print(f"  -> RX write  ({len(line)}B): {line.rstrip().decode()}")


async def repl(client: BleakClient) -> None:
    print(HELP)
    loop = asyncio.get_running_loop()
    while True:
        try:
            key = await loop.run_in_executor(None, input, "> ")
        except (EOFError, KeyboardInterrupt):
            return
        key = key.strip()
        if not key:
            continue
        if key == "q":
            return
        if key == "?":
            print(HELP); continue
        entry = SCENARIOS.get(key)
        if not entry:
            print(f"  unknown key {key!r} — '?' for help"); continue
        label, obj = entry
        print(f"  [{label}]")
        try:
            await send_json(client, obj)
        except Exception as e:
            print(f"  write failed: {e}")


async def main() -> int:
    dev = await find_device()
    if not dev:
        print("no device found. Disconnect Claude Desktop's Hardware Buddy "
              "window and make sure the board is advertising.", file=sys.stderr)
        return 1
    async with BleakClient(dev) as client:
        print(f"connected. MTU={client.mtu_size if hasattr(client, 'mtu_size') else '?'}")
        await client.start_notify(NUS_TX_CHAR, on_notify)
        try:
            await repl(client)
        finally:
            try:
                await client.stop_notify(NUS_TX_CHAR)
            except Exception:
                pass
    print("disconnected.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        sys.exit(130)
