"""Machine-readable test reports and raw evidence capture."""

from __future__ import annotations

import json
import os
import time
import xml.etree.ElementTree as ET
from dataclasses import asdict
from pathlib import Path
from typing import Iterable, List

from .contracts import CanFrame, DwinFrame, ScenarioResult, TestStatus, UsbFrame


def _frame_dict(frame):
    item = asdict(frame)
    for key, value in item.items():
        if isinstance(value, bytes):
            item[key] = value.hex(" ")
    return item


class Evidence:
    def __init__(self, root: str | os.PathLike[str] = "test/artifacts"):
        stamp = time.strftime("%Y%m%d_%H%M%S")
        self.path = Path(root) / stamp
        self.path.mkdir(parents=True, exist_ok=True)
        self.results: List[ScenarioResult] = []

    def add(self, result: ScenarioResult) -> None:
        self.results.append(result)

    def write_frames(self, usb: Iterable[UsbFrame], can: Iterable[CanFrame],
                     dwin: Iterable[DwinFrame]) -> None:
        streams = {
            "usb_raw.json": [_frame_dict(x) for x in usb],
            "can_raw.json": [_frame_dict(x) for x in can],
            "dwin_raw.json": [_frame_dict(x) for x in dwin],
        }
        for name, data in streams.items():
            (self.path / name).write_text(json.dumps(data, indent=2), encoding="utf-8")

    def write(self) -> Path:
        summary = {
            "status": "PASS" if all(x.status == TestStatus.PASS for x in self.results) else "FAIL",
            "results": [asdict(x) for x in self.results],
        }
        (self.path / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")

        suite = ET.Element("testsuite", name="charger_closed_loop", tests=str(len(self.results)))
        for result in self.results:
            case = ET.SubElement(suite, "testcase", name=result.scenario_id,
                                 classname=result.name, time=f"{result.duration_ms / 1000:.3f}")
            if result.status != TestStatus.PASS:
                node = ET.SubElement(case, "failure" if result.status == TestStatus.FAIL else "error",
                                     message=result.error or result.status.value)
                node.text = json.dumps({"expected": result.expected, "observed": result.observed}, indent=2)
        ET.ElementTree(suite).write(self.path / "junit.xml", encoding="utf-8", xml_declaration=True)
        return self.path
