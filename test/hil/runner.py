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


def dwin_text_seen(obs: Observation, vp: int, expected: str) -> bool:
    case_start = int(obs.snapshot.extras.get("case_start_ms", 0))
    if case_start == 0:
        return vp == 0x1048 and obs.snapshot.dwin_soc_text == expected
    return any(frame.vp == vp and
               frame.timestamp_ms >= case_start and
               frame.payload.rstrip(b"\0").decode("latin1", "ignore") == expected
               for frame in obs.dwin)


def dwin_code_seen(obs: Observation, expected: str) -> bool:
    case_start = int(obs.snapshot.extras.get("case_start_ms", 0))
    if case_start == 0:
        return obs.snapshot.dwin_code == expected or any(
            frame.vp == 0x1044 and
            frame.payload.rstrip(b"\0").decode("latin1", "ignore").strip() == expected
            for frame in obs.dwin)
    return any(frame.vp == 0x1044 and frame.timestamp_ms >= case_start and
               frame.payload.rstrip(b"\0").decode("latin1", "ignore").strip() == expected
               for frame in obs.dwin)


def settle(backend: ClosedLoopBackend, seconds: float = 1.0):
    """Allow real CAN/USB/DWIN transports to publish a complete snapshot."""
    time.sleep(seconds)
    return backend.observe()


def wait_until(backend: ClosedLoopBackend,
               predicate: Callable[[Observation], bool],
               timeout: float = 5.0, poll: float = 0.1) -> Observation:
    deadline = time.monotonic() + timeout
    latest = backend.observe()
    while time.monotonic() < deadline:
        latest = backend.observe()
        if predicate(latest):
            return latest
        time.sleep(poll)
    return latest


def start_session(backend: ClosedLoopBackend):
    """Create a valid charging precondition before session-dependent tests."""
    backend.set_sources_online(True, True)
    backend.inject_bms(soc=82, pack_current_a=24.5, pack_voltage_v=53.5)
    backend.inject_module(actually_on=False, fault_bits=0)
    backend.configure(b"test-config")
    obs = wait_until(backend,
                     lambda x: x.snapshot.bms_state == "ONLINE" and
                               x.snapshot.modules_online == 1,
                     timeout=5.0)
    require(obs.snapshot.bms_state == "ONLINE" and obs.snapshot.modules_online == 1,
            "CAN simulators did not establish online precondition")
    backend.pc_start()
    obs = wait_until(backend,
                     lambda x: x.snapshot.controller_state in
                     ("STARTING", "CHARGING", "RUNNING"), timeout=5.0)
    require(obs.snapshot.controller_state in ("STARTING", "CHARGING", "RUNNING"),
            f"charging precondition not reached: status={obs.snapshot.dwin_status}")
    return obs


def dwin_touch_until(backend: ClosedLoopBackend, expected_state: tuple[str, ...],
                     timeout: float = 5.0) -> Observation:
    """Press once and retry only while the panel still reports READY."""
    backend.dwin_touch(1)
    obs = wait_until(backend,
                     lambda x: x.snapshot.controller_state in expected_state,
                     timeout=2.0)
    if obs.snapshot.controller_state in expected_state:
        return obs
    retry_start = obs.snapshot.dwin_status == 0 and "READY" not in expected_state
    retry_stop = "READY" in expected_state and obs.snapshot.dwin_status in (1, 2)
    if retry_start or retry_stop:
        backend.dwin_touch(1)
    return wait_until(backend,
                      lambda x: x.snapshot.controller_state in expected_state,
                      timeout=timeout)


@dataclass(frozen=True)
class Scenario:
    scenario_id: str
    name: str
    execute: Callable[[ClosedLoopBackend], Dict[str, Any]]


def scenario_config_and_start(backend: ClosedLoopBackend) -> Dict[str, Any]:
    backend.set_sources_online(True, True)
    backend.inject_bms(soc=82, pack_current_a=24.5, pack_voltage_v=53.5)
    backend.configure(b"test-config")
    obs = wait_until(backend, lambda x: x.snapshot.bms_state == "ONLINE" and
                     x.snapshot.modules_online == 1, timeout=5.0)
    require(obs.snapshot.bms_state == "ONLINE" and obs.snapshot.modules_online == 1,
            "CAN simulators did not establish online precondition")
    obs = dwin_touch_until(backend, ("STARTING", "CHARGING", "RUNNING"))
    require(obs.snapshot.controller_state in ("STARTING", "CHARGING", "RUNNING"),
            "DWIN START did not produce STARTING/CHARGING")
    require(any((x.direction == "mcu-rx" and x.channel == 0 and x.data[:1] == b"\xAA") or
                x.data == b"START" for x in obs.can), "MCU START CAN command was not observed")
    return {"controller_state": "RUNNING", "dwin_status": 2}


def scenario_boot_idle(backend: ClosedLoopBackend) -> Dict[str, Any]:
    backend.set_sources_online(False, False)
    obs = settle(backend, 8.0)
    require(obs.snapshot.controller_state in ("READY", "IDLE"),
            "boot without CAN sources must remain READY/IDLE")
    require(obs.snapshot.dwin_code in (None, "0000", "----", ""),
            f"unexpected boot alarm: {obs.snapshot.dwin_code!r}")
    require(obs.snapshot.bms_soc is None, "offline BMS must not expose a numeric SOC")
    require(obs.snapshot.modules_online in (None, 0), "offline modules must not be counted online")
    require(dwin_text_seen(obs, 0x1048, "--%"),
            f"offline SOC text is not --%: {obs.snapshot.dwin_soc_text!r}")
    return {"controller_state": "READY/IDLE", "alarm": None, "soc": "--%"}


def scenario_bms_telemetry(backend: ClosedLoopBackend) -> Dict[str, Any]:
    backend.set_sources_online(True, False)
    backend.inject_bms(soc=50)
    obs = wait_until(backend, lambda x: dwin_text_seen(x, 0x1048, "50%"),
                     timeout=5.0)
    require(obs.snapshot.bms_state == "ONLINE", "BMS online state was not observed")
    require(obs.snapshot.bms_soc == 50, "BMS SOC was not propagated")
    require(dwin_text_seen(obs, 0x1048, "50%"),
            f"DWIN SOC text mismatch: {obs.snapshot.dwin_soc_text!r}")
    return {"bms_soc": 50, "dwin_soc": "50%"}


def scenario_module_telemetry(backend: ClosedLoopBackend) -> Dict[str, Any]:
    start_session(backend)
    backend.inject_module(voltage=53.5, current=24.5, actually_on=True)
    obs = settle(backend)
    require(obs.snapshot.modules_online == 1, "module online state was not observed")
    require(obs.snapshot.module_voltage == 53.5, "module voltage mismatch")
    return {"modules_online": 1, "module_voltage": 53.5, "module_current": 24.5}


def scenario_soc_color_boundaries(backend: ClosedLoopBackend) -> Dict[str, Any]:
    expected = ((0, 0xF800), (10, 0xF800), (11, 0xFD20),
                (30, 0xFD20), (31, 0xD520), (60, 0xD520),
                (61, 0x2CEA), (100, 0x2CEA))
    backend.set_sources_online(True, False)
    for soc, color in expected:
        backend.inject_bms(soc=soc)
        obs = settle(backend, 1.0)
        require(obs.snapshot.dwin_soc_color == color,
                f"SOC {soc}% color mismatch: {obs.snapshot.dwin_soc_color!r}")
    backend.inject_bms(soc=101)
    obs = settle(backend, 1.0)
    require(obs.snapshot.dwin_soc_text in (None, "--%"), "invalid SOC must be unavailable")
    require(obs.snapshot.dwin_soc_color in (None, 0x8410), "invalid SOC must be gray")
    return {"boundaries": len(expected), "invalid": "--%"}


def scenario_bms_offline(backend: ClosedLoopBackend) -> Dict[str, Any]:
    # This is a data-availability test, not a communication-fault-under-load
    # test. Keep the controller idle so E021 cannot mask the DWIN data view.
    backend.pc_stop()
    settle(backend, 2.0)
    backend.set_sources_online(False, True)
    obs = wait_until(backend, lambda x: dwin_text_seen(x, 0x1048, "--%"),
                     timeout=12.0)
    require(obs.snapshot.bms_state in (None, "OFFLINE"), "BMS did not become offline")
    require(dwin_text_seen(obs, 0x1048, "--%"), "offline BMS SOC is not --%")
    return {"bms": "OFFLINE", "soc": "--%"}


def scenario_module_offline(backend: ClosedLoopBackend) -> Dict[str, Any]:
    backend.set_sources_online(True, False)
    obs = settle(backend)
    require(obs.snapshot.modules_online in (None, 0), "offline module was counted online")
    return {"module": "OFFLINE", "module_count": 0}


def scenario_bms_fault(backend: ClosedLoopBackend) -> Dict[str, Any]:
    start_session(backend)
    wait_until(backend, lambda x: x.snapshot.controller_state in
               ("CHARGING", "RUNNING"), timeout=5.0)
    backend.inject_bms(high_cell_fault=True)
    obs = settle(backend, 1.5)
    require(obs.snapshot.controller_state == "FAULT", "BMS fault did not reach controller")
    require(obs.snapshot.dwin_code == "E004" or dwin_code_seen(obs, "E004"),
            "DWIN E004 was not observed")
    return {"controller_state": "FAULT", "dwin_code": "E004"}


def scenario_module_fault(backend: ClosedLoopBackend) -> Dict[str, Any]:
    start_session(backend)
    wait_until(backend, lambda x: x.snapshot.controller_state in
               ("CHARGING", "RUNNING"), timeout=5.0)
    backend.inject_module(ac_undervoltage=True)
    obs = settle(backend, 6.0)
    require(obs.snapshot.controller_state == "FAULT" or dwin_code_seen(obs, "E026"),
            "module AC undervoltage did not reach station alarm output")
    require(obs.snapshot.dwin_code == "E026" or dwin_code_seen(obs, "E026"),
            "DWIN E026 was not observed")
    return {"controller_state": "FAULT", "dwin_code": "E026"}


def scenario_stop(backend: ClosedLoopBackend) -> Dict[str, Any]:
    start_session(backend)
    obs = dwin_touch_until(backend, ("READY",), timeout=6.0)
    require(obs.snapshot.controller_state == "READY", "DWIN STOP did not return READY")
    require(any((x.direction == "mcu-rx" and x.channel == 0 and x.data[:1] == b"\x55") or
                x.data == b"STOP" for x in obs.can), "MCU STOP CAN command was not observed")
    return {"controller_state": "READY", "dwin_status": 0}


def scenario_complete(backend: ClosedLoopBackend) -> Dict[str, Any]:
    start_session(backend)
    backend.inject_bms(soc=100)
    obs = wait_until(backend, lambda x: dwin_text_seen(x, 0x1048, "100%"),
                     timeout=5.0)
    require(obs.snapshot.controller_state == "READY" or obs.snapshot.dwin_status == 3,
            "charge completion was not observed")
    require(any((x.direction == "mcu-rx" and x.channel == 0 and
                 x.data[:1] == b"\x55") or x.data == b"STOP" for x in obs.can),
            "completion did not produce MCU STOP")
    require(obs.snapshot.dwin_status == 3, "DWIN did not show COMPLETE")
    return {"controller_state": "COMPLETE", "dwin_status": 3}


def scenario_telemetry(backend: ClosedLoopBackend) -> Dict[str, Any]:
    backend.configure(b"test-config")
    backend.inject_module(voltage=53.5, current=24.5)
    obs = settle(backend)
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
            Scenario("BOOT-IDLE-001", "boot without CAN remains idle", scenario_boot_idle),
            Scenario("BMS-TELEMETRY-001", "BMS telemetry and SOC text", scenario_bms_telemetry),
            Scenario("MODULE-TELEMETRY-001", "module telemetry", scenario_module_telemetry),
            Scenario("CFG-START-001", "config then DWIN start", scenario_config_and_start),
            Scenario("CONTROL-STOP-001", "DWIN stop", scenario_stop),
            Scenario("COMPLETE-001", "charge complete", scenario_complete),
            Scenario("TELEMETRY-001", "SCADA telemetry source", scenario_telemetry),
            Scenario("BMS-FAULT-001", "BMS high cell voltage", scenario_bms_fault),
            Scenario("MOD-FAULT-001", "module AC undervoltage", scenario_module_fault),
            Scenario("SOC-COLOR-001", "SOC color boundaries", scenario_soc_color_boundaries),
            Scenario("BMS-OFFLINE-001", "BMS offline data unavailable", scenario_bms_offline),
            Scenario("MODULE-OFFLINE-001", "module offline data unavailable", scenario_module_offline),
        ]
        for scenario in scenarios:
            backend.reset()
            result = run_scenario(backend, scenario)
            evidence.add(result)
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
    parser.add_argument("--backend", choices=("dry-run", "zcan"), default="dry-run")
    parser.add_argument("--usb-port", default="COM26")
    parser.add_argument("--dwin-port", default="COM25")
    parser.add_argument("--driver", choices=("tonhe", "maxwell", "lianming"), default="tonhe")
    parser.add_argument("--addr", type=int, default=1)
    parser.add_argument("--output", default="test/artifacts")
    args = parser.parse_args()
    if args.backend == "zcan":
        from .hardware import ZcanHardwareBackend
        backend = ZcanHardwareBackend(args.usb_port, args.dwin_port, args.driver, args.addr)
    else:
        backend = DryRunBackend()
    return run(backend, args.output)


if __name__ == "__main__":
    raise SystemExit(main())
