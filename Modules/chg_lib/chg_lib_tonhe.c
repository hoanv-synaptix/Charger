/**
 * @file chg_lib_tonhe.c
 * @brief TonHe V1.3 Charging Module Driver - J1939-based CAN protocol
 * @note Protocol: SAE J1939-21, CAN 2.0B Extended Frame, 125Kbps
 *
 * Hardware Interface:
 *   - CAN bus: 125 Kbps, Extended 29-bit frame
 *   - Isolation: Required (isolated CAN transceiver)
 *
 * CAN Frame Format (29-bit Extended ID - J1939):
 *   | Bit 28-26 | Bit 25 | Bit 24 | Bit 23-16 | Bit 15-8 | Bit 7-0 |
 *   | Priority  | R      | DP     | PF         | PS       | SA      |
 *   | 0-7       | 0      | 0      | PDU Format | PDU Specific| Source |
 *
 * PDU Format (PF) defines message type:
 *   - PF < 240: PDU1 format (PS = Destination Address)
 *   - PF >= 240: PDU2 format (PS = Group Extension)
 *
 * Address:
 *   - Controller: 0xA0 (fixed)
 *   - Module: 0x01 - 0xF0 (1-240)
 *   - Broadcast: 0xFF
 *
 * Parameter Group Numbers (PGN):
 *   Uplink (Module -> Controller):
 *   - 0x000100: M_C_1 - Charging module status
 *   - 0x000200: M_C_2 - Start/stop confirmation
 *   - 0x000B00: M_C_3 - AC phase information
 *   - 0x009100: M_C_4 - Extended status/fault
 *
 *   Downlink (Controller -> Module):
 *   - 0x000300: C_M_1 - Broadcast start/stop
 *   - 0x000400: C_M_2 - Broadcast parameter set
 *   - 0x000600: C_M_24 - Specific module start/stop
 *
 * Data Format (M_C_1 - Status):
 *   | Byte 0    | Byte 1-2      | Byte 3-4     | Byte 5-6    | Byte 7   |
 *   | Status    | Voltage(0.1V)| Current(0.01A)| Fault/Warn | PFC Fault |
 *   | 0x00/0x01| 0x0FA0 (400V) | 0x2710 (100A)| Bit field  | Bit field |
 *
 * Control Commands:
 *   - Start: 0xAA
 *   - Stop:  0x55
 *
 * References:
 *   - "TonHeCANcommunicationbetweenchargingmoduleandmonitor TONHE V1.3.pdf"
 *   - "Tonhe.md" (Vietnamese translation)
 *
 * Example:
 *   Module status (400V, 100A, no fault):
 *     ID: 0x1801A001 (Priority=6, PF=0x01, PS=0xA0, SA=0x01)
 *     Data: 01 A0 0F 10 27 00 00 00
 *
 *   Start modules 1,2,3:
 *     ID: 0x0803FFA0 (Priority=2, PF=0x03, PS=0xFF, SA=0xA0)
 *     Data: 07 00 00 AA 00 00 00 00
 */

#include "chg_lib_driver_tonhe.h"
#include "chg_lib_can_backend.h"
#include "priv/chg_lib_core_priv.h"
#include "priv/chg_lib_protocol.h"
#include "main.h"
#include <string.h>

/* ============== Private Types ============== */

typedef struct {
    CHG_LIB_ModuleView_t view;
    uint8_t retry_count;
    uint8_t stop_retry_count;
    uint32_t stop_tick;
    bool should_run;
    bool pending_param;
    uint32_t last_tx_tick;
    uint32_t confirm_tick;
    bool waiting_confirm;
    float target_voltage;
    float target_current;
    CHG_LIB_AlarmFlag_t ext_alarm_flags;  /* Track extended alarm separately for proper clearing */
    uint32_t state_enter_tick;
    uint32_t last_poll_tick;      /* For IDLE polling */
    uint32_t recovery_start_rx_count;
} TONHE_Internal_t;

/* ============== Private State ============== */

static TONHE_Internal_t g_modules[TONHE_MAX_MODULES];
static volatile uint8_t g_module_count = 0;
static volatile uint8_t g_rr_index = 0;
static volatile uint32_t g_last_timing_tick = 0;

/* ============== Forward Declarations ============== */

static void set_state(TONHE_Internal_t *mod, CHG_LIB_State_t st, uint32_t now);
static void parse_ac_phase(const uint8_t *data, uint8_t src_addr, uint32_t now);
static void send_specific_start_stop(TONHE_Internal_t *mod, bool start);
static void send_param_set(TONHE_Internal_t *mod);
static void send_timing_command(void);
static void parse_status(const uint8_t *data, uint8_t src_addr, uint32_t now);
static void parse_confirm(const uint8_t *data, uint8_t src_addr, uint32_t now);
static void parse_extended(const uint8_t *data, uint8_t src_addr, uint32_t now);
static void process_module(TONHE_Internal_t *mod, uint32_t now);

/* ============== CAN ID Builders ============== */

static uint32_t tonhe_build_id(uint8_t pf, uint8_t ps, uint8_t sa, uint8_t priority)
{
    return (((uint32_t)priority & 0x07U) << 26) |
           (((uint32_t)pf & 0xFFU) << 16) |
           (((uint32_t)ps & 0xFFU) << 8) |
           ((uint32_t)sa & 0xFFU);
}

static uint32_t tonhe_specific_cmd_id(uint8_t module_addr)
{
    return tonhe_build_id(0x06, module_addr, TONHE_ADDR_CONTROLLER, TONHE_PRIORITY_CMD);
}

static uint32_t tonhe_param_set_id(void)
{
    return tonhe_build_id(0x04, TONHE_ADDR_BROADCAST, TONHE_ADDR_CONTROLLER, TONHE_PRIORITY_PARAM);
}

static uint32_t tonhe_timing_cmd_id(void)
{
    return tonhe_build_id(0x05, TONHE_ADDR_BROADCAST, TONHE_ADDR_CONTROLLER, TONHE_PRIORITY_TIMING);
}

static bool tonhe_build_process_flags(uint8_t addr, uint8_t group, uint8_t data[4])
{
    if (data == NULL || addr < TONHE_MODULE_MIN_ADDR || addr > TONHE_MODULE_MAX_ADDR) {
        return false;
    }

    uint8_t multiple = (uint8_t)((addr - 1U) / 24U);
    uint8_t bit_pos = (uint8_t)((addr - 1U) % 24U);

    data[0] = 0x00U;
    data[1] = 0x00U;
    data[2] = 0x00U;
    data[3] = (uint8_t)((((group & 0x0FU) << 4) | (multiple & 0x0FU)));
    data[bit_pos / 8U] = (uint8_t)(1U << (bit_pos % 8U));
    return true;
}

/* ============== Helpers ============== */

/* CHG_LIB_NowTick() removed — use CHG_LIB_NowTick() from shared helpers */

/* ============== Alarm Parsing ============== */

static CHG_LIB_AlarmFlag_t tonhe_parse_fault(uint16_t fault_bits, uint8_t pfc_bits)
{
    CHG_LIB_AlarmFlag_t flags = CHG_LIB_ALARM_NONE;

    /* Byte 6-7: Fault/Warning bits */
    if (fault_bits & (1U << 0)) flags |= CHG_LIB_ALARM_AC_UNDER_VOLT;      /* Input undervoltage */
    if (fault_bits & (1U << 1)) flags |= CHG_LIB_ALARM_AC_UNDER_VOLT;      /* Input phase loss */
    if (fault_bits & (1U << 2)) flags |= CHG_LIB_ALARM_OVER_VOLTAGE_OUT;    /* Input overvoltage */
    if (fault_bits & (1U << 3)) flags |= CHG_LIB_ALARM_OVER_VOLTAGE_OUT;    /* Output overvoltage */
    if (fault_bits & (1U << 4)) flags |= CHG_LIB_ALARM_OVER_CURR_OUT;      /* Output overcurrent */
    if (fault_bits & (1U << 5)) flags |= CHG_LIB_ALARM_OVER_TEMP;          /* Temperature high */
    if (fault_bits & (1U << 6)) flags |= CHG_LIB_ALARM_HW_FAULT;           /* Fan fault */
    if (fault_bits & (1U << 7)) flags |= CHG_LIB_ALARM_HW_FAULT;           /* Hardware fault */
    if (fault_bits & (1U << 8)) flags |= CHG_LIB_ALARM_HW_FAULT;           /* Bus exception */
    if (fault_bits & (1U << 9)) flags |= CHG_LIB_ALARM_COMM_FAIL;          /* SCI communication */
    if (fault_bits & (1U << 10)) flags |= CHG_LIB_ALARM_HW_FAULT;         /* Discharge fault */
    if (fault_bits & (1U << 11)) flags |= CHG_LIB_ALARM_HW_FAULT;         /* PFC shutdown */
    if (fault_bits & (1U << 13)) flags |= CHG_LIB_ALARM_OVER_VOLTAGE_OUT; /* Output overvoltage warning */
    if (fault_bits & (1U << 14)) flags |= CHG_LIB_ALARM_OVER_TEMP;        /* Power limit due to high temperature */
    if (fault_bits & (1U << 15)) flags |= CHG_LIB_ALARM_SHORT_CIRCUIT;    /* Short circuit */

    /* PFC fault byte (Byte 8) - full mapping */
    if (pfc_bits & (1U << 0)) flags |= CHG_LIB_ALARM_PFC_OVERCURR;        /* Input overcurrent */
    if (pfc_bits & (1U << 1)) flags |= CHG_LIB_ALARM_FREQ_FAULT;           /* Mains frequency fault */
    if (pfc_bits & (1U << 2)) flags |= CHG_LIB_ALARM_PFC_IMBALANCE;       /* Mains imbalance */
    if (pfc_bits & (1U << 3)) flags |= CHG_LIB_ALARM_HW_FAULT;            /* DCTz fault */
    if (pfc_bits & (1U << 4)) flags |= CHG_LIB_ALARM_HW_FAULT;            /* Address conflict */
    if (pfc_bits & (1U << 5)) flags |= CHG_LIB_ALARM_PFC_IMBALANCE;       /* Bus bias */
    if (pfc_bits & (1U << 6)) flags |= CHG_LIB_ALARM_HW_FAULT;           /* Phase exception */
    if (pfc_bits & (1U << 7)) flags |= CHG_LIB_ALARM_PFC_OVERVOLT;      /* Bus overvoltage */

    return flags;
}

static CHG_LIB_AlarmFlag_t tonhe_parse_extended_fault(uint16_t ext_bits)
{
    CHG_LIB_AlarmFlag_t flags = CHG_LIB_ALARM_NONE;

    if (ext_bits & (1U << 2)) flags |= CHG_LIB_ALARM_COMM_FAIL;           /* CAN timeout */
    if (ext_bits & (1U << 4)) flags |= CHG_LIB_ALARM_HW_FAULT;            /* Relay operation fault */
    if (ext_bits & (1U << 6)) flags |= CHG_LIB_ALARM_OVER_TEMP;           /* Internal overtemperature */
    if (ext_bits & (1U << 7)) flags |= CHG_LIB_ALARM_OVER_TEMP;           /* Air inlet overtemperature */
    if (ext_bits & (1U << 9)) flags |= CHG_LIB_ALARM_OVER_TEMP;           /* Power limit due to overtemperature */
    if (ext_bits & (1U << 10)) flags |= CHG_LIB_ALARM_HW_FAULT;           /* Discharge changeover exception */
    if (ext_bits & (1U << 11)) flags |= CHG_LIB_ALARM_HW_FAULT;           /* Abnormal voltage/current balancing */
    if (ext_bits & (1U << 12)) flags |= CHG_LIB_ALARM_OVER_TEMP;          /* Heat sink temperature differential protection */
    if (ext_bits & (1U << 13)) flags |= CHG_LIB_ALARM_HW_FAULT;          /* Emergency stop */
    if (ext_bits & (1U << 15)) flags |= CHG_LIB_ALARM_PFC_IMBALANCE;      /* Uneven current in front */

    return flags;
}

/* ============== Command Senders ============== */

static void send_specific_start_stop(TONHE_Internal_t *mod, bool start)
{
    uint8_t data[8];
    /* Byte 1: Start/Stop */
    data[0] = start ? TONHE_CMD_START : TONHE_CMD_STOP;
    /* Byte 2: Mode (standby) */
    data[1] = TONHE_MODE_STANDBY;
    /* Byte 3-4: Voltage (0.1V/bit) */
    uint16_t voltage_raw = (uint16_t)(mod->target_voltage / TONHE_VOLTAGE_SCALE);
    data[2] = (uint8_t)(voltage_raw & 0xFF);
    data[3] = (uint8_t)((voltage_raw >> 8) & 0xFF);
    /* Byte 5-6: Current (0.01A/bit) */
    uint16_t current_raw = (uint16_t)(mod->target_current / TONHE_CURRENT_SCALE);
    data[4] = (uint8_t)(current_raw & 0xFF);
    data[5] = (uint8_t)((current_raw >> 8) & 0xFF);
    /* Byte 7-8: Reserved */
    data[6] = 0x00;
    data[7] = 0x00;

    uint32_t ext_id = tonhe_specific_cmd_id(mod->view.addr);
    if (CHG_LIB_CanBackend_Transmit(ext_id, data, 8)) {
        mod->view.stats.tx_count++;
        mod->last_tx_tick = CHG_LIB_NowTick();
        /* Only wait for confirm on START command, not on STOP */
        if (start) {
            mod->waiting_confirm = true;
            mod->confirm_tick = CHG_LIB_NowTick();
        } else {
            mod->waiting_confirm = false;
        }
    }
}

static void send_param_set(TONHE_Internal_t *mod)
{
    uint8_t data[8];
    if (mod == NULL) {
        return;
    }
    if (!tonhe_build_process_flags(mod->view.addr, mod->view.group, data)) {
        return;
    }

    uint16_t voltage_raw = (uint16_t)(mod->target_voltage / TONHE_VOLTAGE_SCALE);
    data[4] = (uint8_t)(voltage_raw & 0xFFU);
    data[5] = (uint8_t)((voltage_raw >> 8) & 0xFFU);

    uint16_t current_raw = (uint16_t)(mod->target_current / TONHE_CURRENT_SCALE);
    data[6] = (uint8_t)(current_raw & 0xFFU);
    data[7] = (uint8_t)((current_raw >> 8) & 0xFFU);

    if (CHG_LIB_CanBackend_Transmit(tonhe_param_set_id(), data, 8U)) {
        mod->view.stats.tx_count++;
        mod->last_tx_tick = CHG_LIB_NowTick();
        mod->pending_param = false;
    }
}

static void send_timing_command(void)
{
    uint8_t data[8] = {0};
    (void)CHG_LIB_CanBackend_Transmit(tonhe_timing_cmd_id(), data, 8U);
}

/* ============== Message Parsers ============== */

static void parse_status(const uint8_t *data, uint8_t src_addr, uint32_t now)
{
    TONHE_Internal_t *mod = NULL;

    /* Find module by address */
    for (uint8_t i = 0; i < g_module_count; i++) {
        if (g_modules[i].view.enabled && g_modules[i].view.addr == src_addr) {
            mod = &g_modules[i];
            break;
        }
    }
    if (mod == NULL) return;

    /* Byte 1: Module status */
    uint8_t status = data[0];

    if (status == TONHE_STATUS_NORMAL_OFF || status == TONHE_STATUS_FAULT_OFF) {
        mod->view.running = false;
    } else {
        /* 0x01 is ON. Other undocumented values (like CC mode) also mean running */
        mod->view.running = true;
    }

    /* PDF uses 1-based byte numbering:
     * Byte 2-3: Output voltage (0.1V/bit, little-endian)
     * Byte 4-5: Output current (0.01A/bit, little-endian)
     * Byte 6-7: Fault/warning bits
     * Byte 8:   PFC fault byte
     */
    uint16_t voltage_raw = ((uint16_t)data[1] | ((uint16_t)data[2] << 8));
    mod->view.voltage = (float)voltage_raw * TONHE_VOLTAGE_SCALE;

    uint16_t current_raw = ((uint16_t)data[3] | ((uint16_t)data[4] << 8));
    mod->view.current = (float)current_raw * TONHE_CURRENT_SCALE;

    uint16_t fault_bits = ((uint16_t)data[5] | ((uint16_t)data[6] << 8));
    mod->view.alarm_status = (mod->view.alarm_status & 0xFFFF0000UL) | (uint32_t)fault_bits;

    uint8_t pfc_bits = data[7];
    mod->view.pfc_fault = pfc_bits;
    mod->view.alarm_flags = tonhe_parse_fault(fault_bits, pfc_bits) | mod->ext_alarm_flags;

    /* Update online status */
    mod->view.online = true;
    mod->view.last_rx_tick = now;
    mod->view.stats.rx_count++;

    /* Update state based on status */
    if (status == TONHE_STATUS_FAULT_OFF) {
        // LOG("TONHE: Module %u FAULT (status=%02X)\r\n", mod->view.addr, status);
        set_state(mod, CHG_LIB_STATE_FAULT, now);
    } else if (status == TONHE_STATUS_NORMAL_OFF) {
        if (mod->view.state == CHG_LIB_STATE_STOPPING) {
            set_state(mod, CHG_LIB_STATE_IDLE, now); /* Confirmed stop */
        } else if (mod->view.state == CHG_LIB_STATE_RUNNING) {
            set_state(mod, CHG_LIB_STATE_IDLE, now); /* Unexpected stop */
        } else if (!mod->should_run) {
            set_state(mod, CHG_LIB_STATE_IDLE, now);
        }
    } else if (status == TONHE_STATUS_ON) {
        if (!mod->should_run) {
            /* Module still ON but we want it stopped — resend STOP */
            if (mod->view.state != CHG_LIB_STATE_STOPPING && mod->view.state != CHG_LIB_STATE_FAULT) {
                LOG("TONHE: Module %u still ON (status=%02X), forcing STOP\r\n", mod->view.addr, status);
                send_specific_start_stop(mod, false);
                mod->stop_retry_count = 0;
                mod->stop_tick = now;
                set_state(mod, CHG_LIB_STATE_STOPPING, now);
            }
        } else {
            set_state(mod, CHG_LIB_STATE_RUNNING, now);
        }
    }
}

static void parse_confirm(const uint8_t *data, uint8_t src_addr, uint32_t now)
{
    TONHE_Internal_t *mod = NULL;

    for (uint8_t i = 0; i < g_module_count; i++) {
        if (g_modules[i].view.enabled && g_modules[i].view.addr == src_addr) {
            mod = &g_modules[i];
            break;
        }
    }
    if (mod == NULL) return;

    /* Byte 1: Command received (0x01 = success) */
    if (data[0] == 0x01) {
        mod->waiting_confirm = false;
        mod->view.stats.rx_count++;  /* Count successful confirm as valid RX */
    } else {
        mod->view.stats.error_count++;
    }

    /* Mark module as online since we received a valid response */
    mod->view.online = true;
    mod->view.last_rx_tick = now;
}

static void parse_ac_phase(const uint8_t *data, uint8_t src_addr, uint32_t now)
{
    TONHE_Internal_t *mod = NULL;

    for (uint8_t i = 0; i < g_module_count; i++) {
        if (g_modules[i].view.enabled && g_modules[i].view.addr == src_addr) {
            mod = &g_modules[i];
            break;
        }
    }
    if (mod == NULL) return;

    /* Byte 1-2: A-phase voltage (0.1V/bit, LE) */
    uint16_t va = ((uint16_t)data[0] | ((uint16_t)data[1] << 8));
    mod->view.ac_phase_a_voltage = (float)va * TONHE_VOLTAGE_SCALE;

    /* Byte 3-4: B-phase voltage (0.1V/bit, LE) */
    uint16_t vb = ((uint16_t)data[2] | ((uint16_t)data[3] << 8));
    mod->view.ac_phase_b_voltage = (float)vb * TONHE_VOLTAGE_SCALE;

    /* Byte 5-6: C-phase voltage (0.1V/bit, LE) */
    uint16_t vc = ((uint16_t)data[4] | ((uint16_t)data[5] << 8));
    mod->view.ac_phase_c_voltage = (float)vc * TONHE_VOLTAGE_SCALE;

    /* Byte 7-8: Ambient temperature */
    uint16_t temp_raw = ((uint16_t)data[6] | ((uint16_t)data[7] << 8));
    mod->view.temp_ambient = (float)temp_raw * TONHE_TEMP_SCALE;
    mod->view.temp_dcdc = mod->view.temp_ambient; /* TonHe doesn't provide DCDC temp, map ambient to it */

    mod->view.last_rx_tick = now;
    mod->view.stats.rx_count++;
}

static void parse_extended(const uint8_t *data, uint8_t src_addr, uint32_t now)
{
    TONHE_Internal_t *mod = NULL;

    for (uint8_t i = 0; i < g_module_count; i++) {
        if (g_modules[i].view.enabled && g_modules[i].view.addr == src_addr) {
            mod = &g_modules[i];
            break;
        }
    }
    if (mod == NULL) return;

    /* Per TonHe V1.3 spec section 9.1.4:
     *   Byte 1-2: Module status (SPN 2816)
     *   Byte 3-4: Fault/warning (SPN 2817), index 2-3 in C array */
    uint16_t ext_fault = ((uint16_t)data[2] | ((uint16_t)data[3] << 8));
    CHG_LIB_AlarmFlag_t new_ext = tonhe_parse_extended_fault(ext_fault);

    mod->ext_alarm_flags = new_ext;
    mod->view.alarm_status = (mod->view.alarm_status & 0x0000FFFFUL) | ((uint32_t)ext_fault << 16);
    mod->view.alarm_flags = tonhe_parse_fault((uint16_t)mod->view.alarm_status,
                                              mod->view.pfc_fault) |
                            mod->ext_alarm_flags;

    mod->view.last_rx_tick = now;
    mod->view.stats.rx_count++;
}

/* ============== State Machine ============== */
static void set_state(TONHE_Internal_t *mod, CHG_LIB_State_t st, uint32_t now)
{
    if (mod->view.state == st) return;

    mod->view.state = st;
    mod->state_enter_tick = now;
    mod->retry_count = 0;
    mod->recovery_start_rx_count = 0;
    
    switch (st) {
        case CHG_LIB_STATE_IDLE:
        case CHG_LIB_STATE_STARTING:
        case CHG_LIB_STATE_STOPPING:
        case CHG_LIB_STATE_RUNNING:
        case CHG_LIB_STATE_FAULT:
        case CHG_LIB_STATE_WARNING:
            mod->view.running = (st == CHG_LIB_STATE_RUNNING || st == CHG_LIB_STATE_WARNING);
            mod->view.online = (mod->view.last_rx_tick != 0 && 
                               (now - mod->view.last_rx_tick) <= TONHE_OFFLINE_TIMEOUT_MS);
            break;
            
        case CHG_LIB_STATE_OFFLINE:
        case CHG_LIB_STATE_RECOVERING:
            mod->view.online = false;
            mod->view.running = false;
            if (st == CHG_LIB_STATE_RECOVERING) {
                mod->recovery_start_rx_count = mod->view.stats.rx_count;
            }
            break;
    }

}

static void check_offline_timeout(TONHE_Internal_t *mod, uint32_t now) {
    if (mod->view.state == CHG_LIB_STATE_OFFLINE ||
        mod->view.state == CHG_LIB_STATE_RECOVERING) {
        return;
    }
    
    uint32_t since_rx = now - mod->view.last_rx_tick;
    
    if (since_rx > TONHE_OFFLINE_TIMEOUT_MS) {
        mod->view.stats.timeout_count++;
        set_state(mod, CHG_LIB_STATE_OFFLINE, now);
    } else if (since_rx > TONHE_WARNING_TIMEOUT_MS) {
        if (mod->view.state == CHG_LIB_STATE_RUNNING || mod->view.state == CHG_LIB_STATE_STARTING) {
            set_state(mod, CHG_LIB_STATE_WARNING, now);
        }
    } else {
        if (mod->view.state == CHG_LIB_STATE_WARNING) {
            set_state(mod, mod->should_run ? CHG_LIB_STATE_RUNNING : CHG_LIB_STATE_IDLE, now);
        }
    }
}

static void process_module(TONHE_Internal_t *mod, uint32_t now)
{
    if (!mod->view.enabled) return;

    /* GATEKEEPER: Check timeout FIRST */
    check_offline_timeout(mod, now);

    switch (mod->view.state) {

    case CHG_LIB_STATE_IDLE:
        /* Periodic poll for keepalive */
        if ((now - mod->last_poll_tick) >= 1000) {  /* 1 second poll */
            send_param_set(mod);
            mod->last_poll_tick = now;
        }
        if (mod->pending_param) {
            send_param_set(mod);
        }
        if (mod->should_run) {
            mod->retry_count = 0;
            if (mod->pending_param) {
                send_param_set(mod);
            }
            send_specific_start_stop(mod, true);
            set_state(mod, CHG_LIB_STATE_STARTING, now);
            // LOG("TONHE: Module %u IDLE->STARTING (sent start cmd)\r\n", mod->view.addr);
        }
        break;

    case CHG_LIB_STATE_STARTING:
        if (mod->pending_param) {
            send_param_set(mod);
        }
        /* Wait for confirm, retry N times only, then give up */
        if (mod->waiting_confirm && (now - mod->confirm_tick) > TONHE_CONFIRM_TIMEOUT_MS) {
            if (mod->retry_count < TONHE_MAX_RETRY) {
                if (mod->pending_param) {
                    send_param_set(mod);
                }
                send_specific_start_stop(mod, true);
                mod->retry_count++;
            } else {
                /* Max retries reached, give up - signal failure to app via FAULT */
                mod->waiting_confirm = false;
                mod->should_run = false;
                mod->view.alarm_flags |= CHG_LIB_ALARM_COMM_FAIL;
                set_state(mod, CHG_LIB_STATE_FAULT, now);
            }
        }
        break;

    case CHG_LIB_STATE_RUNNING:
        if (mod->pending_param) {
            send_param_set(mod);
        }
        /* Handle stop command - only send when app requests */
        if (!mod->should_run) {
            send_specific_start_stop(mod, false);
            mod->stop_retry_count = 0;
            mod->stop_tick = now;
            set_state(mod, CHG_LIB_STATE_STOPPING, now);
        }
        break;

    case CHG_LIB_STATE_STOPPING:
        /* Retry STOP until module confirms OFF */
        if ((now - mod->stop_tick) > TONHE_CONFIRM_TIMEOUT_MS) {
            if (mod->stop_retry_count < TONHE_MAX_RETRY) {
                send_specific_start_stop(mod, false);
                mod->stop_retry_count++;
                mod->stop_tick = now;
                LOG("TONHE: Module %u STOP retry %u\r\n",
                    mod->view.addr, mod->stop_retry_count);
            } else {
                /* Module refuses to stop — critical safety fault */
                LOG("TONHE: Module %u STOP FAILED after %u retries\r\n",
                    mod->view.addr, TONHE_MAX_RETRY);
                mod->view.alarm_flags |= CHG_LIB_ALARM_COMM_FAIL;
                set_state(mod, CHG_LIB_STATE_FAULT, now);
            }
        }
        /* If module reported NORMAL_OFF via parse_status, state already moved to IDLE */
        break;

    case CHG_LIB_STATE_OFFLINE:
        if ((now - mod->state_enter_tick) > TONHE_RECOVERY_DELAY_MS) {
            set_state(mod, CHG_LIB_STATE_RECOVERING, now);
        }
        break;

    case CHG_LIB_STATE_RECOVERING:
        if ((now - mod->view.last_rx_tick) <= TONHE_WARNING_TIMEOUT_MS) {
            if ((mod->view.stats.rx_count - mod->recovery_start_rx_count) >= 5) {
                set_state(mod, mod->should_run ? CHG_LIB_STATE_STARTING : CHG_LIB_STATE_IDLE, now);
                break;
            }
        }
        /* Keep polling — no abort after 3 retries (SRS 6.2) */
        if ((now - mod->last_poll_tick) >= 500) {
            send_param_set(mod);
            mod->last_poll_tick = now;
        }
        break;

    case CHG_LIB_STATE_FAULT:
        /* Try to recover when alarm clears - send start when cleared */
        if (mod->view.alarm_flags == CHG_LIB_ALARM_NONE) {
            if (mod->should_run) {
                if (mod->pending_param) {
                    send_param_set(mod);
                }
                send_specific_start_stop(mod, true);
                set_state(mod, CHG_LIB_STATE_STARTING, now);
            } else {
                set_state(mod, CHG_LIB_STATE_IDLE, now);
            }
        }
        break;
    default: /* Should not happen */ break;
    }
}

/* ============== CHG_LIB_DriverOps_t Implementation ============== */

static void tonhe_init(void)
{
    memset(g_modules, 0, sizeof(g_modules));
    g_module_count = 0;
    g_rr_index = 0;
    g_last_timing_tick = 0;
}

static int8_t tonhe_add_module(uint8_t addr, uint8_t group)
{
    if (g_module_count >= TONHE_MAX_MODULES) return -1;
    if (addr < TONHE_MODULE_MIN_ADDR || addr > TONHE_MODULE_MAX_ADDR) return -1;

    /* Check for duplicate address */
    for (uint8_t i = 0; i < g_module_count; i++) {
        if (g_modules[i].view.enabled && g_modules[i].view.addr == addr) {
            return -1;
        }
    }

    TONHE_Internal_t *mod = &g_modules[g_module_count];
    memset(mod, 0, sizeof(*mod));

    mod->view.addr = addr;
    mod->view.group = group;
    mod->view.enabled = true;
    mod->view.state = CHG_LIB_STATE_IDLE;
    mod->view.current_limit = 0.0f;
    mod->view.voltage = 0.0f;
    mod->target_voltage = 50.0f;
    mod->target_current = 1.0f;
    mod->pending_param = true;
    mod->should_run = false;
    mod->waiting_confirm = false;
    mod->last_poll_tick = 0;

    return (int8_t)(g_module_count++);
}

static bool tonhe_set_config(uint8_t idx, float rated_current_a)
{
    (void)rated_current_a;
    if (idx >= g_module_count || !g_modules[idx].view.enabled) return false;
    return true;
}

static void tonhe_remove_module(uint8_t idx)
{
    if (idx >= g_module_count) return;
    __disable_irq();
    for (uint8_t i = idx; i + 1 < g_module_count; i++) {
        g_modules[i] = g_modules[i + 1];
    }
    memset(&g_modules[g_module_count - 1], 0, sizeof(g_modules[0]));
    g_module_count--;
    if (g_rr_index >= g_module_count && g_module_count > 0) {
        g_rr_index = g_module_count - 1;
    }
    __enable_irq();
}

static bool tonhe_set_voltage(uint8_t idx, float voltage_v)
{
    if (idx >= g_module_count || !g_modules[idx].view.enabled) return false;

    /* Clamp to limits */
    if (voltage_v > TONHE_MAX_OUTPUT_VOLTAGE_V) voltage_v = TONHE_MAX_OUTPUT_VOLTAGE_V;
    if (voltage_v < 0) voltage_v = 0;

    /* Only send if changed */
    if (g_modules[idx].target_voltage != voltage_v) {
        g_modules[idx].target_voltage = voltage_v;
        g_modules[idx].pending_param = true;

        if (g_modules[idx].view.state == CHG_LIB_STATE_RUNNING) {
            send_param_set(&g_modules[idx]);
        }
    }
    return true;
}

static bool tonhe_set_current_limit(uint8_t idx, float current_a)
{
    if (idx >= g_module_count || !g_modules[idx].view.enabled) return false;
    if (current_a < 0.0f) current_a = 0.0f;
    if (current_a > TONHE_MAX_OUTPUT_CURRENT_A) current_a = TONHE_MAX_OUTPUT_CURRENT_A;

    /* Only send if changed */
    if (g_modules[idx].target_current != current_a) {
        g_modules[idx].target_current = current_a;
        g_modules[idx].view.current_limit = current_a;
        g_modules[idx].pending_param = true;

        if (g_modules[idx].view.state == CHG_LIB_STATE_RUNNING) {
            send_param_set(&g_modules[idx]);
        }
    }
    return true;
}

static bool tonhe_start(uint8_t idx)
{
    if (idx >= g_module_count || !g_modules[idx].view.enabled) return false;
    g_modules[idx].should_run = true;
    return true;
}

static bool tonhe_stop(uint8_t idx)
{
    if (idx >= g_module_count || !g_modules[idx].view.enabled) return false;
    g_modules[idx].should_run = false;
    if (g_modules[idx].view.state == CHG_LIB_STATE_RUNNING ||
        g_modules[idx].view.state == CHG_LIB_STATE_STARTING) {
        send_specific_start_stop(&g_modules[idx], false);
        g_modules[idx].stop_retry_count = 0;
        g_modules[idx].stop_tick = CHG_LIB_NowTick();
        set_state(&g_modules[idx], CHG_LIB_STATE_STOPPING, CHG_LIB_NowTick());
    }
    return true;
}

static void tonhe_set_voltage_all(float voltage_v)
{
    for (uint8_t i = 0; i < g_module_count; i++) {
        (void)tonhe_set_voltage(i, voltage_v);
    }
}

static void tonhe_set_current_limit_all(float current_a)
{
    for (uint8_t i = 0; i < g_module_count; i++) {
        (void)tonhe_set_current_limit(i, current_a);
    }
}

static void tonhe_start_all(void)
{
    for (uint8_t i = 0; i < g_module_count; i++) {
        (void)tonhe_start(i);
    }
}

static void tonhe_stop_all(void)
{
    for (uint8_t i = 0; i < g_module_count; i++) {
        (void)tonhe_stop(i);
    }
}

static void tonhe_emergency_stop(void)
{
    uint32_t now = CHG_LIB_NowTick();
    for (uint8_t i = 0; i < g_module_count; i++) {
        if (!g_modules[i].view.enabled) continue;
        g_modules[i].should_run = false;
        send_specific_start_stop(&g_modules[i], false);
        g_modules[i].stop_retry_count = 0;
        g_modules[i].stop_tick = now;
        set_state(&g_modules[i], CHG_LIB_STATE_STOPPING, now);
    }
}

static void tonhe_process(uint32_t now)
{
    if (g_module_count > 0U && (now - g_last_timing_tick) >= 1000U) {
        send_timing_command();
        g_last_timing_tick = now;
    }

    if (g_module_count == 0) return;
    __disable_irq();
    uint8_t idx = g_rr_index;
    __enable_irq();
    if (idx >= g_module_count) idx = 0;
    TONHE_Internal_t *mod = &g_modules[idx];
    bool enabled;
    __disable_irq();
    enabled = mod->view.enabled;
    __enable_irq();
    if (enabled) {
        process_module(mod, now);
    }
    __disable_irq();
    if (g_module_count > 0) g_rr_index = (g_rr_index + 1) % g_module_count;
    __enable_irq();
}

static void tonhe_feed_frame(uint32_t ext_id, const uint8_t *data, uint8_t dlc)
{
    if (dlc < 8 || data == NULL) return;

    /* Extract J1939 fields from 29-bit extended ID:
     *   Bit 28-26: Priority (P)
     *   Bit 24: DP (must be 0)
     *   Bit 23-16: PF (PDU Format)
     *   Bit 15-8: PS (PDU Specific = Destination Address for PF < 240)
     *   Bit 7-0: SA (Source Address = module address) */
    uint8_t priority = (uint8_t)((ext_id >> 26) & 0x07U);
    uint8_t pf = (uint8_t)((ext_id >> 16) & 0xFFU);
    uint8_t ps = (uint8_t)((ext_id >> 8) & 0xFFU);  /* Destination address */
    uint8_t src_addr = (uint8_t)(ext_id & 0xFFU);

    // LOG("TONHE: feed_frame ext_id=%08lX pf=%02X ps=%02X src=%u dlc=%u modules=%u\r\n",
    //     ext_id, pf, ps, src_addr, dlc, g_module_count);

    /* Filter out frames not from a charger module:
     *   - Priority must be valid (2-7 for charger modules)
     *   - PF >= 240: PS is Group Extension (no destination check)
     *   - PF < 240: PS must be controller address (0xA0) or broadcast (0xFF)
     *   - Source address must be valid module range (1-240) */
    if (priority < 2U || priority > 7U) {
        // LOG("TONHE: rejected - bad priority %u\r\n", priority);
        return;
    }
    if (src_addr < TONHE_MODULE_MIN_ADDR || src_addr > TONHE_MODULE_MAX_ADDR) {
        // LOG("TONHE: rejected - bad src_addr %u (range %u-%u)\r\n",
        //     src_addr, TONHE_MODULE_MIN_ADDR, TONHE_MODULE_MAX_ADDR);
        return;
    }
    if (pf < 240U && ps != TONHE_ADDR_CONTROLLER && ps != TONHE_ADDR_BROADCAST) {
        // LOG("TONHE: rejected - bad ps %02X (expect %02X or %02X)\r\n",
        //     ps, TONHE_ADDR_CONTROLLER, TONHE_ADDR_BROADCAST);
        return;
    }

    uint32_t now = CHG_LIB_NowTick();

    switch (pf) {
    case 0x01:  /* M_C_1: Status */
        parse_status(data, src_addr, now);
        break;
    case 0x02:  /* M_C_2: Confirm */
        parse_confirm(data, src_addr, now);
        break;
    case 0x0B:  /* M_C_3: AC Phase */
        parse_ac_phase(data, src_addr, now);
        break;
    case 0x91:  /* M_C_4: Extended */
        parse_extended(data, src_addr, now);
        break;
    default:
        break;
    }
}

static void tonhe_get_system_summary(CHG_LIB_SystemSummary_t *summary)
{
    if (summary == NULL) return;
    memset(summary, 0, sizeof(*summary));

    for (uint8_t i = 0; i < g_module_count; i++) {
        CHG_LIB_ModuleView_t *v = &g_modules[i].view;
        if (!v->enabled) continue;

        /* Only count RUNNING/STARTING as online (active output).
         * IDLE means no active output even if recently received data.
         * OFFLINE/RECOVERING/FAULT are not online. */
        if (v->online && (v->state == CHG_LIB_STATE_RUNNING || v->state == CHG_LIB_STATE_STARTING || v->state == CHG_LIB_STATE_WARNING)) {
            summary->modules_online++;
            summary->total_current += v->current;
            summary->total_power_in += v->voltage * v->current;
            if (summary->voltage == 0.0f && v->voltage > 0.0f) {
                summary->voltage = v->voltage;
            }
        }

        if (v->state == CHG_LIB_STATE_FAULT) {
            summary->modules_fault++;
            summary->any_critical = true;
        }

        if (v->alarm_flags != CHG_LIB_ALARM_NONE) {
            summary->any_critical = true;
        }
    }
}

static uint8_t tonhe_get_module_count(void)
{
    return g_module_count;
}

static bool tonhe_get_module_view(uint8_t idx, CHG_LIB_ModuleView_t *view)
{
    if (view == NULL) return false;
    __disable_irq();
    if (idx >= g_module_count) { __enable_irq(); return false; }
    CHG_LIB_ModuleView_t tmp = g_modules[idx].view;
    __enable_irq();
    *view = tmp;
    return true;
}

/* ============== Driver Table ============== */

static const CHG_LIB_DriverOps_t g_tonhe_ops = {
    .name = "tonhe",
    .init = tonhe_init,
    .deinit = tonhe_init,
    .add_module = tonhe_add_module,
    .set_config = tonhe_set_config,
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

const CHG_LIB_DriverOps_t *CHG_LIB_TonheDriverOps(void)
{
    return &g_tonhe_ops;
}
