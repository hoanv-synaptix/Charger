"""Scenario runner with strict preflight and observable assertions."""

from __future__ import annotations

import argparse
import json
import time
from dataclasses import dataclass
from typing import Any, Callable, Dict, List

from .contracts import Observation, ScenarioResult, TestStatus
from .report import Evidence
from .transports import ClosedLoopBackend, DryRunBackend, TransportError


class AssertionFailure(AssertionError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionFailure(message)


@dataclass(frozen=True)
class Scenario:
    scenario_id: str
    name: str
    execute: Callable[[ClosedLoopBackend], Dict[str, Any]]


def scenario_config_and_start(backend: ClosedLoopBackend) -> Dict[str, Any]:
    backend.configure(b"test-config")
    backend.dwin_touch(1)
    obs = backend.observe()
    require(obs.snapshot.controller_state == "RUNNING", "DWIN START did not produce RUNNING")
    require(any(x.data == b"START" for x in obs.can), "MCU START CAN command was not observed")
    return {"controller_state": "RUNNING", "dwin_status": 2}


def scenario_bms_fault(backend: ClosedLoopBackend) -> Dict[str, Any]:
    backend.configure(b"test-config")
    backend.dwin_touch(1)
    backend.inject_bms(high_cell_fault=True)
    obs = backend.observe()
    require(obs.snapshot.controller_state == "FAULT", "BMS fault did not reach controller")
    require(obs.snapshot.dwin_code == "E004" or any(x.payload == b"E004" for x in obs.dwin),
            "DWIN E004 was not observed")
    return {"controller_state": "FAULT", "dwin_code": "E004"}


def scenario_module_fault(backend: ClosedLoopBackend) -> Dict[str, Any]:
    backend.configure(b"test-config")
    backend.dwin_touch(1)
    backend.inject_module(ac_undervoltage=True)
    obs = backend.observe()
    require(obs.snapshot.controller_state == "FAULT", "module fault did not reach controller")
    require(any(x.payload == b"E026" for x in obs.dwin), "DWIN E026 was not observed")
    return {"controller_state": "FAULT", "dwin_code": "E026"}


def scenario_stop(backend: ClosedLoopBackend) -> Dict[str, Any]:
    backend.configure(b"test-config")
    backend.dwin_touch(1)
    backend.dwin_touch(1)
    obs = backend.observe()
    require(obs.snapshot.controller_state == "READY", "DWIN STOP did not return READY")
    require(any(x.data == b"STOP" for x in obs.can), "MCU STOP CAN command was not observed")
    return {"controller_state": "READY", "dwin_status": 0}


def scenario_complete(backend: ClosedLoopBackend) -> Dict[str, Any]:
    backend.configure(b"test-config")
    backend.dwin_touch(1)
    backend.inject_bms(soc=100)
    obs = backend.observe()
    require(obs.snapshot.controller_state == "READY" or obs.snapshot.dwin_status == 3,
            "charge completion was not observed")
    require(any(x.data == b"STOP" for x in obs.can), "completion did not produce MCU STOP")
    require(obs.snapshot.dwin_status == 3, "DWIN did not show COMPLETE")
    return {"controller_state": "COMPLETE", "dwin_status": 3}


def scenario_telemetry(backend: ClosedLoopBackend) -> Dict[str, Any]:
    backend.configure(b"test-config")
    backend.inject_module(voltage=53.5, current=24.5)
    obs = backend.observe()
    require(obs.snapshot.bms_soc == 82, "BMS telemetry was not observed")
    require(obs.snapshot.modules_online == 1, "module online telemetry was not observed")
    return {"bms_soc": 82, "modules_online": 1}


def run_scenario(backend: ClosedLoopBackend, scenario: Scenario) -> ScenarioResult:
    started = time.monotonic()
    try:
        expected = scenario.execute(backend)
        obs = backend.observe()
        return ScenarioResult(scenario.scenario_id, scenario.name, TestStatus.PASS,
                              int((time.monotonic() - started) * 1000), expected,
                              obs.snapshot.__dict__.copy())
    except TransportError as exc:
        return ScenarioResult(scenario.scenario_id, scenario.name, TestStatus.BLOCKED,
                              int((time.monotonic() - started) * 1000), error=str(exc))
    except Exception as exc:
        obs = backend.observe()
        return ScenarioResult(scenario.scenario_id, scenario.name, TestStatus.FAIL,
                              int((time.monotonic() - started) * 1000),
                              observed=obs.snapshot.__dict__.copy(), error=str(exc))


def run(backend: ClosedLoopBackend, output: str = "test/artifacts") -> int:
    evidence = Evidence(output)
    try:
        backend.preflight()
        scenarios = [
            Scenario("CFG-START-001", "config then DWIN start", scenario_config_and_start),
            Scenario("CONTROL-STOP-001", "DWIN stop", scenario_stop),
            Scenario("COMPLETE-001", "charge complete", scenario_complete),
            Scenario("TELEMETRY-001", "SCADA telemetry source", scenario_telemetry),
            Scenario("BMS-FAULT-001", "BMS high cell voltage", scenario_bms_fault),
            Scenario("MOD-FAULT-001", "module AC undervoltage", scenario_module_fault),
        ]
        for scenario in scenarios:
            backend.reset()
            result = run_scenario(backend, scenario)
            evidence.add(result)
            if result.status != TestStatus.PASS:
                break
        obs = backend.observe()
        evidence.write_frames(obs.usb, obs.can, obs.dwin)
    except TransportError as exc:
        evidence.add(ScenarioResult("PREFLIGHT", "strict hardware preflight", TestStatus.BLOCKED, 0,
                                    error=str(exc)))
    finally:
        backend.close()
    path = evidence.write()
    print(json.dumps({"artifact": str(path), "results": [x.__dict__ for x in evidence.results]}, indent=2,
                     default=str))
    return 0 if evidence.results and all(x.status == TestStatus.PASS for x in evidence.results) else 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Charger closed-loop automation runner")
    parser.add_argument("--backend", choices=("dry-run",), default="dry-run")
    parser.add_argument("--output", default="test/artifacts")
    args = parser.parse_args()
    return run(DryRunBackend(), args.output)


if __name__ == "__main__":
    raise SystemExit(main())
