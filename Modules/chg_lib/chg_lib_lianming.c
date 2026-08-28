/**
 * @file chg_lib_lianming.c
 * @brief Lianming Power Digital Charging Module Driver
 * @note Protocol V2.0 - CAN 2.0B Extended Frame, 125Kbps
 *
 * Hardware Interface:
 *   - CAN bus: 125 Kbps, Extended 29-bit frame
 *   - Isolation: Required (isolated CAN transceiver)
 *
 * CAN Frame Format (29-bit Extended ID):
 *   Base command ID OR module address (lower 7 bits).
 *
 * TX Command ID:  0x1907C080 | module_addr
 * RX Response ID: 0x1807C080 | module_addr
 *
 * Data Format - Set Output (CMD=0):
 *   | Byte 0 | Byte 1-3          | Byte 4-7          |
 *   | CMD    | Current (mA)      | Voltage (mV)     |
 *   | 0x00   | 0x015F90 (90A)   | 0x000186A0 (100V) |
 *
 * Data Format - Read Status (CMD=1):
 *   | Byte 0 | Byte 1    | Byte 2-3     | Byte 4-5    | Byte 6-7    |
 *   | CMD    | Status    | Current(A)   | Voltage(V) | Fault Flags |
 *   | 0x01   | 0xFF     | 0x0117(27.9)| 0x03E8(100) | 0x0000      |
 *
 * Data Format - Start/Stop (CMD=2):
 *   | Byte 0 | Byte 1-6 | Byte 7     |
 *   | CMD    | Reserved | Start/Stop |
 *   | 0x02   | 0x00    | 0x55/0xAA  |
 *
 * References:
 *   - "Lianming Power Digital Power Module CAN Communication Protocol V2.0.pdf"
 *   - "Lian.md" (Vietnamese translation)
 *
 * Example:
 *   Set 100V, 90A:  ID=0x1907C083, Data: 00 01 5F 90 00 01 86 A0
 *   Start module 1: ID=0x1907C081, Data: 02 00 00 00 00 00 00 55
 *   Stop module 1:  ID=0x1907C081, Data: 02 00 00 00 00 00 00 AA
 */

#include "chg_lib_driver_lianming.h"
#include <math.h>
#include "chg_lib_can_backend.h"
#include "priv/chg_lib_core_priv.h"
#include "priv/chg_lib_protocol.h"
#include "bsp_sys.h"
#include <string.h>

/* ============== Lianming-specific CAN Constants ============== */
/* Protocol V2.0 - CAN 2.0B Extended Frame, 125Kbps */

#define LM_MODULE_MIN_ADDR       1U           /* Module address range */
#define LM_MODULE_MAX_ADDR       60U
#define LM_ADDR_MASK             0x7FU

/* CAN ID Base Addresses */
#define LM_CMD_BASE             0x1907C080U    /* Command TX ID base */
#define LM_RESP_BASE            0x1807C080U    /* Response RX ID base */

/* AC Input and Temp read ID bases (command only, no response) */
#define LM_AC_CMD_BASE          0x1907A080U    /* Read AC input voltage (0x1907A0xx) */
#define LM_AC_RESP_BASE         0x1807A080U    /* Read AC input voltage response (0x1807A0xx) */
#define LM_TEMP_CMD_BASE        0x19008080U    /* Read ambient temperature (0x190080xx) */
#define LM_TEMP_RESP_BASE       0x18008080U    /* Read temp response (0x180080xx) */

/* Command Codes (Byte 0 of data) */
#define LM_CMD_SET_OUTPUT       0x00U        /* Set voltage/current */
#define LM_CMD_READ_INFO        0x01U        /* Read status */
#define LM_CMD_START_STOP       0x02U        /* Start/Stop */

/* Start/Stop values (Byte 7 of data, per Lianming V2.0 protocol) */
#define LM_START_VALUE          0x55U        /* Start */
#define LM_STOP_VALUE           0xAAU        /* Stop */

/* ============== Configuration ============== */

#define LM_MAX_MODULES          8U
#define LM_WARNING_TIMEOUT_MS   2000U
#define LM_OFFLINE_TIMEOUT_MS   10000U
#define LM_RECOVERY_DELAY_MS    3000U
#define LM_STEP_DELAY_MS        50U
#define LM_START_CONFIRM_TIMEOUT_MS 500U
#define LM_MAX_RETRY            3U
#define LM_STOP_MAX_RETRY       5U      /* BUGFIX B-07: stop-confirm retry cap before FAULT */
#define LM_DIAG_INTERVAL       4U      /* Poll AC/temp every N cycles */

/* ============== CAN Frame ID Builder ============== */

/**
 * @brief  Build TX CAN ID for Lianming
 * @note   Format: Command ID (14 bits) | Module Address (7 bits)
 *         TX: 0x1907C080 + addr
 *         RX: 0x1807C080 + addr
 * @param  module_addr  Module address (1-60, 0 for broadcast)
 * @param  is_response  true for response ID, false for command ID
 * @retval 29-bit CAN ID
 */
static uint32_t lm_build_can_id(uint8_t module_addr, bool is_response)
{
    uint32_t base = is_response ? LM_RESP_BASE : LM_CMD_BASE;
    return base | (module_addr & LM_ADDR_MASK);
}

/**
 * @brief  Extract module address from response ID
 * @param  ext_id  Response CAN ID
 * @retval Module address (1-60)
 */
static uint8_t lm_parse_response_addr(uint32_t ext_id)
{
    return (uint8_t)(ext_id & LM_ADDR_MASK);
}

/**
 * @brief  Convert Lianming voltage payload to float (V)
 * @note   Response format: bytes 4-5 = voltage (0.1V/bit, big-endian)
 */
static float lm_payload_to_voltage(const uint8_t *data)
{
    uint16_t value = CHG_LIB_ProtocolBEToU16(&data[4]);  /* Voltage at bytes 4-5 */
    return (float)value / 10.0f;  /* 0.1V/bit -> V */
}

/**
 * @brief  Convert Lianming current payload to float (A)
 * @note   Response format: bytes 2-3 = current (0.1A/bit, big-endian)
 *         Example: 0x0117 (279) = 27.9A
 */
static float lm_payload_to_current(const uint8_t *data)
{
    uint16_t value = CHG_LIB_ProtocolBEToU16(&data[2]);  /* Current at bytes 2-3 */
    return (float)value / 10.0f;  /* 0.1A/bit -> A */
}

/* ============== Internal Types ============== */

typedef struct {
    CHG_LIB_ModuleView_t view;
    uint32_t state_enter_tick;   /* Unified: renamed from state_enter_tick */
    uint32_t last_poll_tick;     /* For IDLE polling */
    uint8_t retry_count;
    uint8_t start_attempts;
    bool should_run;
    float voltage_setpoint;
    float rated_current_a;
    uint8_t diag_counter;        /* Counts cycles, triggers AC/temp read */
    uint8_t diag_step;           /* 0=idle, 1=AC read sent, 2=temp read sent */
    uint32_t recovery_start_rx_count;
} LM_Module_t;

static LM_Module_t g_modules[LM_MAX_MODULES];
static volatile uint8_t g_module_count = 0;

/* ============== Helper Functions ============== */

/* CHG_LIB_NowTick() removed â€” use CHG_LIB_NowTick() from shared helpers */

/* Note: Lianming uses CMD=1 to read all status at once, no need for poll registers */

static CHG_LIB_AlarmFlag_t parse_lianming_alarm(uint16_t raw_alarm)
{
    CHG_LIB_AlarmFlag_t flags = CHG_LIB_ALARM_NONE;
    /* raw_alarm is (Byte6 << 8) | Byte7 */
    
    /* Byte 7 bits (0-7 in raw_alarm) */
    if (raw_alarm & (1U << 1)) flags |= CHG_LIB_ALARM_HW_FAULT;         /* Module fault */
    if (raw_alarm & (1U << 3)) flags |= CHG_LIB_ALARM_HW_FAULT;         /* Fan fault */
    if (raw_alarm & (1U << 4)) flags |= CHG_LIB_ALARM_HW_FAULT;         /* Input overvoltage */
    if (raw_alarm & (1U << 5)) flags |= CHG_LIB_ALARM_AC_UNDER_VOLT;    /* Input under-voltage */
    if (raw_alarm & (1U << 6)) flags |= CHG_LIB_ALARM_OVER_VOLTAGE_OUT; /* Output overvoltage */
    if (raw_alarm & (1U << 7)) flags |= CHG_LIB_ALARM_HW_FAULT;         /* Output under-voltage */
    
    /* Byte 6 bits (8-15 in raw_alarm): Byte6 bit5/6/7 map to raw bits 13/14/15. */
    if (raw_alarm & (1U << 13)) flags |= CHG_LIB_ALARM_OVER_CURR_OUT;   /* Overcurrent protection */
    if (raw_alarm & (1U << 14)) flags |= CHG_LIB_ALARM_OVER_TEMP;       /* Over temperature */
    
    return flags;
}

static CHG_LIB_State_t lm_state_from_flags(const LM_Module_t *mod, bool fault, bool recovering)
{
    if (recovering) {
        return CHG_LIB_STATE_RECOVERING;
    }
    if (fault) {
        return CHG_LIB_STATE_FAULT;
    }
    if (mod == 0 || !mod->view.online) {
        return CHG_LIB_STATE_OFFLINE;
    }
    if (mod->should_run) {
        return mod->view.running ? CHG_LIB_STATE_RUNNING : CHG_LIB_STATE_STARTING;
    }
    return mod->view.running ? CHG_LIB_STATE_STOPPING : CHG_LIB_STATE_IDLE;
}

static void clear_view(CHG_LIB_ModuleView_t *view)
{
    memset(view, 0, sizeof(*view));
    view->enabled = true;
    view->current_limit = 1.0f;
    view->state = CHG_LIB_STATE_IDLE;
}

static void set_state(LM_Module_t *mod, CHG_LIB_State_t state, uint32_t now)
{
    if (mod->view.state == state) return;

    /* Track consecutive start attempts (only increment on transition TO STARTING) */
    if (state == CHG_LIB_STATE_STARTING) {
        mod->start_attempts++;
    } else if (state == CHG_LIB_STATE_IDLE || state == CHG_LIB_STATE_OFFLINE || state == CHG_LIB_STATE_FAULT) {
        mod->start_attempts = 0;
    }

    mod->view.state = state;
    mod->state_enter_tick = now;
    mod->retry_count = 0;

    /* Unified online/running flag management */
    switch (state) {
        case CHG_LIB_STATE_IDLE:
        case CHG_LIB_STATE_STARTING:
        case CHG_LIB_STATE_STOPPING:
        case CHG_LIB_STATE_WARNING:
            mod->view.running = (state == CHG_LIB_STATE_WARNING);
            mod->view.online = (mod->view.last_rx_tick != 0 &&
                              (now - mod->view.last_rx_tick) <= LM_OFFLINE_TIMEOUT_MS);
            break;

        case CHG_LIB_STATE_FAULT:
            mod->view.running = false;
            mod->view.online = (mod->view.last_rx_tick != 0 &&
                              (now - mod->view.last_rx_tick) <= LM_OFFLINE_TIMEOUT_MS);
            /* BUGFIX B-08: same 5-clean-reads debounce as
             * OFFLINE->RECOVERING -- see chg_lib_maxwell.c's matching
             * comment for the full rationale. */
            mod->recovery_start_rx_count = mod->view.stats.rx_count;
            break;

        case CHG_LIB_STATE_RUNNING:
            mod->view.running = true;
            mod->view.online = true;
            break;

        case CHG_LIB_STATE_OFFLINE:
        case CHG_LIB_STATE_RECOVERING:
            mod->view.running = false;
            mod->view.online = false;
            if (state == CHG_LIB_STATE_RECOVERING) {
                mod->recovery_start_rx_count = mod->view.stats.rx_count;
            }
            break;
    }
}

/**
 * @brief Check for communication timeout and transition to OFFLINE if needed
 * @note Called FIRST in process_module() for all States except OFFLINE/RECOVERING
 */
static void check_offline_timeout(LM_Module_t *mod, uint32_t now) {
    /* See the matching comment in chg_lib_tonhe.c's check_offline_timeout()
     * -- keep `online` a plain, always-fresh function of last_rx_tick even
     * while should_run is false and the state-machine watchdog below is
     * gated off. */
    mod->view.online = (mod->view.last_rx_tick != 0 &&
                        (now - mod->view.last_rx_tick) <= LM_OFFLINE_TIMEOUT_MS);

    CHG_LIB_State_t new_state;
    bool timeout_flag;
    CHG_LIB_FSM_CheckOfflineTimeout(
        &mod->view.state,
        mod->view.last_rx_tick,
        mod->should_run,
        LM_OFFLINE_TIMEOUT_MS,
        LM_WARNING_TIMEOUT_MS,
        now,
        &new_state,
        &timeout_flag
    );
    if (timeout_flag) {
        mod->view.stats.timeout_count++;
    }
    if (new_state != mod->view.state) {
        set_state(mod, new_state, now);
    }
}

/**
 * @brief  Send CAN frame to Lianming module (command format)
 * @note   Lianming format: CMD | Current(mA) | Voltage(mV)
 */
/**
 * @brief  Send read status command
 */
static void lm_send_read(LM_Module_t *mod)
{
    uint8_t data[8] = { LM_CMD_READ_INFO, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint32_t ext_id = lm_build_can_id(mod->view.addr, false);
    if (CHG_LIB_CanBackend_Transmit(ext_id, data, 8)) {
        mod->view.stats.tx_count++;
        mod->view.last_tx_tick = CHG_LIB_NowTick(); /* BUGFIX B-17: was never updated here */
    }
}

/**
 * @brief  Set output voltage and current
 * @note   Lianming format: CMD=0, Byte 1-3: Current(mA), Byte 4-7: Voltage(mV)
 */
static void lm_set_output(uint8_t idx, float voltage_v, float current_a)
{
    /* Lianming format:
     * Byte 0: CMD (0x00 = Set)
     * Byte 1-3: Current (mA, 3 bytes big-endian)
     * Byte 4-7: Voltage (mV, 4 bytes big-endian)
     * Example: 00 01 5F 90 00 01 86 A0 = 90A, 100V
     */
    uint32_t voltage_mv = (uint32_t)(voltage_v * 1000.0f);  /* V -> mV */
    uint32_t current_ma = (uint32_t)(current_a * 1000.0f);  /* A -> mA */
    LM_Module_t *mod = &g_modules[idx];

    uint8_t data[8];
    data[0] = LM_CMD_SET_OUTPUT;    /* CMD = 0 */
    data[1] = (uint8_t)(current_ma >> 16);  /* Current byte 2 */
    data[2] = (uint8_t)(current_ma >> 8);   /* Current byte 1 */
    data[3] = (uint8_t)(current_ma);        /* Current byte 0 */
    data[4] = (uint8_t)(voltage_mv >> 24);  /* Voltage byte 3 */
    data[5] = (uint8_t)(voltage_mv >> 16);  /* Voltage byte 2 */
    data[6] = (uint8_t)(voltage_mv >> 8);   /* Voltage byte 1 */
    data[7] = (uint8_t)(voltage_mv);        /* Voltage byte 0 */

    uint32_t ext_id = lm_build_can_id(mod->view.addr, false);
    if (CHG_LIB_CanBackend_Transmit(ext_id, data, 8)) {
        mod->view.stats.tx_count++;
        mod->view.last_tx_tick = CHG_LIB_NowTick(); /* BUGFIX B-17 */
    }
}

/**
 * @brief  Start module
 */
static void lm_start_module(uint8_t idx)
{
    LM_Module_t *mod = &g_modules[idx];
    uint8_t data[8] = { LM_CMD_START_STOP, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, LM_START_VALUE };
    uint32_t ext_id = lm_build_can_id(mod->view.addr, false);
    if (CHG_LIB_CanBackend_Transmit(ext_id, data, 8)) {
        mod->view.stats.tx_count++;
        mod->view.last_tx_tick = CHG_LIB_NowTick(); /* BUGFIX B-17 */
    }
}

/**
 * @brief  Stop module
 */
static void lm_stop_module(uint8_t idx)
{
    LM_Module_t *mod = &g_modules[idx];
    uint8_t data[8] = { LM_CMD_START_STOP, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, LM_STOP_VALUE };
    uint32_t ext_id = lm_build_can_id(mod->view.addr, false);
    if (CHG_LIB_CanBackend_Transmit(ext_id, data, 8)) {
        mod->view.stats.tx_count++;
        mod->view.last_tx_tick = CHG_LIB_NowTick(); /* BUGFIX B-17 */
    }
}

/**
 * @brief  Send read AC input voltage command (Vab, Vbc, Vca)
 * @note   Response format: CMD=0x31, Byte2-3=Vab/32, Byte4-5=Vbc/32, Byte6-7=Vca/32
 */
static void lm_send_ac_read(LM_Module_t *mod)
{
    uint8_t data[8] = { 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint32_t ext_id = LM_AC_CMD_BASE | (mod->view.addr & LM_ADDR_MASK);
    if (CHG_LIB_CanBackend_Transmit(ext_id, data, 8)) {
        mod->view.stats.tx_count++;
        mod->view.last_tx_tick = CHG_LIB_NowTick(); /* BUGFIX B-17 */
    }
}

/**
 * @brief  Send read ambient temperature command
 * @note   Response format: Byte4-5 = temp * 10 (0.1 deg C/bit)
 */
static void lm_send_temp_read(LM_Module_t *mod)
{
    uint8_t data[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint32_t ext_id = LM_TEMP_CMD_BASE | (mod->view.addr & LM_ADDR_MASK);
    if (CHG_LIB_CanBackend_Transmit(ext_id, data, 8)) {
        mod->view.stats.tx_count++;
        mod->view.last_tx_tick = CHG_LIB_NowTick(); /* BUGFIX B-17 */
    }
}

/**
 * @brief  Send read status command (keepalive) + diagnostic reads periodically
 * @note   Cycles through: status -> AC read -> temp read -> status ...
 *         AC/temp reads only sent when diag_step indicates it
 */
static void lm_read_status(uint8_t idx, uint32_t now)
{
    LM_Module_t *mod = &g_modules[idx];
    mod->view.last_tx_tick = now;
    mod->diag_counter++;

    if (mod->diag_counter >= LM_DIAG_INTERVAL) {
        mod->diag_counter = 0;
        mod->diag_step = (mod->diag_step + 1) % 3;

        if (mod->diag_step == 1) {
            /* AC input voltage read */
            lm_send_ac_read(mod);
        } else if (mod->diag_step == 2) {
            /* Ambient temperature read */
            lm_send_temp_read(mod);
        } else {
            /* Normal status read */
            lm_send_read(mod);
        }
    } else {
        /* Normal status read */
        lm_send_read(mod);
    }
}

static uint8_t find_by_addr(uint8_t addr)
{
    for (uint8_t i = 0; i < g_module_count; i++) {
        if (g_modules[i].view.enabled && g_modules[i].view.addr == addr) {
            return i;
        }
    }
    return 0xFFU;
}

static void sync_state_from_flags(LM_Module_t *mod, uint32_t now)
{
    bool reported_running = mod->view.running;
    bool fault = mod->view.alarm_flags != CHG_LIB_ALARM_NONE;
    bool recovering = (mod->view.state == CHG_LIB_STATE_RECOVERING);
    CHG_LIB_State_t new_state = lm_state_from_flags(mod, fault, recovering);
    set_state(mod, new_state, now);
    if (!fault && mod->view.online) {
        mod->view.running = reported_running;
    }
    mod->view.last_rx_tick = now;
}

/**
 * @brief  Apply Lianming response data to module view
 * @note   Lianming response format (CMD=1 Read):
 *         Byte 0: CMD echo
 *         Byte 1: Reserved in the official read-status examples
 *         Byte 2-3: Current (0.01A/bit, big-endian)
 *         Byte 4-5: Voltage (0.1V/bit, big-endian)
 *         Byte 6-7: Status flags
 */
static void apply_status(uint8_t idx, const uint8_t *data, uint32_t now)
{
    LM_Module_t *mod = &g_modules[idx];

    /* Parse the official read-status response format from Lianming. */
    mod->view.current = lm_payload_to_current(data);      /* bytes 2-3 */
    mod->view.voltage = lm_payload_to_voltage(data);      /* bytes 4-5 */

    /* Byte 6-7: Status flags */
    uint16_t status_flags = CHG_LIB_ProtocolBEToU16(&data[6]);
    mod->view.alarm_status = status_flags;
    mod->view.alarm_flags = parse_lianming_alarm(status_flags);

    /* Running state: bit 0 of status = 0 means running */
    mod->view.running = ((status_flags & 0x01U) == 0U);

    mod->view.online = true;
    mod->view.last_rx_tick = now;
    mod->view.stats.rx_count++;

    if (mod->view.alarm_flags != CHG_LIB_ALARM_NONE) {
        set_state(mod, CHG_LIB_STATE_FAULT, now);
        return;
    }

    if (mod->view.state == CHG_LIB_STATE_FAULT) {
        /* BUGFIX B-08: this read is clean, but sync_state_from_flags()
         * below would otherwise leave FAULT on the very first clean read
         * -- bypassing process_module()'s 5-read debounce entirely, since
         * that debounce only guards the *polling* path, not this RX
         * callback path. Require the same 5 consecutive clean reads here
         * (same field/threshold as OFFLINE->RECOVERING) before trusting
         * the fault has cleared. */
        mod->view.last_rx_tick = now;
        if ((mod->view.stats.rx_count - mod->recovery_start_rx_count) < 5) {
            return;
        }
        set_state(mod, mod->should_run ? CHG_LIB_STATE_STARTING : CHG_LIB_STATE_IDLE, now);
        return;
    }

    if (mod->view.state == CHG_LIB_STATE_OFFLINE || mod->view.state == CHG_LIB_STATE_RECOVERING) {
 mod->view.stats.recovery_count++;
        set_state(mod, mod->should_run ? CHG_LIB_STATE_STARTING : CHG_LIB_STATE_IDLE, now);
        return;
    }

    sync_state_from_flags(mod, now);
}

static void process_module(uint8_t idx, uint32_t now)
{
    LM_Module_t *mod = &g_modules[idx];
    if (!mod->view.enabled) {
        return;
    }

    /* GATEKEEPER: Check timeout FIRST */
    check_offline_timeout(mod, now);

    switch (mod->view.state) {
    case CHG_LIB_STATE_IDLE:
        /* Periodic poll for keepalive */
        if ((now - mod->last_poll_tick) >= 1000) {  /* 1 second poll */
            lm_read_status(idx, now);
            mod->last_poll_tick = now;
        }
        if (mod->should_run) {
            set_state(mod, CHG_LIB_STATE_STARTING, now);
        }
        break;

    case CHG_LIB_STATE_STARTING:
        if (mod->start_attempts > LM_MAX_RETRY) {
            mod->view.alarm_flags |= CHG_LIB_ALARM_COMM_FAIL; /* Timeout */
            set_state(mod, CHG_LIB_STATE_FAULT, now);
            break;
        }
        if (mod->retry_count == 0U) {
            /* Set voltage and current together (Lianming format) */
            lm_set_output(idx, mod->voltage_setpoint, mod->view.current_limit);
            mod->retry_count = 1U;
            mod->state_enter_tick = now;
        } else if (mod->retry_count == 1U && (now - mod->state_enter_tick) >= LM_STEP_DELAY_MS) {
            /* Start module */
            lm_start_module(idx);
            mod->retry_count = 2U;
            mod->state_enter_tick = now;
        } else if (mod->retry_count >= 3U && (now - mod->state_enter_tick) >= LM_START_CONFIRM_TIMEOUT_MS) {
            mod->start_attempts++;
            mod->retry_count = 0U;
            mod->state_enter_tick = now;
        } else if (mod->retry_count == 2U && (now - mod->state_enter_tick) >= LM_STEP_DELAY_MS) {
            lm_send_read(mod);
            mod->retry_count = 3U;
            mod->state_enter_tick = now;
        }
        break;

    case CHG_LIB_STATE_RUNNING:
        lm_read_status(idx, now);
        if (!mod->should_run) {
            lm_stop_module(idx);
            set_state(mod, CHG_LIB_STATE_STOPPING, now);
        }
        break;

    case CHG_LIB_STATE_STOPPING:
        /* Wait for stop confirmation - periodically retry the stop command.
         * BUGFIX B-07: previously this only polled status and never re-sent
         * lm_stop_module(), so a lost STOP command (dropped CAN frame) left
         * the module running indefinitely with no escalation -- unlike
         * Maxwell/TonHe, which retry then declare COMM_FAIL/FAULT. */
        if ((now - mod->last_poll_tick) >= 500) {  /* 500ms poll/retry */
            if (mod->retry_count < LM_STOP_MAX_RETRY) {
                lm_stop_module(idx);
                mod->retry_count++;
                lm_read_status(idx, now);
                mod->last_poll_tick = now;
            } else {
                mod->view.alarm_flags |= CHG_LIB_ALARM_COMM_FAIL;
                set_state(mod, CHG_LIB_STATE_FAULT, now);
            }
        }
        break;

    case CHG_LIB_STATE_OFFLINE:
        if ((now - mod->state_enter_tick) >= LM_RECOVERY_DELAY_MS) {
            set_state(mod, CHG_LIB_STATE_RECOVERING, now);
        }
        break;

    case CHG_LIB_STATE_RECOVERING:
        if ((now - mod->view.last_rx_tick) <= LM_WARNING_TIMEOUT_MS) {
            if ((mod->view.stats.rx_count - mod->recovery_start_rx_count) >= 5) {
                set_state(mod, mod->should_run ? CHG_LIB_STATE_STARTING : CHG_LIB_STATE_IDLE, now);
                break;
            }
        }
        /* Keep polling until 5 RX — no abort after 3 retries (SRS 6.2) */
        lm_read_status(idx, now);
        break;

    case CHG_LIB_STATE_FAULT:
        lm_read_status(idx, now);
        /* BUGFIX B-08: require 5 CONSECUTIVE clean reads since the alarm
         * bits last cleared, same debounce as OFFLINE->RECOVERING. Reads
         * taken while still faulted also increment rx_count, so the
         * streak start must be re-anchored on every dirty read, or the
         * first clean read after a long fault would satisfy >=5 at once. */
        if (mod->view.alarm_flags == CHG_LIB_ALARM_NONE) {
            if ((mod->view.stats.rx_count - mod->recovery_start_rx_count) >= 5) {
                set_state(mod, mod->should_run ? CHG_LIB_STATE_STARTING : CHG_LIB_STATE_IDLE, now);
            }
        } else {
            mod->recovery_start_rx_count = mod->view.stats.rx_count;
        }
        break;
    default: /* Should not happen */ break;
    }
}

void CHG_Lianming_Init(void)
{
    memset(g_modules, 0, sizeof(g_modules));
    g_module_count = 0;
}

static void lm_init(void)
{
    CHG_Lianming_Init();
}

static int8_t lm_add_module(uint8_t addr, uint8_t group)
{
    if (addr < LM_MODULE_MIN_ADDR || addr > LM_MODULE_MAX_ADDR) {
        return -1;
    }
    if (find_by_addr(addr) != 0xFFU) {
        return -1;
    }
    if (g_module_count >= LM_MAX_MODULES) {
        return -1;
    }

    LM_Module_t *mod = &g_modules[g_module_count];
    memset(mod, 0, sizeof(*mod));
    clear_view(&mod->view);
    mod->view.addr = addr;
    mod->view.group = group;
    mod->should_run = false;
    mod->state_enter_tick = 0;
    mod->last_poll_tick = 0;
    mod->retry_count = 0;
    mod->rated_current_a = 0.0f;
    mod->diag_counter = 0;
    mod->diag_step = 0;
    mod->recovery_start_rx_count = 0;
    g_module_count++;
    return (int8_t)(g_module_count - 1U);
}

static bool lm_set_config(uint8_t idx, float rated_current_a)
{
    if (idx >= g_module_count || !g_modules[idx].view.enabled) return false;
    if (rated_current_a <= 0.0f) return false;
    g_modules[idx].rated_current_a = rated_current_a;
    return true;
}

static void lm_remove_module(uint8_t idx)
{
    if (idx >= g_module_count) {
        return;
    }
    BSP_EnterCritical();
    g_modules[idx].should_run = false;
    for (uint8_t i = idx; i + 1 < g_module_count; i++) {
        g_modules[i] = g_modules[i + 1];
    }
    memset(&g_modules[g_module_count - 1], 0, sizeof(g_modules[0]));
    g_module_count--;
    BSP_ExitCritical();
}

static bool lm_set_voltage(uint8_t idx, float voltage_v)
{
    if (idx >= g_module_count || !g_modules[idx].view.enabled) return false;
    if (!isfinite(voltage_v)) return false; /* BUGFIX B-09: reject NaN/Inf setpoint */
    g_modules[idx].voltage_setpoint = voltage_v;
    /* Lianming: send voltage+current together when running */
    if (g_modules[idx].view.state == CHG_LIB_STATE_RUNNING) {
        lm_set_output(idx, voltage_v, g_modules[idx].view.current_limit);
    }
    return true;
}

static bool lm_set_current_limit(uint8_t idx, float current_a)
{
    if (idx >= g_module_count || !g_modules[idx].view.enabled) return false;
    if (!isfinite(current_a)) return false; /* BUGFIX B-09: reject NaN/Inf setpoint */
    if (current_a < 0.0f) current_a = 0.0f;
    /* Clamp to rated current if configured */
    if (g_modules[idx].rated_current_a > 0.0f && current_a > g_modules[idx].rated_current_a) {
        current_a = g_modules[idx].rated_current_a;
    }
    g_modules[idx].view.current_limit = current_a;
    /* Lianming: send voltage+current together when running */
    if (g_modules[idx].view.state == CHG_LIB_STATE_RUNNING) {
        lm_set_output(idx, g_modules[idx].voltage_setpoint, current_a);
    }
    return true;
}

static bool lm_start(uint8_t idx)
{
    uint32_t now = CHG_LIB_NowTick();
    if (idx >= g_module_count || !g_modules[idx].view.enabled) return false;
    g_modules[idx].should_run = true;
    if (g_modules[idx].view.state == CHG_LIB_STATE_IDLE) {
        set_state(&g_modules[idx], CHG_LIB_STATE_STARTING, now);
    }
    return true;
}

static bool lm_stop(uint8_t idx)
{
    uint32_t now = CHG_LIB_NowTick();
    if (idx >= g_module_count || !g_modules[idx].view.enabled) return false;
    g_modules[idx].should_run = false;
    LM_Module_t *mod = &g_modules[idx];
    if (mod->view.state == CHG_LIB_STATE_WARNING || mod->view.state == CHG_LIB_STATE_OFFLINE ||
        mod->view.state == CHG_LIB_STATE_RECOVERING) {
        /* BUGFIX 2026-08-29 -- see the matching comment in
         * chg_lib_tonhe.c's tonhe_stop(). These are comms-health states,
         * not a real running session: sending a STOP CAN frame to a module
         * already known to be comms-unreachable and waiting through
         * STOPPING's retry-then-FAULT escalation (below) would just be a
         * slower, worse way to end up somewhere that isn't IDLE either --
         * an operator-issued STOP should read as IDLE right away. FAULT
         * intentionally excluded -- keeps its own 5-clean-read debounce
         * (B-08). */
        set_state(mod, CHG_LIB_STATE_IDLE, now);
    } else {
        /* Previously unconditional (sent the STOP frame + went to STOPPING
         * regardless of state) -- now scoped to the states that actually
         * mean something is running to stop: RUNNING/STARTING, or already
         * STOPPING/IDLE/FAULT (where sending it again, or leaving it
         * alone, is harmless and matches the pre-existing behavior for
         * those). */
        lm_stop_module(idx);
        set_state(mod, CHG_LIB_STATE_STOPPING, now);
    }
    return true;
}

static void lm_set_voltage_all(float voltage_v)
{
    for (uint8_t i = 0; i < g_module_count; i++) {
        (void)lm_set_voltage(i, voltage_v);
    }
}

static void lm_set_current_limit_all(float current_a)
{
    for (uint8_t i = 0; i < g_module_count; i++) {
        (void)lm_set_current_limit(i, current_a);
    }
}

static void lm_start_all(void)
{
    for (uint8_t i = 0; i < g_module_count; i++) {
        (void)lm_start(i);
    }
}

static void lm_stop_all(void)
{
    for (uint8_t i = 0; i < g_module_count; i++) {
        (void)lm_stop(i);
    }
}

static void lm_emergency_stop(void)
{
    uint32_t now = CHG_LIB_NowTick();
    for (uint8_t i = 0; i < g_module_count; i++) {
        if (!g_modules[i].view.enabled) continue;
        g_modules[i].should_run = false;
        lm_stop_module(i);
        set_state(&g_modules[i], CHG_LIB_STATE_STOPPING, now);
    }
}

static void lm_process(uint32_t now)
{
    /* BUGFIX B-10: service every enabled module every call instead of one
     * per round-robin index -- see the matching comment in
     * chg_lib_maxwell.c's mx_process() for the full rationale, including
     * why the nested BSP_EnterCritical()/ExitCritical() calls this
     * replaces were also a latent bug (BSP_EnterCritical()/ExitCritical()
     * doesn't nest, so calling it inside CHG_LIB_Process()'s already-held
     * critical section re-enabled interrupts partway through). */
    for (uint8_t idx = 0; idx < g_module_count; idx++) {
        if (g_modules[idx].view.enabled) {
            process_module(idx, now);
        }
    }
}

static void lm_process_rx(uint32_t ext_id, const uint8_t *data, uint8_t dlc, uint32_t now)
{
    if (dlc < 8U) return;

    /* Compare command base while ignoring the lower 7-bit module address. */
    uint32_t id_base = ext_id & ~LM_ADDR_MASK;

    if (id_base == LM_RESP_BASE) {
        /* Standard status response (CMD=1) */
        uint8_t addr = lm_parse_response_addr(ext_id);
        uint8_t idx = find_by_addr(addr);
        if (idx == 0xFFU) return;

        if (data[0] == LM_CMD_READ_INFO) {
            apply_status(idx, data, now);
        } else if (data[0] == LM_CMD_SET_OUTPUT || data[0] == LM_CMD_START_STOP) {
            LM_Module_t *mod = &g_modules[idx];
            mod->view.last_rx_tick = now;
            if (data[1] != 0U) {
                mod->view.stats.rx_count++;
            } else {
                mod->view.stats.error_count++;
            }
        } else {
            /* Unknown command echo: ignore. */
        }
    } else if (id_base == LM_AC_RESP_BASE) {
        /* AC input voltage response (Vab, Vbc, Vca)
         * Response format: CMD=0x31, Byte2-3=Vab/32, Byte4-5=Vbc/32, Byte6-7=Vca/32 */
        uint8_t addr = lm_parse_response_addr(ext_id);
        uint8_t idx = find_by_addr(addr);
        if (idx == 0xFFU) return;

        LM_Module_t *mod = &g_modules[idx];
        if (data[0] == 0x31) {
            uint16_t vab = CHG_LIB_ProtocolBEToU16(&data[2]);
            uint16_t vbc = CHG_LIB_ProtocolBEToU16(&data[4]);
            uint16_t vca = CHG_LIB_ProtocolBEToU16(&data[6]);
            /* Per-phase voltage = raw / 32 (V) */
            mod->view.ac_phase_a_voltage = (float)vab / 32.0f;
            mod->view.ac_phase_b_voltage = (float)vbc / 32.0f;
            mod->view.ac_phase_c_voltage = (float)vca / 32.0f;
            mod->view.last_rx_tick = now;
            mod->view.stats.rx_count++;
        }
    } else if (id_base == LM_TEMP_RESP_BASE) {
        /* Ambient temperature response: Byte4-5 = temp * 10 (0.1 deg C/bit). */
        uint8_t addr = lm_parse_response_addr(ext_id);
        uint8_t idx = find_by_addr(addr);
        if (idx == 0xFFU) return;

        LM_Module_t *mod = &g_modules[idx];
        uint16_t temp_raw = CHG_LIB_ProtocolBEToU16(&data[4]);
        mod->view.temp_ambient = (float)temp_raw / 10.0f;
        mod->view.last_rx_tick = now;
        mod->view.stats.rx_count++;
    }
}

static void lm_feed_frame(uint32_t ext_id, const uint8_t *data, uint8_t dlc)
{
    if (dlc < 8U || data == 0) return;
    uint32_t now = CHG_LIB_NowTick();
    lm_process_rx(ext_id, data, dlc, now);
}

static void lm_get_system_summary(CHG_LIB_SystemSummary_t *summary)
{
    if (summary == 0) return;
    memset(summary, 0, sizeof(*summary));
    for (uint8_t i = 0; i < g_module_count; i++) {
        CHG_LIB_Summary_Accumulate(summary, &g_modules[i].view, 0.0f);
    }
}

static uint8_t lm_get_module_count(void)
{
    return g_module_count;
}

static bool lm_get_module_view(uint8_t idx, CHG_LIB_ModuleView_t *view)
{
    if (view == 0) return false;
    BSP_EnterCritical();
    if (idx >= g_module_count) { BSP_ExitCritical(); return false; }
    CHG_LIB_ModuleView_t tmp = g_modules[idx].view;
    BSP_ExitCritical();
    *view = tmp;
    return true;
}

static const CHG_LIB_DriverOps_t g_lm_ops = {
    .name = "lianming",
    .init = lm_init,
    .deinit = lm_init,
    .add_module = lm_add_module,
    .set_config = lm_set_config,
    .remove_module = lm_remove_module,
    .set_voltage = lm_set_voltage,
    .set_current_limit = lm_set_current_limit,
    .start = lm_start,
    .stop = lm_stop,
    .set_voltage_all = lm_set_voltage_all,
    .set_current_limit_all = lm_set_current_limit_all,
    .start_all = lm_start_all,
    .stop_all = lm_stop_all,
    .emergency_stop = lm_emergency_stop,
    .process = lm_process,
    .feed_frame = lm_feed_frame,
    .get_system_summary = lm_get_system_summary,
    .get_module_count = lm_get_module_count,
    .get_module_view = lm_get_module_view,
};

const CHG_LIB_DriverOps_t *CHG_LIB_LianmingDriverOps(void)
{
    return &g_lm_ops;
}
