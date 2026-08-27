# AGENTS.md — Charger Firmware Engineering Rules

## 1. Purpose

This file defines the engineering rules for all AI agents and contributors working on the Charger firmware.

The project is an STM32G0 bare-metal embedded system. The primary goals are:

- deterministic behavior
- simple architecture
- clear ownership
- testable business logic
- safe hardware interaction
- small, reviewable changes
- CI green after every change
- no unnecessary abstraction or framework-style design

Do not redesign working code for stylistic purity.

---

## 2. Core Engineering Principles

### 2.1 Keep the code simple

Prefer:

- plain C functions
- small private static contexts
- explicit data flow
- fixed-size buffers
- deterministic state machines
- direct, readable control flow

Avoid:

- unnecessary factories
- service layers
- repository patterns
- generic frameworks
- deep abstraction chains
- excessive callbacks
- unnecessary indirection
- abstraction created for only one trivial implementation

Do not introduce a new abstraction unless at least one of these is true:

1. there are two or more real implementations;
2. the abstraction isolates hardware/platform dependencies;
3. the abstraction is required for host-side testing;
4. it significantly simplifies ownership or dependency direction.

---

## 3. Platform Constraints

The firmware uses:

- C11
- STM32G0
- bare-metal execution
- STM32 HAL / CubeMX generated code
- no RTOS unless explicitly requested

Mandatory rules:

- no dynamic memory allocation
- no `malloc`
- no `calloc`
- no `realloc`
- no `free`
- no recursion
- no unbounded loops waiting for hardware
- no blocking delays during normal runtime
- `HAL_Delay()` is allowed only during initialization when justified
- no business logic inside interrupts
- no relay/charger enable decisions inside interrupts
- no logging from ISR unless explicitly proven safe
- no floating-point equality checks for physical values
- all external input lengths must be validated before access
- all CAN DLC values must be validated before payload parsing
- all protocol frame lengths must be validated before decoding

---

## 4. Generated Code Policy

These directories are generated or vendor-controlled:

- `Core/`
- `Drivers/`
- `Middlewares/`
- `USB_Device/`
- `cmake/stm32cubemx/`

Do not refactor generated files unless there is no safe alternative.

Prefer integration through:

- user-code sections
- callbacks
- wrapper modules
- platform modules
- application hooks

Never move business logic into CubeMX-generated files.

Generated code must remain regeneratable.

---

## 5. Target Architecture

The intended dependency direction is:

```text
App
 ↓
Modules
 ↓
Platform
 ↓
STM32 HAL
```

The application layer owns system decisions.

The modules layer owns device/protocol behavior.

The platform layer owns MCU-specific hardware access.

STM32 HAL must never depend upward.

### 5.1 Application

Application code may contain:

- charging policy
- charging state machine
- system orchestration
- command handling
- protection decisions
- operating state

Application code must not directly depend on:

- `main.h`
- STM32 HAL headers
- FDCAN handles
- UART handles
- GPIO registers
- direct ADC access
- direct flash access

Pure charging policy should compile on a host machine without STM32 headers.

### 5.2 Modules

Modules may contain:

- BMS normalized state
- BMS protocol parsing
- charger driver abstraction
- Maxwell protocol
- Lianming protocol
- TonHe protocol
- PC protocol
- DWIN protocol

Modules may depend on Platform when hardware access is required.

Modules must not own unrelated application policy.

### 5.3 Platform

Platform owns:

- CAN
- GPIO
- ADC
- RS485/UART
- Flash
- Clock/tick
- Watchdog
- low-level USB transport when applicable

Platform may depend on STM32 HAL.

Platform must not contain charging business policy.

---

## 6. Dependency Rules

Dependencies must point downward.

Forbidden examples:

```text
Platform -> App
Platform -> ChargeController
BMS protocol -> DWIN
Charger driver -> PC protocol
Charge policy -> HAL
Charge policy -> BSP
```

Application may read normalized views from modules.

Application may send commands to modules.

Hardware-specific details must not leak into application policy.

Example:

Bad:

```c
if (HAL_GPIO_ReadPin(...) == GPIO_PIN_RESET) {
    CHG_LIB_StopAll();
}
```

Better:

```c
if (inputs.emergency_stop_active) {
    ChargeController_EmergencyStop();
}
```

Hardware produces facts.

Application makes decisions.

Drivers execute decisions.

---

## 7. State Ownership

Every subsystem must clearly own its state.

For every module, it must be possible to answer:

1. What state does this module own?
2. Who may modify that state?
3. Who may read that state?
4. Which modules does it depend on?

Prefer one private static context for single-instance embedded subsystems.

Example:

```c
static BmsContext g_bms;
```

is acceptable.

Do not introduce public mutable global state.

Avoid:

```c
extern SomeState g_state;
```

Prefer:

```c
bool BMS_GetView(BmsView *view);
```

or immutable/snapshot-style data access.

---

## 8. ISR Rules

Interrupt handlers must be:

- short
- bounded
- deterministic
- non-blocking

Preferred ISR flow:

```text
ISR
 ↓
capture event/frame
 ↓
store into fixed-size queue or atomic snapshot
 ↓
set flag
 ↓
return
```

Main-loop flow:

```text
main loop
 ↓
consume event/frame
 ↓
parse
 ↓
update module state
 ↓
application decision
```

Do not perform the following in ISR unless strictly required and justified:

- business state transitions
- long protocol parsing
- flash writes
- formatted logging
- blocking I/O
- charger enable decisions
- relay sequencing
- heavy floating-point processing

---

## 9. Timing Rules

Use unsigned subtraction for timeout checks.

Correct:

```c
if ((uint32_t)(now - last_tick) >= timeout_ms) {
    ...
}
```

Avoid:

```c
if (now >= last_tick + timeout_ms) {
    ...
}
```

Do not call `HAL_GetTick()` from pure application/domain logic if the current time is already passed into the function.

Preferred:

```c
void ChargeController_Process(uint32_t now);
```

and all internal timing should derive from `now`.

Timeout logic must tolerate `uint32_t` tick wraparound.

---

## 10. Memory Rules

All runtime memory must be deterministic.

Prefer:

- static storage
- stack storage with bounded size
- fixed-size arrays
- compile-time capacities

Do not use heap allocation.

All queues must have explicit capacity.

Overflow behavior must be defined.

Examples:

- drop newest
- drop oldest
- increment overflow counter
- set fault flag

Never silently write beyond a buffer.

---

## 11. Protocol and Transport Separation

Communication modules should follow:

```text
Transport
 ↓
Protocol
 ↓
Normalized state / commands
```

### BMS

```text
CAN2
 ↓
BMS transport
 ↓
BMS protocol parser
 ↓
BmsView
```

### Charger

```text
CAN1
 ↓
charger transport
 ↓
Maxwell / Lianming / TonHe protocol
 ↓
ChargerModuleView
```

### PC Debug

```text
USB CDC
 ↓
PC transport
 ↓
PC protocol
 ↓
application commands / telemetry
```

### DWIN

```text
RS485
 ↓
DWIN transport
 ↓
DWIN protocol
 ↓
application commands / view mapping
```

Transport must not contain application policy.

Protocol parsing must not directly control relays or charging state.

---

## 12. Charger Driver Rules

Multiple charger manufacturers may share one common interface.

A small ops table is acceptable:

```c
typedef struct
{
    bool (*set_voltage)(uint8_t module, float voltage);
    bool (*set_current)(uint8_t module, float current);
    bool (*start)(uint8_t module);
    bool (*stop)(uint8_t module);
} ChargerDriverOps;
```

Do not add another abstraction layer around this unless necessary.

Driver-specific code owns:

- frame encoding
- frame decoding
- polling
- protocol-specific state
- vendor-specific alarm mapping

Application code must not contain:

```c
if (driver == MAXWELL) { ... }
else if (driver == TONHE) { ... }
```

Vendor-specific behavior belongs in the vendor driver.

---

## 13. BMS Rules

BMS should expose normalized state.

Application should consume normalized values such as:

- online
- SOC
- battery voltage
- battery current
- max cell voltage
- min cell voltage
- temperature
- alarm flags

Application should not parse raw BMS CAN bytes.

BMS protocol-specific fields should remain inside the BMS module unless required for telemetry/debugging.

---

## 14. Configuration Ownership

Charging configuration must have one clear owner.

PC, DWIN, Flash and application logic must not independently maintain different copies of configuration.

Preferred flow:

```text
Flash load ─────┐
PC update ──────┼─> ChargeConfig
DWIN update ────┘
                     ↓
               validated snapshot
                     ↓
              ChargeController
```

All externally supplied configuration must be validated before becoming active.

Configuration serialization for PC debug must use the same canonical configuration model used by the controller.

Avoid duplicate protocol-only copies of configuration unless explicitly required.

---

## 15. Safety Rules

Safety behavior has priority over convenience.

On uncertainty or critical communication failure:

- outputs must move toward a safe state
- charger commands must not remain enabled indefinitely
- relays must not be enabled without valid preconditions
- stale telemetry must not be treated as fresh data
- watchdog refresh must remain in a controlled main execution path
- safety decisions must be deterministic

Do not weaken safety behavior merely to make a test pass.

Any change to:

- emergency stop
- relay behavior
- BMS fault handling
- charger fault handling
- watchdog logic
- over-voltage protection
- over-temperature protection

requires explicit tests and review.

---

## 16. Error Handling

Use simple, meaningful return types.

For simple queries:

```c
bool BMS_GetView(BmsView *view);
```

is sufficient.

For APIs with multiple meaningful failure modes, use a small enum.

Example:

```c
typedef enum
{
    CHARGER_OK = 0,
    CHARGER_ERR_ARGUMENT,
    CHARGER_ERR_NOT_READY,
    CHARGER_ERR_TIMEOUT,
    CHARGER_ERR_IO
} ChargerResult;
```

Do not create large generic error frameworks.

Do not return undocumented magic integers.

---

## 17. Naming Rules

Names must reflect responsibility, not implementation history.

Prefer:

```text
Charger_Init
Charger_Process
Charger_Start
Charger_Stop
Charger_GetModuleView
Charger_GetSummary
```

over generic historical names such as:

```text
CHG_LIB_*
```

Renaming must be done incrementally and separately from behavioral changes where possible.

Type names should represent domain meaning:

```text
ChargerState
ChargerModuleView
ChargerSummary
BmsView
ChargeConfig
ChargeControllerView
```

Avoid meaningless names such as:

```text
Manager
Helper
Common
Misc
Utils2
Handler2
```

unless the responsibility is genuinely clear.

---

## 18. Function Design

Prefer small functions with one responsibility.

A function should not simultaneously:

- parse transport data
- update business state
- modify hardware
- format telemetry
- persist configuration

Avoid very large orchestration functions.

However, do not split a clear 20-line function into five tiny functions only for style.

Readable locality is more important than artificial function-count metrics.

---

## 19. Comments

Comments should explain:

- why
- hardware constraints
- protocol quirks
- safety rationale
- timing assumptions
- non-obvious invariants

Do not comment obvious syntax.

Bad:

```c
/* increment counter */
counter++;
```

Good:

```c
/* Keep the previous valid target while BMS data is stale for less than
 * the offline timeout. This prevents unnecessary output oscillation. */
```

---

## 20. Logging

Logging must not affect real-time behavior.

Rules:

- no formatted logging from ISR
- rate-limit periodic logs
- avoid logging every control-loop iteration
- state transitions may be logged
- communication error counters may be logged periodically
- logs must not be required for correct behavior

Debug logging may be compiled out in production.

---

## 21. Testing Rules

Pure logic must be host-testable.

Host tests should cover:

- charging state transitions
- limit calculations
- derating
- configuration validation
- timeout behavior
- wraparound timing
- BMS protocol parsing
- charger protocol parsing
- PC protocol serialization/deserialization
- error and malformed-frame cases

Bug fixes should include regression tests whenever reproducible without real hardware.

Do not delete, disable or weaken tests to make CI pass.

Do not modify expected results merely to match broken behavior.

---

## 22. CI Rules

Every commit and pull request must keep CI green.

Minimum required checks:

1. architecture/dependency check
2. static analysis
3. host unit tests
4. host integration tests where applicable
5. firmware Release build
6. firmware size report

Hardware-in-the-loop tests should run separately when hardware is available.

Do not merge architecture refactors with failing firmware builds.

---

## 23. Architecture Enforcement

The project should reject forbidden includes automatically.

Examples that should fail CI:

```text
App/charge/* -> main.h
App/charge/* -> stm32*.h
App/charge/* -> bsp_*.h
Platform/* -> App/*
Modules/bms/* -> App/*
Modules/charger/* -> App/*
```

Do not bypass architecture checks with relative include tricks.

If a dependency is genuinely required, redesign the boundary or explicitly update the architecture rule with justification.

---

## 24. Refactor Discipline

Refactoring must be incremental.

Do not perform big-bang rewrites.

One conceptual change per commit.

Examples:

Good:

```text
commit 1: add architecture checker
commit 2: isolate clock dependency
commit 3: add host tests
commit 4: move BMS parsing out of ISR
```

Bad:

```text
commit: rewrite entire firmware architecture and fix CAN
```

During architecture-only refactors:

- preserve runtime behavior
- preserve protocol compatibility
- preserve configuration format
- preserve hardware pin behavior
- preserve timing unless explicitly changed
- preserve public API where practical

Behavior changes must be explicit.

---

## 25. Bug-Fix Discipline

Do not mix broad refactoring with bug fixing unless the bug cannot be isolated otherwise.

Preferred order:

1. reproduce
2. add instrumentation or test
3. identify failing layer
4. add regression test if possible
5. fix smallest responsible layer
6. run full CI
7. verify hardware behavior

For communication bugs, debug layer-by-layer:

```text
physical peripheral
 ↓
transport RX/TX
 ↓
protocol parser/encoder
 ↓
normalized state
 ↓
application
```

Do not guess at the application layer before proving transport works.

---

## 26. AI Agent Change Rules

Before modifying code, an AI agent must:

1. identify the owning module;
2. identify current dependencies;
3. identify whether the change is refactor or behavior change;
4. inspect relevant tests;
5. keep the change minimal.

After modifying code, an AI agent must:

1. build affected host tests;
2. run relevant tests;
3. run static analysis where available;
4. build firmware;
5. report any behavior change explicitly;
6. report any remaining risk explicitly.

AI agents must never:

- invent hardware behavior
- invent protocol fields
- invent undocumented pin mappings
- silently change timing values
- silently change CAN IDs
- silently change bit layout
- silently change flash layout
- silently change configuration compatibility
- disable watchdog logic
- suppress failures merely to get green CI
- perform unrelated cleanup in the same change
- add abstractions solely because they look cleaner

---

## 27. Definition of Done

A change is complete only when:

- responsibility remains clear;
- dependency direction is valid;
- no unnecessary abstraction was added;
- tests pass;
- firmware builds;
- CI passes;
- safety behavior is preserved or intentionally updated;
- protocol compatibility is preserved or explicitly documented;
- no unrelated code was modified.

---

## 28. Project Refactor Order

The intended refactor sequence is:

```text
PR0  Guardrails / CI / architecture rules
PR1  Build dependency boundaries
PR2  Platform/BSP cleanup
PR3  Charger subsystem cleanup
PR4  BMS transport/protocol/state separation
PR5  Pure charge policy/controller cleanup
PR6  App orchestration cleanup
PR7  PC + DWIN protocol cleanup
PR8  Integration cleanup and naming
```

Do not skip ahead with large cross-layer rewrites unless explicitly requested.

Communication bugs will be fixed after the architectural baseline is stable, except for critical safety defects.

---

## 29. Final Rule

When choosing between two designs, prefer the design that is:

1. easier to understand on an embedded debugger;
2. easier to test on host;
3. easier to reason about during a fault;
4. more deterministic;
5. smaller;
6. less abstract.

Simple, explicit embedded C is preferred over architecturally fashionable code.
