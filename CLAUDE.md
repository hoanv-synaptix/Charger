# CLAUDE.md — Working Agreement for Claude Code on this Repo

This file is read automatically at the start of every Claude Code session here.
It is the *operational* companion to `AGENTS.md`. `AGENTS.md` is the canonical
engineering rulebook (29 sections: architecture, ISR rules, timing, safety,
naming, refactor discipline, etc.) — **read it in full before making any
non-trivial change.** This file does not repeat those rules; it tells you
concretely how to work in *this* codebase, on *this* toolchain, today.

If anything here ever conflicts with `AGENTS.md`, `AGENTS.md` wins — file an
inconsistency instead of silently picking one.

---

## 1. What this is

STM32G0B1CBT6 bare-metal C11 battery-charger controller firmware
("Charger_CTRL"). Single-executable CMake build (no per-module library
targets — see section 6 below for why that matters). No RTOS. Talks to:

- one or more charger power modules over CAN1 (Maxwell / Lianming / TonHe
  vendor protocols, selectable driver)
- a BMS over CAN2 (custom frame protocol)
- a DWIN HMI touchscreen over RS485 (half-duplex, DE-toggled)
- a PC over USB CDC (custom framed binary protocol + a debug sub-protocol)

Everything above matters because the whole design is "keep the charger
producing the right voltage/current safely even when any one of these four
links is flaky or absent."

## 2. Repo map (source layers)

```
App/System/     — composition root, orchestration, App_Init/App_Loop
App/Charge/     — pure charging policy + charge_controller.c (hardware-agnostic)
App/Protocol/   — PC protocol + PC debug protocol (USB CDC)
Modules/bms/    — BMS CAN protocol parsing + normalized BmsView
Modules/chg_lib/— charger driver abstraction (ops-table) + Maxwell/Lianming/TonHe drivers
Modules/hmi/    — DWIN protocol (RS485)
BSP/            — MCU-specific hardware access (CAN, flash, RS485, failsafe, critical sections)
Core/, Drivers/, Middlewares/, USB_Device/Target/, cmake/  — CubeMX/vendor GENERATED, do not hand-edit business logic here
Utils/Log/      — LOG() — see the blocking-UART warning below, this one bites
docs/AUDIT_Findings.md — the living bug tracker/audit log, see section 8 below
tools/check_architecture.py, tools/arch_baseline.txt — dependency-direction checker
check_ioc.py    — guards hand-tuned CubeMX-generated values against silent regen drift
test/           — host-buildable unit tests (gcc, no STM32 headers)
debug_app/, ui/ — SOMEONE ELSE'S CONCURRENT WORK. Never touch, stage, or commit these.
```

Dependency direction is strictly downward (App → Modules → Platform/BSP →
HAL); `tools/check_architecture.py` enforces it by grep, not by a real
translation-unit graph — on purpose (AGENTS.md 2.1: no unnecessary tooling
abstraction). A small number of pre-existing exceptions are documented,
with rationale, at the top of `tools/arch_baseline.txt` — read that file
before assuming a flagged include is fixable in isolation.

## 3. The verification loop — run this before every commit

This project has no hardware-in-the-loop CI reachable from this session, so
these four checks are the whole safety net. Run all four after touching
anything, not just the file you think you changed:

```bash
# 1. Per-file compile sanity (pick include paths matching the file's layer;
#    look at a recent commit touching a sibling file for the exact -I list)
gcc -fsyntax-only -Wall -Wextra -Wimplicit-fallthrough -DSTM32G0B1xx \
    -I <layer dirs...> -I test/mock_hal <file.c>

# 2. Architecture / dependency-direction check
python3 tools/check_architecture.py

# 3. .ioc-vs-generated-code drift check (catches silent CubeMX regen damage)
python3 check_ioc.py

# 4. Host unit tests
gcc -I"Modules/bms" -I"Modules/chg_lib" -I"test/mock_hal" \
    test/test_logic.c Modules/bms/bms_protocol.c Modules/chg_lib/chg_lib_fsm.c \
    -o /tmp/test_logic.elf && /tmp/test_logic.elf
# must print: ALL TESTS PASSED.
```

A change is not done until all four are clean. If (1) needs headers that
pull in real STM32/CMSIS code (USB stack, HAL structs), add
`-DSTM32G0B1xx -I Core/Inc -I Drivers/STM32G0xx_HAL_Driver/Inc
-I Drivers/CMSIS/Device/ST/STM32G0xx/Include -I Drivers/CMSIS/Include` —
don't skip the compile check just because the include list got long.
Benign, pre-existing warnings you will see and can ignore: `-Wint-to-pointer-cast`
style cast warnings from CMSIS/HAL macros built for a 32-bit target,
compiled here on a 64-bit host.

Where files live on the user's device (not this cloud container), do the
edit and the verification loop with `device_bash` — read/edit/compile in
place, don't round-trip the file through the cloud workspace unless a step
genuinely needs a tool only the cloud workspace has (see the device-bridge
guidance already in your system prompt).

## 4. Hard constraints learned the expensive way

These are not in `AGENTS.md` verbatim but follow directly from it — call
them out explicitly because they're easy to violate without noticing:

- **`LOG()` blocks for up to 50ms.** `Utils/Log/debug_log.c`'s `LOG()` calls
  `HAL_UART_Transmit(&huart1, ..., 50)` — a blocking call with a 50ms
  timeout. **Never call `LOG()` inside a `BSP_EnterCritical()/BSP_ExitCritical()`
  section** — that disables all interrupts (including CAN RX) for up to
  50ms. When you need a critical section around a read-modify-write that's
  surrounded by `LOG()` calls in the same function, wrap *only* the specific
  statements that race, not the whole function/state machine.
- **Tick-diff idiom depends on who else touches the tick.** For a tick field
  written only from the main loop, plain unsigned subtraction
  (`now - last_tick >= timeout_ms`, per AGENTS.md 9) is correct and
  wraparound-safe — leave it alone. For a tick field that can be written
  from an ISR while the main loop also reads/compares it (e.g. BMS
  `last_rx_tick`), use the signed-diff idiom instead:
  `int32_t diff = (int32_t)(now - last); elapsed = (diff < 0) ? 0 : (uint32_t)diff;`
  (see `bms_tick_elapsed()` in `Modules/bms/bms_core.c`) — this also
  correctly handles genuine 32-bit wraparound (~49.7 days).
- **CMake puts the whole firmware on one executable target.** There are no
  per-module library targets, so nothing at the build-system level stops
  one module from reaching into another module's internals. Where that
  matters, a `priv/` subfolder (e.g. `Modules/chg_lib/priv/`) marks a
  header as private to its module, enforced by `check_architecture.py`'s
  private-header rule, not by the linker. Follow that convention rather
  than inventing a new one.
- **Float setpoint validation.** Any externally-supplied float that becomes
  a voltage/current setpoint must be checked with `isfinite()` before use —
  a plain `x != x` NaN check does not catch `Inf`. This has already bitten
  this codebase twice (config validation, driver setpoints); don't
  reintroduce it.

## 5. Established patterns — reuse them, don't reinvent

- **Charger driver abstraction**: a `CHG_LIB_DriverOps_t` ops table
  (Strategy/Abstract-Factory); Maxwell/Lianming/TonHe each implement it;
  only one driver active at a time. New vendor → new ops-table
  implementation, not a new abstraction layer (AGENTS.md 12).
- **BMS frame dispatch**: a table of `{is_ext, id, id_mask, min_dlc, type,
  func}` entries in `Modules/bms/bms_protocol.c`, iterated once in
  `BMS_ParseFrame()`. Adding a new frame type means adding a table row, not
  new branching logic.
- **Critical sections**: `BSP_EnterCritical()`/`BSP_ExitCritical()`
  (wraps `__disable_irq`/`__enable_irq`) is the house abstraction for
  ISR/main-loop races. Keep sections surgically short (see the LOG warning
  above).
- **BMS staleness is informational-only by explicit product decision.**
  `BMS_IsDataStale()` does not gate the charge relay and does not clear
  `BmsView` telemetry fields — the view keeps the last known values, which
  is what lets `charge_controller.c` "coast" on old targets without extra
  freeze logic. Do not change this without asking — it was a deliberate,
  explicit user decision (see `docs/AUDIT_Findings.md` section 4, DES-02),
  and AGENTS.md 15 requires explicit tests and review for any BMS
  fault-handling change anyway.
- **Named constants over magic numbers** for anything with a physical or
  protocol meaning (retry counts, timeouts, default ratings) — see the
  `MXR_*`/`LM_*` constants in the chg_lib drivers for the expected style.

## 6. Current project status

`AGENTS.md` section 28 defines the refactor order PR0→PR8. As of the last
session:

- **PR0-PR4 done**: guardrails/CI, build boundaries, platform/BSP cleanup,
  charger subsystem cleanup, BMS transport/protocol/state separation.
- **`docs/AUDIT_Findings.md` sections 3 (chg_lib), 4 (BMS), 5 (Charge
  Controller/Config), 6 (PC/BMS/HMI comms), 7 (BSP/Platform) have all been
  re-verified against current source** (not just read from the original
  audit — many listed items turned out to be already fixed by an earlier,
  undocumented "Sprint 1" commit pair and were closed out with a
  re-verification note rather than re-fixed). Read the `_Re-verified
  2026-08-27: ..._` annotations before assuming an open checkbox is still
  live — some are deliberately left open with a stated reason (needs real
  hardware to verify timing, or is a style/testability nice-to-have, not a
  defect).
- **PR5 (pure charge policy/controller cleanup), PR6 (App orchestration),
  PR7 (PC+DWIN protocol cleanup beyond what's already fixed), PR8
  (integration/naming per AGENTS.md 17)** are the remaining scope. PR8 in
  particular (renaming `CHG_LIB_*` → domain names) is explicitly
  lower-priority and should be done as its own isolated, behavior-preserving
  commit series, never mixed with a bug fix (AGENTS.md 24/25).
- A few items are intentionally left open pending real hardware access:
  RS485 TX blocking-timeout tuning (I-07), POWER_EN power-up delay sizing
  (I-10). Do not guess timing values for these without a scope/logic
  analyzer on the real board — say so and leave them open instead.

Before starting new work, read the addendum note near the top of
`docs/AUDIT_Findings.md` and the most recent `_Re-verified_` annotations —
they are the actual source of truth on what's open, not the original
checkbox state.

## 7. Workflow checklist for any change

Mirrors AGENTS.md section 26, made concrete for this repo:

1. Identify the owning layer/module (section 2 above) and read its current
   dependencies before touching it.
2. Decide explicitly: refactor (behavior-preserving) or bug fix
   (behavior change)? Don't mix the two in one commit (AGENTS.md 24/25).
3. Make the smallest change that fixes the root cause in the layer that
   actually owns it — don't patch a transport-layer bug at the application
   layer (AGENTS.md 25's layer-by-layer debugging order).
4. Run the full verification loop (section 3 above).
5. Commit with a message that states root cause, why it's real (or why
   something was deliberately left alone), what was verified, and any
   hardware-verification caveat — end it with:
   ```
   Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
   Claude-Session: <this session's URL>
   ```
6. Update `docs/AUDIT_Findings.md` if the change closes or reclassifies a
   tracked item — use the `_Re-verified <date>: <verdict + evidence>_`
   annotation style already established there, don't just delete the line.
7. Never touch, stage, or commit anything under `debug_app/` or `ui/` —
   that's someone else's concurrent, unrelated work; it will show up
   modified in `git status` constantly and is not yours to manage.
8. If the user asks for a broad autonomous pass ("do the whole thing, only
   report back when done"), still commit incrementally (one conceptual
   change per commit, per AGENTS.md 24) — "report only at the end" means
   don't stop to ask questions, not "squash everything into one commit."

## 8. When you're not sure

Default to the judgment call in AGENTS.md 29: prefer the design that's
easier to understand on a debugger, easier to host-test, easier to reason
about during a fault, more deterministic, smaller, less abstract. When a
fix would touch safety-relevant behavior (AGENTS.md 15's list — e-stop,
relay behavior, BMS/charger fault handling, watchdog, over-voltage/
over-temperature protection) and you can't verify it on real hardware,
document the decision and the reasoning instead of guessing — that's not
indecision, it's the correct senior-engineer call for this kind of system.
