"""Real-bench backend for STM32 USB CDC + ZLG USBCAN + DWIN RS485.

This module is test-only and reuses the existing CAN simulators. It never
imports firmware production modules.
"""

from __future__ import annotations

import importlib.util
import json
import struct
import sys
import time
from pathlib import Path

from .contracts import CanFrame, DwinFrame, Observation, Snapshot, UsbFrame
from .transports import TransportError

ROOT = Path(__file__).resolve().parents[2]


def _load_legacy_simulator():
    path = ROOT / "test" / "zcan_hil_simulator.py"
    spec = importlib.util.spec_from_file_location("charger_legacy_zcan_sim", path)
    if spec is None or spec.loader is None:
        raise TransportError(f"cannot load CAN simulator: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _crc8(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (((crc << 1) ^ 0x07) & 0xFF
                   if crc & 0x80 else (crc << 1) & 0xFF)
    return crc


def _pc_frame(command: int, payload: bytes = b"") -> bytes:
    body = bytes((command, len(payload))) + payload
    return b"\xAA\x55" + body + bytes((_crc8(body),))


def _golden_config_frame() -> bytes:
    """Build SET_CHARGE_CFG using the repository's test-side Python model."""
    path = ROOT / "debug_app" / "protocol" / "debug_protocol.py"
    spec = importlib.util.spec_from_file_location("charger_debug_protocol", path)
    if spec is None or spec.loader is None:
        raise TransportError(f"cannot load config model: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    config_path = ROOT / "debug_app" / "charge_config_golden_path.json"
    values = json.loads(config_path.read_text(encoding="utf-8"))
    return _pc_frame(0x1A, module.ChargeCycleConfig(**values).to_bytes())


class ZcanHardwareBackend:
    def __init__(self, usb_port: str, dwin_port: str, driver: str = "tonhe", addr: int = 1):
        try:
            import serial
        except ImportError as exc:
            raise TransportError("pyserial is required for HIL backend") from exc
        self.serial = serial
        self.usb_port, self.dwin_port = usb_port, dwin_port
        self.driver, self.addr = driver, addr
        self.usb = self.dwin = self.dev = self.bms = self.module = None
        self._usb_frames = []
        self._usb_buf = bytearray()
        self._can_frames = []
        self._dwin_frames = []
        self._dwin_buf = bytearray()

    def preflight(self) -> None:
        legacy = _load_legacy_simulator()
        try:
            self.usb = self.serial.Serial(self.usb_port, 115200, timeout=0.05)
            self.dwin = self.serial.Serial(self.dwin_port, 115200, timeout=0.05)
            self.dev = legacy.ZlgCanDevice()
            self.dev.open()
            self.dev.init_channel(0, 125000)
            self.dev.init_channel(1, 250000)
            self.bms = legacy.BmsSimulator(self.dev)
            self.module = legacy.ModuleSimulator(self.dev, self.driver, self.addr)
            self.bms.start(); self.module.start()
        except Exception as exc:
            self.close()
            raise TransportError(f"HIL preflight failed: {exc}") from exc

        self.usb.reset_input_buffer()
        self.usb.write(_pc_frame(0x06))
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            self._poll_usb()
            if any(x.command == 0x84 for x in self._usb_frames):
                break
        else:
            raise TransportError("MCU did not answer PING during preflight")

        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            self._poll_dwin()
            if self._dwin_frames:
                return
        raise TransportError("DWIN did not produce a frame during preflight")

    def configure(self, payload: bytes) -> None:
        if payload == b"test-config":
            payload = _golden_config_frame()
        if not payload:
            raise TransportError("empty configuration payload")
        self.usb.write(payload); self.usb.flush()
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            self._poll_usb()
            if any(x.command in (0x82, 0x97) for x in self._usb_frames):
                return
        raise TransportError("USB configuration response was not received")

    def reset(self) -> None:
        """Reset simulator inputs; MCU state is reset by the scenario itself."""
        if self.bms is not None:
            self.bms.transmitting = True
            self.bms.fault_high_cell_volt = 0
        if self.module is not None:
            self.module.transmitting = True
            self.module.fault_bits = 0
            self.module.actually_on = False

    def dwin_touch(self, key: int) -> None:
        frame = bytes((0xA5, 0x5A, 0x06, 0x83, 0x10, 0x43, 0x01,
                       (key >> 8) & 0xFF, key & 0xFF))
        self.dwin.write(frame); self.dwin.flush(); self._poll_dwin()

    def inject_bms(self, **values) -> None:
        if self.bms is None:
            raise TransportError("BMS simulator is not running")
        for key, value in values.items():
            if key == "soc": self.bms.soc_pct = int(value)
            elif key == "high_cell_fault":
                self.bms.fault_high_cell_volt = 2 if value else 0
                self.bms.max_cell_mv = 3680 if value else 3315
            elif key == "transmitting": self.bms.transmitting = bool(value)

    def inject_module(self, **values) -> None:
        if self.module is None:
            raise TransportError("module simulator is not running")
        for key, value in values.items():
            if key == "ac_undervoltage": self.module.fault_bits = 0x0001 if value else 0
            elif key == "transmitting": self.module.transmitting = bool(value)
            elif hasattr(self.module, key): setattr(self.module, key, value)

    def _poll_usb(self) -> None:
        self._usb_buf.extend(self.usb.read(512))
        buf = self._usb_buf
        while len(buf) >= 5:
            if buf[:2] != b"\xAA\x55": del buf[0]; continue
            length, total = buf[3], 5 + buf[3]
            if len(buf) < total: break
            self._usb_frames.append(UsbFrame(int(time.monotonic() * 1000), buf[2],
                                             bytes(buf[4:4 + length]), "rx"))
            del buf[:total]

    def _wait_usb_command(self, command: int, timeout: float) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self._poll_usb()
            if any(x.command == command for x in self._usb_frames): return
        raise TransportError(f"USB response 0x{command:02X} not received")

    def _poll_dwin(self) -> None:
        self._dwin_buf.extend(self.dwin.read(512))
        buf = self._dwin_buf
        while len(buf) >= 6:
            if buf[:2] != b"\xA5\x5A": del buf[0]; continue
            length, total = buf[2], 3 + buf[2]
            if len(buf) < total: break
            self._dwin_frames.append(DwinFrame(int(time.monotonic() * 1000), buf[3],
                                               (buf[4] << 8) | buf[5],
                                               bytes(buf[6:total]), "rx"))
            del buf[:total]

    def observe(self) -> Observation:
        self._poll_usb(); self._poll_dwin()
        if self.dev is not None:
            for channel in (0, 1):
                for can_id, extended, data in self.dev.receive(channel, max_count=64, wait_ms=0):
                    self._can_frames.append(CanFrame(int(time.monotonic() * 1000), channel,
                                                     can_id, data, extended, "bus"))
        snap = Snapshot()
        for frame in reversed(self._dwin_frames):
            if frame.vp == 0x1041 and len(frame.payload) >= 2:
                snap.dwin_status = struct.unpack(">H", frame.payload[:2])[0]; break
            if frame.vp == 0x1044:
                snap.dwin_code = frame.payload.rstrip(b"\0").decode("latin1", "ignore")
        return Observation(list(self._usb_frames), list(self._can_frames),
                           list(self._dwin_frames), snap)

    def close(self) -> None:
        for obj in (self.bms, self.module):
            if obj is not None: obj.running = False
        if self.dev is not None:
            try: self.dev.close()
            except Exception: pass
        for obj in (self.usb, self.dwin):
            if obj is not None:
                try: obj.close()
                except Exception: pass
