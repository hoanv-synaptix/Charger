"""Shared test-side contracts for USB, CAN and DWIN observations."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Dict, List, Optional


class TestStatus(str, Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    BLOCKED = "BLOCKED"
    ERROR = "ERROR"


@dataclass(frozen=True)
class CanFrame:
    timestamp_ms: int
    channel: int
    can_id: int
    data: bytes
    extended: bool
    direction: str


@dataclass(frozen=True)
class UsbFrame:
    timestamp_ms: int
    command: int
    payload: bytes
    direction: str


@dataclass(frozen=True)
class DwinFrame:
    timestamp_ms: int
    command: int
    vp: int
    payload: bytes
    direction: str


@dataclass
class Snapshot:
    """Latest observable state, intentionally independent of production types."""

    controller_state: Optional[str] = None
    stop_reason: Optional[str] = None
    fault_flags: int = 0
    bms_state: Optional[str] = None
    bms_soc: Optional[int] = None
    modules_online: Optional[int] = None
    module_state: Optional[str] = None
    module_voltage: Optional[float] = None
    module_current: Optional[float] = None
    dwin_status: Optional[int] = None
    dwin_button: Optional[int] = None
    dwin_code: Optional[str] = None
    dwin_soc: Optional[int] = None
    extras: Dict[str, Any] = field(default_factory=dict)


@dataclass
class Observation:
    usb: List[UsbFrame] = field(default_factory=list)
    can: List[CanFrame] = field(default_factory=list)
    dwin: List[DwinFrame] = field(default_factory=list)
    snapshot: Snapshot = field(default_factory=Snapshot)


@dataclass
class ScenarioResult:
    scenario_id: str
    name: str
    status: TestStatus
    duration_ms: int
    expected: Dict[str, Any] = field(default_factory=dict)
    observed: Dict[str, Any] = field(default_factory=dict)
    events: List[str] = field(default_factory=list)
    error: Optional[str] = None

