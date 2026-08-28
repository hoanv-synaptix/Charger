# Detailed Implementation Plan: Unified State Machine Pattern

## Overview

Refactor 3 driver files to use unified pattern for:
- `set_state()` - centralized state transitions with online flag management
- `check_offline_timeout()` - centralized timeout detection
- `process_module()` - unified loop with timeout check first

---

## Common Pattern Definition

### 1. check_offline_timeout()
```c
/**
 * @brief Check for communication timeout and transition to OFFLINE if needed
 * @note Called FIRST in process_module() for ALL states except OFFLINE/RECOVERING
 */
static void check_offline_timeout(Mod *mod, uint32_t now) {
    /* Already offline or recovering - no need to check */
    if (mod->view.state == CHG_LIB_STATE_OFFLINE ||
        mod->view.state == CHG_LIB_STATE_RECOVERING) {
        return;
    }

    uint32_t since_rx = now - mod->view.last_rx_tick;

    if (since_rx > OFFLINE_TIMEOUT_MS) {
        mod->view.stats.timeout_count++;
        set_state(mod, CHG_LIB_STATE_OFFLINE, now);
    }
}
```

### 2. set_state() - Unified Online Flag Logic
```c
/**
 * @brief Centralized state transition handler
 * @note This is the ONLY place where online flag is set
 */
static void set_state(Mod *mod, CHG_LIB_State_t new_state, uint32_t now) {
    if (mod->view.state == new_state) return;

    mod->view.state = new_state;
    mod->state_enter_tick = now;  /* Unified: always update state enter time */
    mod->retry_count = 0;

    switch (new_state) {
        /* "Alive" states - evaluate online at transition moment */
        case CHG_LIB_STATE_IDLE:
        case CHG_LIB_STATE_STARTING:
        case CHG_LIB_STATE_STOPPING:
        case CHG_LIB_STATE_RUNNING:
        case CHG_LIB_STATE_FAULT:
            mod->view.online = (mod->view.last_rx_tick != 0 &&
                               (now - mod->view.last_rx_tick) <= OFFLINE_TIMEOUT_MS);
            break;

        /* "Dead" states - definitely offline */
        case CHG_LIB_STATE_OFFLINE:
        case CHG_LIB_STATE_RECOVERING:
            mod->view.online = false;
            break;
    }
}
```

### 3. process_module() - Unified Loop
```c
static void process_module(Mod *mod, uint32_t now) {
    if (!mod->view.enabled) return;

    /* GATEKEEPER: Check timeout FIRST */
    check_offline_timeout(mod, now);

    /* Then handle state-specific logic */
    switch (mod->view.state) {
        case CHG_LIB_STATE_OFFLINE:
            /* Try recover after delay */
            if ((now - mod->state_enter_tick) >= RECOVERY_DELAY_MS) {
                set_state(mod, CHG_LIB_STATE_RECOVERING, now);
            }
            break;

        case CHG_LIB_STATE_RECOVERING:
            /* Poll for recovery - if respond, will transition via set_state */
            send_recovery_poll(mod);
            mod->retry_count++;
            if (mod->retry_count >= MAX_RECOVERY_RETRY) {
                set_state(mod, CHG_LIB_STATE_OFFLINE, now);
            }
            break;

        case CHG_LIB_STATE_IDLE:
            /* Periodic poll for keepalive */
            if ((now - mod->last_poll_tick) >= POLL_INTERVAL_MS) {
                send_status_poll(mod);
                mod->last_poll_tick = now;
            }
            if (mod->should_run) {
                set_state(mod, CHG_LIB_STATE_STARTING, now);
            }
            break;

        case CHG_LIB_STATE_STARTING:
            /* Start sequence */
            break;

        case CHG_LIB_STATE_RUNNING:
            /* Monitor and update */
            break;

        case CHG_LIB_STATE_STOPPING:
            /* Wait for confirm off */
            break;

        case CHG_LIB_STATE_FAULT:
            /* Try recover if alarm cleared */
            break;
    }
}
```

---

# DRIVER 1: Maxwell (chg_lib_maxwell.c)

## Changes Required

### Step 1: Add last_poll_tick to internal struct (if missing)
**Location:** Around line 125 in MXR_Internal_t
```c
typedef struct {
    CHG_LIB_ModuleView_t view;
    uint8_t start_attempts;
    uint8_t retry_count;
    uint32_t state_enter_tick;
    uint32_t last_poll_tick;        // ADD: for IDLE polling
    float setpoint_voltage_v;
    float setpoint_current_limit;
    bool should_run;
    float rated_current_a;
    uint8_t poll_step;
} MXR_Internal_t;
```

### Step 2: Add check_offline_timeout() function
**Location:** After set_state() function (around line 294)
```c
/**
 * @brief Check for communication timeout
 * @note Called FIRST in process_module() for all states except OFFLINE/RECOVERING
 */
static void check_offline_timeout(MXR_Internal_t *m, uint32_t now) {
    if (m->view.state == CHG_LIB_STATE_OFFLINE ||
        m->view.state == CHG_LIB_STATE_RECOVERING) {
        return;
    }

    uint32_t since_rx = now - m->view.last_rx_tick;

    if (since_rx > MXR_OFFLINE_TIMEOUT_MS) {
        m->view.stats.timeout_count++;
        set_state(m, CHG_LIB_STATE_OFFLINE, now);
    }
}
```

### Step 3: Modify set_state() to use unified online logic
**Location:** Lines 252-294
**Change:** Update switch statement to use unified pattern

OLD:
```c
switch (new_state) {
    case CHG_LIB_STATE_IDLE:
        m->view.running = false;
        m->view.online = has_recent_rx(m, now);
        break;
    case CHG_LIB_STATE_STARTING:
        m->view.running = false;
        m->view.online = has_recent_rx(m, now);
        break;
    // ... existing code
```

NEW:
```c
switch (new_state) {
    case CHG_LIB_STATE_IDLE:
    case CHG_LIB_STATE_STARTING:
    case CHG_LIB_STATE_STOPPING:
    case CHG_LIB_STATE_RUNNING:
    case CHG_LIB_STATE_FAULT:
        m->view.running = (new_state == CHG_LIB_STATE_RUNNING);
        m->view.online = (m->view.last_rx_tick != 0 &&
                         (now - m->view.last_rx_tick) <= MXR_OFFLINE_TIMEOUT_MS);
        break;
    case CHG_LIB_STATE_OFFLINE:
    case CHG_LIB_STATE_RECOVERING:
        m->view.online = false;
        m->view.running = false;
        break;
```

### Step 4: Modify process_module() to call check_offline_timeout() FIRST
**Location:** Around line 423 in mx_process_module()

OLD:
```c
static void process_module(MXR_Internal_t *m, uint32_t now)
{
    if (!m->view.enabled) return;

    uint32_t since_rx = now - m->view.last_rx_tick;
    uint32_t since_state = now - m->state_enter_tick;

    switch (m->view.state) {
```

NEW:
```c
static void process_module(MXR_Internal_t *m, uint32_t now)
{
    if (!m->view.enabled) return;

    /* GATEKEEPER: Check timeout FIRST */
    check_offline_timeout(m, now);

    uint32_t since_state = now - m->state_enter_tick;

    switch (m->view.state) {
```

### Step 5: Remove duplicate timeout checks from individual cases

**REMOVE from RUNNING case (lines 471-474):**
```c
// DELETE:
if (since_rx > MXR_OFFLINE_TIMEOUT_MS) {
    m->view.stats.timeout_count++;
    set_state(m, CHG_LIB_STATE_OFFLINE, now);
}
```

**REMOVE from STARTING timeout check (lines 506-510):**
```c
// DELETE this duplicate check - now handled by check_offline_timeout()
```

**KEEP the else-if in FAULT recovery (line 497-501):**
```c
// KEEP - this is recovery logic, not pure timeout
if (m->view.alarm_flags == CHG_LIB_ALARM_NONE && since_rx < MXR_OFFLINE_TIMEOUT_MS) {
    set_state(m, m->setpoint.should_run ? CHG_LIB_STATE_STARTING : CHG_LIB_STATE_IDLE, now);
} else if (since_rx > MXR_OFFLINE_TIMEOUT_MS) {
    set_state(m, CHG_LIB_STATE_OFFLINE, now);
}
```

### Step 6: Add IDLE polling (Maxwell doesn't have it)
**Location:** In process_module(), case CHG_LIB_STATE_IDLE:
```c
case CHG_LIB_STATE_IDLE:
    /* Periodic poll for keepalive - ADD THIS */
    if ((now - m->last_poll_tick) >= 1000) {  /* 1 second poll */
        send_read(m, MXR_REG_STATUS);  /* or appropriate register */
        m->last_poll_tick = now;
    }
    if (m->setpoint.should_run && m->view.state != CHG_LIB_STATE_STARTING) {
        set_state(m, CHG_LIB_STATE_STARTING, now);
    }
    break;
```

### Step 7: Add STOPPING state handler (if missing)
**Location:** In set_state() switch - add case CHG_LIB_STATE_STOPPING:
```c
case CHG_LIB_STATE_STOPPING:
    m->view.running = false;
    m->view.online = (m->view.last_rx_tick != 0 &&
                     (now - m->view.last_rx_tick) <= MXR_OFFLINE_TIMEOUT_MS);
    break;
```

---

# DRIVER 2: Lianming (chg_lib_lianming.c)

## Changes Required

### Step 1: Rename last_state_tick to state_enter_tick
**Location:** Line 135 in LM_Module_t

OLD:
```c
uint32_t last_state_tick;
```

NEW:
```c
uint32_t state_enter_tick;
```

**Then update ALL references:**
- Line 212: `mod->last_state_tick = now;` → `mod->state_enter_tick = now;`
- Line 477, 478, 482, 483, 486, 487: Same replacement
- Line 504: `mod->last_state_tick` → `mod->state_enter_tick`

### Step 2: Add last_poll_tick to internal struct
**Location:** Around line 135
```c
typedef struct {
    CHG_LIB_ModuleView_t view;
    uint8_t start_attempts;
    uint8_t retry_count;
    uint32_t state_enter_tick;
    uint32_t last_poll_tick;        // ADD: for IDLE polling
    uint8_t diag_counter;
    // ... existing fields
} LM_Module_t;
```

### Step 3: Add check_offline_timeout() function
**Location:** After set_state() function
```c
/**
 * @brief Check for communication timeout
 * @note Called FIRST in process_module() for all states except OFFLINE/RECOVERING
 */
static void check_offline_timeout(LM_Module_t *mod, uint32_t now) {
    if (mod->view.state == CHG_LIB_STATE_OFFLINE ||
        mod->view.state == CHG_LIB_STATE_RECOVERING) {
        return;
    }

    uint32_t since_rx = now - mod->view.last_rx_tick;

    if (since_rx > LM_KEEPALIVE_TIMEOUT_MS) {
        mod->view.stats.timeout_count++;
        set_state(mod, CHG_LIB_STATE_OFFLINE, now);
    }
}
```

### Step 4: Modify set_state() to use unified online logic
**Location:** Lines 200-240

Change switch to unified pattern (similar to Maxwell):

OLD:
```c
switch (state) {
    case CHG_LIB_STATE_IDLE:
        mod->view.running = false;
        break;
    case CHG_LIB_STATE_STARTING:
        mod->view.online = true;
        mod->view.running = false;
        break;
    // ...
```

NEW:
```c
switch (state) {
    case CHG_LIB_STATE_IDLE:
    case CHG_LIB_STATE_STARTING:
    case CHG_LIB_STATE_STOPPING:
    case CHG_LIB_STATE_RUNNING:
    case CHG_LIB_STATE_FAULT:
        mod->view.running = (state == CHG_LIB_STATE_RUNNING);
        mod->view.online = (mod->view.last_rx_tick != 0 &&
                           (now - mod->view.last_rx_tick) <= LM_KEEPALIVE_TIMEOUT_MS);
        break;
    case CHG_LIB_STATE_OFFLINE:
    case CHG_LIB_STATE_RECOVERING:
        mod->view.online = false;
        mod->view.running = false;
        break;
```

### Step 5: Modify process_module() to call check_offline_timeout() FIRST
**Location:** Around line 443

OLD:
```c
static void process_module(uint8_t idx, uint32_t now)
{
    LM_Module_t *mod = &g_modules[idx];
    if (!mod->view.enabled) {
        return;
    }

    switch (mod->view.state) {
```

NEW:
```c
static void process_module(uint8_t idx, uint32_t now)
{
    LM_Module_t *mod = &g_modules[idx];
    if (!mod->view.enabled) {
        return;
    }

    /* GATEKEEPER: Check timeout FIRST */
    check_offline_timeout(mod, now);

    switch (mod->view.state) {
```

### Step 6: Remove duplicate timeout checks

**REMOVE from RUNNING case (lines 493-496):**
```c
// DELETE:
if ((now - mod->view.last_rx_tick) > LM_KEEPALIVE_TIMEOUT_MS) {
    mod->view.stats.timeout_count++;
    set_state(mod, CHG_LIB_STATE_OFFLINE, now);
}
```

**REMOVE from FAULT case (lines 518-521):**
```c
// DELETE:
if ((now - mod->view.last_rx_tick) > LM_KEEPALIVE_TIMEOUT_MS) {
    mod->view.stats.timeout_count++;
    set_state(mod, CHG_LIB_STATE_OFFLINE, now);
}
```

### Step 7: Add IDLE polling (if not present)
**Check if lm_read_status() is called in IDLE** - if yes, keep it. If no, add polling.

---

# DRIVER 3: Tonhe (chg_lib_tonhe.c)

## Changes Required

### Step 1: Add last_poll_tick to internal struct
**Location:** Around line 73 in TONHE_Internal_t
```c
typedef struct {
    CHG_LIB_ModuleView_t view;
    uint8_t retry_count;
    uint8_t stop_retry_count;
    uint32_t stop_tick;
    uint32_t state_enter_tick;
    uint32_t last_poll_tick;        // ADD: for IDLE polling
    bool should_run;
    // ... existing fields
} TONHE_Internal_t;
```

### Step 2: Add check_offline_timeout() function
**Location:** After set_state() function
```c
/**
 * @brief Check for communication timeout
 * @note Called FIRST in process_module() for all states except OFFLINE/RECOVERING
 */
static void check_offline_timeout(TONHE_Internal_t *mod, uint32_t now) {
    if (mod->view.state == CHG_LIB_STATE_OFFLINE ||
        mod->view.state == CHG_LIB_STATE_RECOVERING) {
        return;
    }

    uint32_t since_rx = now - mod->view.last_rx_tick;

    if (since_rx > TONHE_OFFLINE_TIMEOUT_MS) {
        mod->view.stats.timeout_count++;
        set_state(mod, CHG_LIB_STATE_OFFLINE, now);
    }
}
```

### Step 3: Modify set_state() to use unified online logic
**Location:** Lines 426-461

Change switch to unified pattern:

OLD:
```c
switch (st) {
    case CHG_LIB_STATE_IDLE:
        mod->view.running = false;
        break;
    case CHG_LIB_STATE_FAULT:
        mod->view.online = true;  // Fault is valid response
        mod->view.running = false;
        break;
    // ...
```

NEW:
```c
switch (st) {
    case CHG_LIB_STATE_IDLE:
    case CHG_LIB_STATE_STARTING:
    case CHG_LIB_STATE_STOPPING:
    case CHG_LIB_STATE_RUNNING:
    case CHG_LIB_STATE_FAULT:
        mod->view.running = (st == CHG_LIB_STATE_RUNNING);
        mod->view.online = (mod->view.last_rx_tick != 0 &&
                           (now - mod->view.last_rx_tick) <= TONHE_OFFLINE_TIMEOUT_MS);
        break;
    case CHG_LIB_STATE_OFFLINE:
    case CHG_LIB_STATE_RECOVERING:
        mod->view.online = false;
        mod->view.running = false;
        break;
```

### Step 4: Modify process_module() to call check_offline_timeout() FIRST
**Location:** Around line 463

OLD:
```c
static void process_module(TONHE_Internal_t *mod, uint32_t now)
{
    if (!mod->view.enabled) return;

    uint32_t since_rx = now - mod->view.last_rx_tick;

    switch (mod->view.state) {
```

NEW:
```c
static void process_module(TONHE_Internal_t *mod, uint32_t now)
{
    if (!mod->view.enabled) return;

    /* GATEKEEPER: Check timeout FIRST */
    check_offline_timeout(mod, now);

    switch (mod->view.state) {
```

### Step 5: Remove duplicate timeout checks

**FIX STARTING case (lines 506-510):**

Currently has AND logic - change to rely on check_offline_timeout():

OLD:
```c
/* Check for offline timeout - CURRENTLY HAS BUG: AND logic */
if ((now - mod->last_tx_tick) > TONHE_OFFLINE_TIMEOUT_MS &&
    (now - mod->view.last_rx_tick) > TONHE_OFFLINE_TIMEOUT_MS) {
```

DELETE these lines - now handled by check_offline_timeout()

**REMOVE from RUNNING case (lines 515-519):**
```c
// DELETE - now in check_offline_timeout()
if (since_rx > TONHE_OFFLINE_TIMEOUT_MS) {
    mod->view.stats.timeout_count++;
    mod->view.online = false;
    set_state(mod, CHG_LIB_STATE_OFFLINE, now);
}
```

**REMOVE from STOPPING case:**
```c
// Check for offline timeout - DELETE
if (since_rx > TONHE_OFFLINE_TIMEOUT_MS) {
```

**REMOVE from FAULT case (lines 579-581):**
```c
// DELETE - now in check_offline_timeout()
```

### Step 6: Fix since_rx variable
**Location:** After refactoring, since_rx is no longer used in process_module()

Remove the line:
```c
uint32_t since_rx = now - mod->view.last_rx_tick;
```

This is now handled inside check_offline_timeout()

### Step 7: Add IDLE polling
**Location:** In process_module(), case CHG_LIB_STATE_IDLE:
```c
case CHG_LIB_STATE_IDLE:
    /* Periodic poll for keepalive - ADD THIS */
    if ((now - mod->last_poll_tick) >= 1000) {  /* 1 second poll */
        send_param_set(mod);  /* or appropriate status poll */
        mod->last_poll_tick = now;
    }
    if (mod->should_run) {
        mod->retry_count = 0;
        send_param_set(mod);
        send_specific_start_stop(mod, true);
        set_state(mod, CHG_LIB_STATE_STARTING, now);
    }
    break;
```

---

# Summary of Changes by Driver

| Step | Maxwell | Lianming | Tonhe |
|------|---------|----------|-------|
| 1. Add last_poll_tick | ✅ Add field | ✅ Add field | ✅ Add field |
| 2. Add check_offline_timeout() | ✅ Add function | ✅ Add function | ✅ Add function |
| 3. Modify set_state() | ✅ Update switch | ✅ Update switch + rename field | ✅ Update switch |
| 4. Modify process_module() | ✅ Call check first | ✅ Call check first | ✅ Call check first |
| 5. Remove duplicate timeouts | ✅ Clean RUNNING/STARTING | ✅ Clean RUNNING/FAULT | ✅ Clean ALL |
| 6. Add IDLE polling | ✅ Add polling | ⚠️ Check existing | ✅ Add polling |
| 7. Fix since_rx usage | Remove unused var | N/A | Remove unused var |

---

# Testing Recommendations

After implementation:

1. **Unit Test: Timeout Detection**
   - Disconnect CAN cable → module should transition to OFFLINE within timeout period

2. **Unit Test: State Transitions**
   - Start module → verify transitions: IDLE → STARTING → RUNNING
   - Stop module → verify transitions: RUNNING → STOPPING → IDLE

3. **Unit Test: Recovery**
   - Force OFFLINE → verify recovery attempt after RECOVERY_DELAY_MS

4. **Integration Test: Multiple Modules**
   - Start with multiple modules → disconnect one → verify only that one shows offline

5. **Integration Test: Long Running**
   - Run for extended period → verify no false timeout triggers

---

# Files to Modify

1. `charger/lib/chg_lib/Src/chg_lib_maxwell.c`
2. `charger/lib/chg_lib/Src/chg_lib_lianming.c`
3. `charger/lib/chg_lib/Src/chg_lib_tonhe.c`
