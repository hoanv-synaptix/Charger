"""Test-only transport interfaces and a deterministic dry-run backend."""

from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Callable, List, Optional, Protocol

from .contracts import CanFrame, DwinFrame, Observation, Snapshot, UsbFrame


class TransportError(RuntimeError):
    pass


class ClosedLoopBackend(Protocol):
    def preflight(self) -> None: ...
    def reset(self) -> None: ...
    def configure(self, payload: bytes) -> None: ...
    def pc_start(self) -> None: ...
    def pc_stop(self) -> None: ...
    def dwin_touch(self, key: int) -> None: ...
    def inject_bms(self, **values) -> None: ...
    def inject_module(self, **values) -> None: ...
    def set_sources_online(self, bms: bool, module: bool) -> None: ...
    def observe(self) -> Observation: ...
    def close(self) -> None: ...


def _now_ms() -> int:
    return int(time.monotonic() * 1000)


@dataclass
class DryRunBackend:
    """Deterministic backend for validating runner/report mechanics.

    It deliberately models only observable events.  Production behavior is
    still owned by the firmware and must be verified with the real HIL backend.
    """

    ready: bool = True
    configured: bool = False
    running: bool = False
    fault: bool = False
    dwin_status: int = 0
    dwin_button: int = 0
    bms_soc: int = 82
    module_voltage: float = 0.0
    module_current: float = 0.0
    bms_online: bool = True
    module_online: bool = True
    _usb: List[UsbFrame] = None
    _can: List[CanFrame] = None
    _dwin: List[DwinFrame] = None

    def __post_init__(self):
        self._usb = []
        self._can = []
        self._dwin = []

    def preflight(self) -> None:
        if not self.ready:
            raise TransportError("dry-run backend is not ready")
        self._usb.append(UsbFrame(_now_ms(), 0x84, b"\x02\x00\x00", "rx"))

    def reset(self) -> None:
        self.configured = False
        self.running = False
        self.fault = False
        self.dwin_status = 0
        self.dwin_button = 0
        self.bms_soc = 82
        self.module_voltage = 0.0
        self.module_current = 0.0
        self.bms_online = True
        self.module_online = True

    def set_sources_online(self, bms: bool, module: bool) -> None:
        self.bms_online = bool(bms)
        self.module_online = bool(module)

    def configure(self, payload: bytes) -> None:
        if len(payload) == 0:
            raise TransportError("empty configuration")
        self.configured = True
        self._usb.append(UsbFrame(_now_ms(), 0x82, b"\x09", "rx"))

    def pc_start(self) -> None:
        if not self.configured or not self.bms_online or not self.module_online:
            raise TransportError("dry-run PC START precondition failed")
        self.running = True
        self.dwin_status = 2
        self.dwin_button = 1
        self._can.append(CanFrame(_now_ms(), 0, 0, b"START", True, "tx"))

    def pc_stop(self) -> None:
        was_running = self.running
        self.running = False
        self.dwin_status = 0
        self.dwin_button = 0
        if was_running:
            self._can.append(CanFrame(_now_ms(), 0, 0, b"STOP", True, "tx"))

    def dwin_touch(self, key: int) -> None:
        now = _now_ms()
        self._dwin.append(DwinFrame(now, 0x83, 0x1043, key.to_bytes(2, "big"), "tx"))
        if key == 1 and self.configured and self.dwin_status == 3:
            self.dwin_status = 0
            self.dwin_button = 0
        elif key == 1 and self.configured and self.fault:
            self.fault = False
            self.dwin_status = 0
            self.dwin_button = 0
        elif key == 1 and self.configured:
            self.running = not self.running
            self.dwin_status = 2 if self.running else 0
            self.dwin_button = 1 if self.running else 0
            cmd = b"START" if self.running else b"STOP"
            self._can.append(CanFrame(now, 0, 0, cmd, True, "tx"))

    def inject_bms(self, **values) -> None:
        if "soc" in values:
            self.bms_soc = int(values["soc"])
            if self.bms_soc >= 100 and self.running and not self.fault:
                self.running = False
                self.dwin_status = 3
                self.dwin_button = 2
                self._can.append(CanFrame(_now_ms(), 0, 0, b"STOP", True, "tx"))
        if values.get("high_cell_fault"):
            self.fault = True
            self.running = False
            self.dwin_status = 4
            self.dwin_button = 2
            self._dwin.append(DwinFrame(_now_ms(), 0x82, 0x1044, b"E004", "rx"))
        if "soc" in values and not 0 <= self.bms_soc <= 100:
            self.bms_online = True

    def inject_module(self, **values) -> None:
        if "voltage" in values:
            self.module_voltage = float(values["voltage"])
        if "current" in values:
            self.module_current = float(values["current"])
        if values.get("ac_undervoltage"):
            self.fault = True
            self.running = False
            self.dwin_status = 4
            self.dwin_button = 2
            self._dwin.append(DwinFrame(_now_ms(), 0x82, 0x1044, b"E026", "rx"))

    def observe(self) -> Observation:
        soc_text = "--%"
        soc_color = 0x8410
        if self.bms_online and 0 <= self.bms_soc <= 100:
            soc_text = f"{self.bms_soc}%"
            if self.bms_soc <= 10:
                soc_color = 0xF800
            elif self.bms_soc <= 30:
                soc_color = 0xFD20
            elif self.bms_soc <= 60:
                soc_color = 0xFFE0
            else:
                soc_color = 0x07E0
        snap = Snapshot(
            controller_state=("FAULT" if self.fault else
                              ("COMPLETE" if self.dwin_status == 3 else
                               ("RUNNING" if self.running else "READY"))),
            stop_reason="BMS_ALARM" if self.fault else None,
            fault_flags=1 if self.fault else 0,
            bms_state="ONLINE" if self.bms_online else "OFFLINE",
            bms_soc=self.bms_soc if self.bms_online else None,
            modules_online=1 if self.module_online else 0,
            module_state="FAULT" if self.fault else ("RUNNING" if self.running else "IDLE"),
            module_voltage=self.module_voltage if self.module_online else None,
            module_current=self.module_current if self.module_online else None,
            dwin_status=self.dwin_status,
            dwin_button=self.dwin_button,
            dwin_soc_text=soc_text,
            dwin_soc_color=soc_color,
        )
        return Observation(list(self._usb), list(self._can), list(self._dwin), snap)

    def close(self) -> None:
        pass
