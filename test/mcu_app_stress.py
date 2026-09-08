#!/usr/bin/env python3
"""Test-only MCU emulator for stressing the PC application's RX path.

Use with an OS virtual COM pair, for example COM1 <-> COM2:
    app     -> COM1
    emulator -> COM2

The payloads match the Python debug protocol parser layouts. This is not a
firmware simulator and must not be used to claim MCU safety behavior.
"""

from __future__ import annotations

import argparse
import struct
import threading
import time

import serial


def crc8(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def frame(command: int, payload: bytes = b"") -> bytes:
    body = bytes((command, len(payload))) + payload
    return b"\xAA\x55" + body + bytes((crc8(body),))


def system_info(tick: int) -> bytes:
    # DebugSystemInfo_t: 14 bytes, 7 floats, 7 uint32, 2 bytes.
    head = bytes((1, 0, 0, 3, 1, 1, 0, 1, 2, 0, 0, 0, 0, 0))
    values = struct.pack("<7f", 53.5, 24.5, 1311.0, 38.0, 53.5, 24.5, 0.5)
    counters = struct.pack("<7I", tick, tick, tick, tick, tick, 0, 0)
    return head + values + counters + bytes((0, 0))


def module_data(tick: int, soc_fault: bool = False) -> bytes:
    # DebugModuleData_t, 123 bytes, packed little-endian.
    head = bytes((0, 3, 1, 1, 1, 5 if soc_fault else 2))
    floats = struct.pack("<9f", 53.5, 24.5, 100.0, 35.0, 28.0, 38.0,
                         221.0, 222.0, 220.5)
    bus = struct.pack("<2f", 380.0, 380.0)
    input_power = struct.pack("<I", 1311)
    rated = struct.pack("<2f", 10000.0, 100.0)
    alarms = struct.pack("<2I", 0, 0x1 if soc_fault else 0)
    tail = bytes((0, 1, 0)) + struct.pack("<7I", tick, tick, tick, tick, tick, 0, 0)
    vendor = bytes((0,)) + bytes(21)
    payload = head + floats + bus + input_power + rated + alarms + tail + vendor
    assert len(payload) == 123, len(payload)
    return payload


def bms_data(tick: int, soc: int = 50) -> bytes:
    return struct.pack("<BBBBffffBBHHffffII", 1, 1, 1, 0,
                       53.5, 24.5, 50.0, 100.0, soc, 100,
                       3315, 3300, 28.0, 26.0, 54.6, 30.0, 0, tick)


def drain_rx(port: serial.Serial, stop: threading.Event, counter: list[int]) -> None:
    while not stop.is_set():
        data = port.read(port.in_waiting or 1)
        if data:
            counter[0] += len(data)


def main() -> int:
    parser = argparse.ArgumentParser(description="Flood a PC app with valid MCU telemetry")
    parser.add_argument("--port", required=True, help="emulator endpoint of a virtual COM pair")
    parser.add_argument("--duration", type=float, default=60.0)
    parser.add_argument("--rate", type=float, default=20.0,
                        help="telemetry cycles per second (default: 20)")
    args = parser.parse_args()
    if args.duration <= 0 or args.rate <= 0:
        parser.error("duration and rate must be positive")

    stop = threading.Event()
    rx_bytes = [0]
    sent = 0
    tick = 0
    with serial.Serial(args.port, 115200, timeout=0.02, write_timeout=1.0) as port:
        reader = threading.Thread(target=drain_rx, args=(port, stop, rx_bytes), daemon=True)
        reader.start()
        deadline = time.monotonic() + args.duration
        period = 1.0 / args.rate
        next_tick = time.monotonic()
        try:
            while time.monotonic() < deadline:
                tick += 20
                port.write(frame(0x94, system_info(tick)))
                port.write(frame(0x91, bytes((sent & 0xFF, 1)) + module_data(tick)))
                port.write(frame(0x93, bms_data(tick, 50 + ((sent // 20) % 2))))
                sent += 1
                next_tick += period
                time.sleep(max(0.0, next_tick - time.monotonic()))
        finally:
            stop.set()
            reader.join(timeout=1.0)
    print(f"MCU emulator complete: cycles={sent}, tx_frames={sent * 3}, rx_bytes={rx_bytes[0]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
