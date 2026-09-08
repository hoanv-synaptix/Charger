# Charger closed-loop automation

This directory contains test-only automation. It must not be included by the
firmware build and must not change `App/`, `Modules/`, `BSP/`, or `Core/`.

## Current entrypoint

```powershell
py -3 -m test.hil.runner --backend dry-run
py -3 -m unittest test.hil.test_runner
```

The dry-run backend verifies the runner, strict assertions, and report format
without hardware. The hardware backend connects the same scenario/oracle
interfaces to USB CDC, ZLG ControlCAN and DWIN RS485:

```powershell
py -3 -m test.hil.runner --backend zcan --usb-port COM26 --dwin-port COM25 --driver tonhe
```

## Evidence

Each run writes a timestamped directory under `test/artifacts/` containing:

- `summary.json`
- `junit.xml`
- `usb_raw.json`
- `can_raw.json`
- `dwin_raw.json`

Hardware runs must fail preflight if any required USB, ZCAN or DWIN endpoint is
missing. A missing DWIN sniffer is never treated as a pass.

The ZCAN suite covers boot without CAN, BMS/module telemetry, start/stop,
completion, BMS/module fault paths, SOC text/color boundaries, BMS offline and
module offline. It observes the real USB and DWIN endpoints; missing observation
is reported as blocked or failed, never as a successful assertion.

The existing standalone simulator can still be launched from the repository
root with `run_zcan_auto_test.bat` or `run_zcan_hil_sim.bat`.
