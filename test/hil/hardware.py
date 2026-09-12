"""Real-bench backend for STM32 USB CDC + ZLG USBCAN + DWIN RS485.

This module is test-only and reuses the existing CAN simulators. It never
imports firmware production modules.
"""

from __future__ import annotations

import importlib.util
import json
import re
import struct
import sys
import time
from pathlib import Path

from .contracts import CanFrame, DwinFrame, Observation, Snapshot, UsbFrame
from .transports import TransportError

ROOT = Path(__file__).resolve().parents[2]


def _parse_dwin_number(payload: bytes):
    text = payload.rstrip(b"\0").decode("latin1", "ignore").strip()
    match = re.match(r"^-?\d+(?:\.\d+)?", text)
    return float(match.group(0)) if match else None


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
    # The checked-in JSON is a v3 golden fixture. The running controller
    # rejects older versions at its active-config check, even though the
    # config setter accepts them for migration. Use the current wire version
    # for closed-loop start/stop scenarios.
    values["version"] = 6
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
        self._case_start_ms = 0

    def preflight(self) -> None:
        legacy = _load_legacy_simulator()
        try:
            self.usb = self.serial.Serial(self.usb_port, 115200, timeout=0.05)
            self.dwin = self.serial.Serial(self.dwin_port, 115200, timeout=0.05)
            self.dev = legacy.ZlgCanDevice()
            self.dev.open()
            self.dev.init_channel(0, 125000)
            self.dev.init_channel(1, 250000)
            self._record_can_transmit()
            self._record_can_receive()
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

    def _send_pc_command(self, command: int, payload: bytes = b"") -> None:
        self.usb.write(_pc_frame(command, payload)); self.usb.flush()
        start = len(self._usb_frames)
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            self._poll_usb()
            if any(x.command in (0x82, 0x83) for x in self._usb_frames[start:]):
                return
        raise TransportError(f"USB command 0x{command:02X} response was not received")

    def pc_start(self) -> None:
        self._send_pc_command(0x03, b"\x00")

    def pc_stop(self) -> None:
        self._send_pc_command(0x04)

    def reset(self) -> None:
        """Restore simulator inputs and establish a clean MCU test baseline."""
        if self.bms is not None:
            self.bms.transmitting = True
            self.bms.fault_high_cell_volt = 0
            self.bms.fault_low_cell_volt = 0
            self.bms.fault_high_pack_volt = 0
            self.bms.fault_low_pack_volt = 0
            self.bms.fault_over_temp = 0
            self.bms.soc_pct = 82
            self.bms.pack_voltage_v = 52.8
            self.bms.pack_current_a = 0.0
            self.bms.max_cell_mv = 3315
            self.bms.min_cell_mv = 3300
        if self.module is not None:
            self.module.transmitting = True
            self.module.fault_bits = 0
            self.module.actually_on = False
            self.module.voltage = 0.0
            self.module.current = 0.0
        # Deterministic teardown; unlike a DWIN touch this does not depend on
        # a stale button/status frame from the previous scenario.
        if self.usb is not None:
            try:
                self.pc_stop()
            except TransportError:
                pass
        # Keep the streams for the final evidence report. Scenario assertions
        # use _case_start_ms instead of destructively clearing observations.
        self._case_start_ms = 0
        if self.usb is not None:
            self.usb.reset_input_buffer()
        if self.dwin is not None:
            self.dwin.reset_input_buffer()
        time.sleep(1.1)
        self._poll_dwin()
        latest_status = None
        latest_code = None
        latest_button = None
        for frame in reversed(self._dwin_frames):
            if latest_status is None and frame.vp == 0x1041 and len(frame.payload) >= 2:
                latest_status = struct.unpack(">H", frame.payload[:2])[0]
            if latest_code is None and frame.vp == 0x1044:
                latest_code = frame.payload.rstrip(b"\0").decode("latin1", "ignore")
            if latest_button is None and frame.vp == 0x1042 and len(frame.payload) >= 2:
                latest_button = struct.unpack(">H", frame.payload[:2])[0]
            if latest_status is not None and latest_code is not None and latest_button is not None:
                break
        if (latest_status in (1, 2, 3, 4) or latest_button == 1 or
                (latest_code not in (None, "", "0000", "----"))):
            self.dwin_touch(1)
            time.sleep(2.0)
            self._poll_dwin()
            # A latched alarm may require a second acknowledge after the
            # controller has completed its STOP sequence.
            latest_status = None
            latest_code = None
            for frame in reversed(self._dwin_frames):
                if latest_status is None and frame.vp == 0x1041 and len(frame.payload) >= 2:
                    latest_status = struct.unpack(">H", frame.payload[:2])[0]
                if latest_code is None and frame.vp == 0x1044:
                    latest_code = frame.payload.rstrip(b"\0").decode("latin1", "ignore")
                if latest_status is not None and latest_code is not None:
                    break
            if latest_status in (3, 4) or (latest_code not in (None, "", "0000", "----")):
                self.dwin_touch(1)
                time.sleep(1.5)
                self._poll_dwin()
        # Converge the panel to READY before opening the next case. This is
        # deliberately bounded; an unresponsive panel is a test blocker, not
        # a reason to continue with contaminated state.
        for _ in range(3):
            state = self.observe().snapshot.dwin_status
            if state == 0:
                break
            if state in (1, 2, 3, 4):
                self.dwin_touch(1)
                time.sleep(2.0)
                self._poll_dwin()
        self._case_start_ms = int(time.monotonic() * 1000)

    def _record_can_transmit(self) -> None:
        original = self.dev.transmit

        def transmit(channel, can_id, data, extended=False):
            result = original(channel, can_id, data, extended)
            if result:
                self._can_frames.append(CanFrame(int(time.monotonic() * 1000),
                                                 channel, can_id, bytes(data),
                                                 extended, "zcan-tx"))
            return result

        self.dev.transmit = transmit

    def _record_can_receive(self) -> None:
        original = self.dev.receive

        def receive(channel, max_count=32, wait_ms=0):
            frames = original(channel, max_count, wait_ms)
            now = int(time.monotonic() * 1000)
            for can_id, extended, data in frames:
                self._can_frames.append(CanFrame(now, channel, can_id,
                                                 bytes(data), extended,
                                                 "mcu-rx"))
            return frames

        self.dev.receive = receive

    def set_sources_online(self, bms: bool, module: bool) -> None:
        if self.bms is None or self.module is None:
            raise TransportError("HIL simulators are not running")
        self.bms.transmitting = bool(bms)
        self.module.transmitting = bool(module)

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
        snap = Snapshot()
        current = [x for x in self._can_frames if x.timestamp_ms >= self._case_start_ms]
        module_frames = [x for x in current if x.channel == 0 and x.direction == "zcan-tx"]
        bms_frames = [x for x in current if x.channel == 1 and x.direction == "zcan-tx"]
        now_ms = int(time.monotonic() * 1000)
        # A source is online only while its most recent simulator frame is
        # fresh. Looking at whether *any* frame existed in the case made an
        # offline test accidentally pass/fail based on stale history.
        module_fresh = (module_frames and
                        (now_ms - module_frames[-1].timestamp_ms) <= 500)
        bms_fresh = (bms_frames and
                     (now_ms - bms_frames[-1].timestamp_ms) <= 500)
        snap.modules_online = 1 if module_fresh else 0
        snap.bms_state = "ONLINE" if bms_fresh else "OFFLINE"
        for frame in reversed(bms_frames):
            if bms_fresh and frame.can_id == 0x02F4 and len(frame.data) >= 5:
                snap.bms_soc = frame.data[4]
                break
        for frame in reversed(module_frames):
            if module_fresh and frame.extended and ((frame.can_id >> 16) & 0xFF) == 0x01 and len(frame.data) >= 5:
                snap.module_voltage = ((frame.data[1] | (frame.data[2] << 8)) * 0.1)
                snap.module_current = ((frame.data[3] | (frame.data[4] << 8)) * 0.01)
                break
        current_dwin = [x for x in self._dwin_frames if x.timestamp_ms >= self._case_start_ms]
        for frame in reversed(current_dwin):
            if frame.vp == 0x1041 and len(frame.payload) >= 2:
                if snap.dwin_status is None:
                    snap.dwin_status = struct.unpack(">H", frame.payload[:2])[0]
            elif frame.vp == 0x1044 and snap.dwin_code is None:
                snap.dwin_code = frame.payload.rstrip(b"\0").decode("latin1", "ignore")
        if snap.dwin_status is None:
            # A quiet READY panel does not retransmit its status every cycle.
            # Use the last observed status only as a baseline fallback; all
            # alarm/text assertions remain restricted to the current case.
            for frame in reversed(self._dwin_frames):
                if frame.vp == 0x1041 and len(frame.payload) >= 2:
                    snap.dwin_status = struct.unpack(">H", frame.payload[:2])[0]
                    break
        if snap.dwin_status == 0:
            snap.controller_state = "READY"
        elif snap.dwin_status == 1:
            snap.controller_state = "STARTING"
        elif snap.dwin_status == 2:
            snap.controller_state = "CHARGING"
        elif snap.dwin_status == 3:
            snap.controller_state = "COMPLETE"
        elif snap.dwin_status == 4:
            snap.controller_state = "FAULT"
        for frame in reversed(current_dwin):
            if frame.vp == 0x1042 and len(frame.payload) >= 2:
                snap.dwin_button = struct.unpack(">H", frame.payload[:2])[0]
                break
        for frame in reversed(current_dwin):
            if frame.vp == 0x1048:
                snap.dwin_soc_text = frame.payload.rstrip(b"\0").decode("latin1", "ignore")
                match = re.match(r"^(-?\d+)", snap.dwin_soc_text)
                if match:
                    snap.dwin_soc = int(match.group(1))
                break
            if frame.vp == 0x1000:
                snap.module_voltage = _parse_dwin_number(frame.payload)
            elif frame.vp == 0x1004:
                snap.module_current = _parse_dwin_number(frame.payload)
        for frame in reversed(current_dwin):
            if frame.vp == 0x8003 and len(frame.payload) >= 2:
                snap.dwin_soc_color = struct.unpack(">H", frame.payload[:2])[0]
                break
        snap.extras["case_start_ms"] = self._case_start_ms
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
