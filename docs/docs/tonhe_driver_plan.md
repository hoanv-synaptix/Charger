# Plan: Driver TONHE V1.3 Implementation

## 1. Mục tiêu

Viết driver cho module sạc TONHE theo chuẩn giao thức V1.3, tuân thủ kiến trúc canonical đã xây dựng ở Sprint B.

## 2. So sánh Maxwell vs TONHE

| Khía cạnh | Maxwell MXR | TONHE V1.3 |
|------------|------------|-------------|
| **Frame Format** | Custom 29-bit Extended | SAE J1939-based 29-bit |
| **Protocol Type** | Request/Response (polling registers) | Event-driven (periodic + trigger) |
| **Baudrate** | 125 Kbps | 125 Kbps (isolated) |
| **Module Address** | 0-63 (DIP switch) | 1-240 |
| **Controller Address** | 0xF0 | 0xA0 |
| **Broadcast** | 0xFF | 0xFF |
| **Message Flow** | Master poll → Module respond | Module periodic → Master command |
| **Register Access** | READ/SET commands | Message-based parameters |
| **Status Update** | On poll | 500ms periodic + on change |

## 3. Architecture

### 3.1 File Structure

```
App/Inc/
├── driver_tonhe.h      ← NEW: declare CHG_TonheDriverOps()
└── ...

App/Src/
├── driver_tonhe.c      ← NEW: implement CHG_DriverOps_t
└── ...
```

### 3.2 Driver Interface

Driver implement `CHG_DriverOps_t` interface:

```c
static const CHG_DriverOps_t g_tonhe_ops = {
    .name = "tonhe",
    .init = tonhe_init,
    .add_module = tonhe_add_module,
    .remove_module = tonhe_remove_module,
    .set_voltage = tonhe_set_voltage,
    .set_current_limit = tonhe_set_current_limit,
    .start = tonhe_start,
    .stop = tonhe_stop,
    .set_voltage_all = tonhe_set_voltage_all,
    .set_current_limit_all = tonhe_set_current_limit_all,
    .start_all = tonhe_start_all,
    .stop_all = tonhe_stop_all,
    .emergency_stop = tonhe_emergency_stop,
    .process = tonhe_process,
    .feed_frame = tonhe_feed_frame,
    .get_system_summary = tonhe_get_system_summary,
    .get_module_count = tonhe_get_module_count,
    .get_module_view = tonhe_get_module_view,
};
```

## 4. Chi tiết implementation

### 4.1 Constants (driver_tonhe.h)

```c
/* CAN IDs */
#define TONHE_ADDR_CONTROLLER    0xA0
#define TONHE_ADDR_BROADCAST     0xFF
#define TONHE_PGN_STATUS         0x000100
#define TONHE_PGN_CONFIRM        0x000200
#define TONHE_PGN_AC_PHASE       0x000B00
#defineTONHE_PGN_EXTENDED       0x009100
#define TONHE_PGN_BROADCAST_CMD  0x000300
#define TONHE_PGN_PARAM_SET      0x000400
#define TONHE_PGN_TIMING         0x000500
#define TONHE_PGN_SPECIFIC_CMD   0x000600
#define TONHE_PGN_ADDR_SET       0x000900

/* Priority */
#define TONHE_PRIORITY_STATUS    6
#define TONHE_PRIORITY_CMD       2
#define TONHE_PRIORITY_EXTENDED  7

/* Scale */
#define TONHE_VOLTAGE_SCALE      0.1f    // V per bit
#define TONHE_CURRENT_SCALE      0.01f   // A per bit

/* Limits */
#define TONHE_MAX_VOLTAGE_V     750.0f
#define TONHE_MAX_CURRENT_A      500.0f
#define TONHE_OFFLINE_TIMEOUT_MS 2000
```

### 4.2 Internal Types

```c
typedef struct {
    CHG_ModuleView_t view;

    /* Driver-private state */
    uint8_t group;
    uint8_t poll_step;
    uint8_t retry_count;
    bool should_run;
    uint32_t last_tx_tick;
} TONHE_Internal_t;
```

### 4.3 Message Parsing

| PGN | Handler | Data Extracted |
|------|---------|----------------|
| 0x000100 | `parse_status()` | voltage, current, alarm_status |
| 0x000200 | `parse_confirm()` | command acknowledged |
| 0x000B00 | `parse_ac_phase()` | phase_voltages, temp_ambient |
| 0x009100 | `parse_extended()` | extended_alarms |

### 4.4 Command Sending

| PGN | Function | Purpose |
|-----|----------|---------|
| 0x000600 | `send_start_stop()` | Specific module start/stop |
| 0x000400 | `send_param_set()` | Broadcast voltage/current |
| 0x000300 | `send_broadcast_start_stop()` | Broadcast start/stop |

### 4.5 Alarm Mapping

```c
static CHG_AlarmFlag_t tonhe_parse_alarm(uint16_t fault_bits, uint8_t pfc_bits)
{
    CHG_AlarmFlag_t flags = CHG_ALARM_NONE;

    if (fault_bits & (1 << 0)) flags |= CHG_ALARM_AC_UNDER_VOLT;
    if (fault_bits & (1 << 2)) flags |= CHG_ALARM_OVER_VOLTAGE_OUT;
    if (fault_bits & (1 << 3)) flags |= CHG_ALARM_OVER_VOLTAGE_OUT;
    if (fault_bits & (1 << 4)) flags |= CHG_ALARM_HW_FAULT;
    if (fault_bits & (1 << 5)) flags |= CHG_ALARM_OVER_TEMP;
    if (fault_bits & (1 << 6)) flags |= CHG_ALARM_HW_FAULT;
    if (fault_bits & (1 << 7)) flags |= CHG_ALARM_HW_FAULT;
    if (fault_bits & (1 << 15)) flags |= CHG_ALARM_SHORT_CIRCUIT;
    if (fault_bits & (1 << 9)) flags |= CHG_ALARM_COMM_FAIL;

    return flags;
}
```

## 5. FSM Implementation

### 5.1 Process Loop

```c
static void process_module(TONHE_Internal_t *mod, uint32_t now)
{
    switch (mod->view.state) {

    case CHG_STATE_IDLE:
        /* Send parameter set */
        if (mod->should_run) {
            send_param_set(mod);
            set_state(mod, CHG_STATE_STARTING, now);
        }
        break;

    case CHG_STATE_STARTING:
        /* Wait for confirm, retry if needed */
        if (mod->retry_count < 3 && (now - mod->last_tx_tick) > 1000) {
            send_start_stop(mod, true);
            mod->retry_count++;
        }
        break;

    case CHG_STATE_RUNNING:
        /* Monitor for offline */
        if ((now - mod->view.last_rx_tick) > TONHE_OFFLINE_TIMEOUT_MS) {
            set_state(mod, CHG_STATE_OFFLINE, now);
        }
        break;

    case CHG_STATE_OFFLINE:
        /* Try to recover */
        if ((now - mod->last_tx_tick) > 3000) {
            send_param_set(mod);
            set_state(mod, CHG_STATE_RECOVERING, now);
        }
        break;
    }
}
```

### 5.2 CAN Frame Feed

```c
static void tonhe_feed_frame(uint32_t ext_id, const uint8_t *data, uint8_t dlc)
{
    uint8_t src = ext_id & 0xFF;
    uint16_t pgn = (ext_id >> 8) & 0xFFFF;

    switch (pgn) {
    case 0x0001: parse_status(src, data); break;
    case 0x0002: parse_confirm(src, data); break;
    case 0x000B: parse_ac_phase(src, data); break;
    case 0x0091: parse_extended(src, data); break;
    }
}
```

## 6. Steps to Implement

### Step 1: Create driver_tonhe.h
- [ ] Define constants (addresses, PGNs, scales)
- [ ] Declare `CHG_TonheDriverOps()`

### Step 2: Create driver_tonhe.c
- [ ] Define internal types
- [ ] Implement `tonhe_init()`
- [ ] Implement `tonhe_add_module()` / `tonhe_remove_module()`
- [ ] Implement message parsers (M_C_1~4)
- [ ] Implement command senders (C_M_1, C_M_2, C_M_24)
- [ ] Implement `tonhe_process()` FSM
- [ ] Implement `tonhe_feed_frame()` router
- [ ] Implement `tonhe_get_system_summary()`
- [ ] Implement `tonhe_get_module_view()`
- [ ] Implement `CHG_DriverOps_t` table

### Step 3: Integrate
- [ ] Add to CMakeLists.txt
- [ ] Update app_charger.c: register TONHE driver
- [ ] Update driver registry: add `CHG_DRIVER_TONHE = 3`

### Step 4: Test
- [ ] Build with CMake
- [ ] Flash to board
- [ ] Connect to TONHE module (or mock)
- [ ] Verify PING/PONG works
- [ ] Test start/stop sequence
- [ ] Verify status parsing

## 7. Files to Create/Modify

| File | Action |
|------|--------|
| `docs/tonhe_protocol_analysis.md` | ✅ Created |
| `docs/tonhe_driver_plan.md` | ✅ Created |
| `App/Inc/driver_tonhe.h` | CREATE |
| `App/Src/driver_tonhe.c` | CREATE |
| `App/Inc/charger_core.h` | MODIFY: add `CHG_DRIVER_TONHE = 3` |
| `App/Src/app_charger.c` | MODIFY: register TONHE driver |
| `CMakeLists.txt` | MODIFY: add driver_tonhe.c |

## 8. Key Differences from Maxwell

| Aspect | Maxwell | TONHE |
|--------|---------|-------|
| Polling | READ register command | Wait for periodic M_C_1 |
| Setting | SET register command | Send C_M_24 (start) + C_M_2 (param) |
| Confirmation | None | M_C_2 response |
| Address | Hardcoded DIP | Can set via C_M_23 |
| Multi-module | Round-robin poll | All modules report independently |
| Timeout | No response to poll | No M_C_1 within 2s |

## 9. Testing Strategy

### 9.1 Unit Tests
- Parse M_C_1 status frame correctly
- Parse M_C_2 confirm frame correctly
- Build C_M_24 command frame correctly
- Alarm bit mapping accuracy

### 9.2 Integration Tests
- Start module → receives confirm → status shows online
- Set voltage → module outputs correct voltage
- Module offline → detect and recover

### 9.3 Manual Tests
- Connect real TONHE module
- Send start command
- Verify charging starts
- Verify status updates
- Verify fault detection
