/**
 * @file chg_lib_maxwell.c
 * @brief Maxwell MXR Series Charging Module Driver
 * @note Protocol V1.50 - CAN 2.0B Extended Frame, 125Kbps
 *
 * Hardware Interface:
 *   - CAN bus: 125 Kbps, Extended 29-bit frame
 *   - Isolation: Required (isolated CAN transceiver)
 *
 * CAN Frame Format (29-bit Extended ID):
 *   | Bit 28-20 | Bit 19 | Bit 18-11 | Bit 10-3 | Bit 2-0 |
 *   | PROTNO     | PTP     | DSTADDR    | SRCADDR  | Group   |
 *   | 0x060     | 0/1     | 0-63       | 0xF0     | 0-7     |
 *
 * Data Format (8 bytes):
 *   | Byte 0     | Byte 1   | Byte 2-3      | Byte 4-7        |
 *   | Func Code  | Reserved | Register      | Data (IEEE 754) |
 *   | 0x03/0x10 | 0x00     | 0x0021 (Volt) | Float value     |
 *
 * References:
 *   - "CAN Communication Protocol - Maxwell_V1.50.pdf"
 *   - "Maxwell.md" ( Vietnamese translation)
 *
 * Example:
 *   Set voltage 700V: 03 00 00 21 44 2F 00 00 (0x442F0000 = 700.0f)
 *   Start module:    03 00 00 30 00 00 00 00
 *   Stop module:     03 00 00 30 00 01 00 00
 */

#include "chg_lib.h"
#include "chg_lib_can_backend.h"
#include "priv/chg_lib_core_priv.h"
#include "priv/chg_lib_protocol.h"
#include "main.h"
#include <string.h>

/* ============== Maxwell-specific CAN Constants ============== */
/* Protocol theo Maxwell V1.50 - 29-bit Extended Frame */

#define MXR_PROTNO              0x060U      /* Protocol Number */
#define MXR_PTP_POINT           1U          /* Point-to-Point */
#define MXR_PTP_BROADCAST       0U          /* Broadcast */
#define MXR_ADDR_CONTROLLER     0xF0U       /* Controller address */
#define MXR_ADDR_BROADCAST      0xFFU       /* Broadcast address */
#define MXR_FUNC_SET            0x03U       /* Write register */
#define MXR_FUNC_READ           0x10U       /* Read register */
#define MXR_RESP_FLOAT          0x41U       /* Float response */
#define MXR_RESP_INT            0x42U       /* Integer response */
#define MXR_RESP_OK             0xF0U       /* Response OK */
#define MXR_RESP_FAIL           0xF2U       /* Response fail */

/* Command values for REG_ON_OFF */
#define MXR_CMD_STOP            0x00010000U  /* Stop command */
#define MXR_CMD_START          0x00000000U  /* Start command */

/* ============== Configuration ============== */

#define MXR_MAX_MODULES 8
#define MXR_WARNING_TIMEOUT_MS 2000
#define MXR_OFFLINE_TIMEOUT_MS 10000
#define MXR_RECOVERY_DELAY_MS 3000
#define MXR_MAX_RETRIES 3
#define MXR_POLL_REG_COUNT 15
#define MXR_START_MAX_ATTEMPTS      3U
#define MXR_START_VOLTAGE_DELAY_MS  50U
#define MXR_START_CURRENT_DELAY_MS  50U
#define MXR_START_CONFIRM_WAIT_MS  100U
#define MXR_START_CONFIRM_READS      5U

/* Vendor-specific register (không dùng chung, chỉ riêng Maxwell) */
#define MXR_REG_INPUT_VOLTAGE 0x0005
#define MXR_REG_PFC0_VOLTAGE 0x0008
#define MXR_REG_PFC1_VOLTAGE 0x000A
#define MXR_REG_AC_PHASE_A 0x000C
#define MXR_REG_AC_PHASE_B 0x000D
#define MXR_REG_AC_PHASE_C 0x000E
#define MXR_REG_TEMP_PFC 0x0010
#define MXR_REG_SET_ALTITUDE 0x0017
#define MXR_REG_SET_CURRENT_INT 0x001B
#define MXR_REG_SET_GROUP 0x001E
#define MXR_REG_SET_ADDR_MODE 0x001F
#define MXR_REG_OVP_RESET 0x0031
#define MXR_REG_ALTITUDE_RD 0x004A

/* Vendor-specific alarm bits (Theo Maxwell.md) */
#define MXR_ALARM_MODULE_FAULT      (1U << 0)
#define MXR_ALARM_MODULE_PROTECT    (1U << 1)
#define MXR_ALARM_SCI_FAILURE       (1U << 3)
#define MXR_ALARM_INPUT_ERROR       (1U << 4)
#define MXR_ALARM_INPUT_MISMATCH    (1U << 5)
#define MXR_ALARM_DCDC_OV           (1U << 7)
#define MXR_ALARM_PFC_ABNORMAL      (1U << 8)
#define MXR_ALARM_AC_OV             (1U << 9)
#define MXR_ALARM_AC_UNDERVOLTAGE   (1U << 14)
#define MXR_ALARM_CAN_FAILURE       (1U << 16)
#define MXR_ALARM_CURR_IMBALANCE    (1U << 17)
#define MXR_ALARM_DCDC_OFF          (1U << 22)
#define MXR_ALARM_POWER_LIMIT       (1U << 23)
#define MXR_ALARM_TEMP_DERATING     (1U << 24)
#define MXR_ALARM_AC_POWER_LIMIT    (1U << 25)
#define MXR_ALARM_FAN_FAILURE       (1U << 27)
#define MXR_ALARM_SHORT_CIRCUIT     (1U << 28)
#define MXR_ALARM_DCDC_OVERTEMP     (1U << 30)
#define MXR_ALARM_DCDC_OUTPUT_OV    (1U << 31)

#define MXR_ALARM_CRITICAL_MASK (MXR_ALARM_MODULE_FAULT | \
 MXR_ALARM_DCDC_OV | \
 MXR_ALARM_SHORT_CIRCUIT | \
 MXR_ALARM_DCDC_OVERTEMP | \
 MXR_ALARM_DCDC_OUTPUT_OV | \
 MXR_ALARM_SCI_FAILURE)

/* ============== Internal types ============== */

typedef struct {
 float voltage_v;
 float current_limit;
 bool should_run;
} MXR_Setpoint_t;

typedef struct {
 /* Public surface (chính là CHG_LIB_ModuleView_t) */
 CHG_LIB_ModuleView_t view;

 /* Driver-private state */
 MXR_Setpoint_t setpoint;
 uint32_t state_enter_tick;
 uint32_t last_poll_tick;    /* For IDLE polling */
 uint8_t retry_count;
 uint8_t start_attempts;
 uint8_t poll_step;
 float rated_current_a;
 bool should_run;
 uint32_t recovery_start_rx_count;
} MXR_Internal_t;

/* ============== Private state ============== */

static MXR_Internal_t g_modules[MXR_MAX_MODULES];
static volatile uint8_t g_module_count = 0;
static volatile uint8_t g_rr_index = 0;

static const uint16_t g_poll_regs[MXR_POLL_REG_COUNT] = {
 CHG_LIB_REG_VOLTAGE,
 CHG_LIB_REG_CURRENT,
 CHG_LIB_REG_CURR_LIMIT,
 CHG_LIB_REG_TEMP_DCDC,
 CHG_LIB_REG_TEMP_AMBIENT,
 CHG_LIB_REG_ALARM_STATUS,
 CHG_LIB_REG_INPUT_POWER,
 /* Extended registers (diagnostics) */
 CHG_LIB_REG_PFC0_VOLTAGE,
 CHG_LIB_REG_PFC1_VOLTAGE,
 CHG_LIB_REG_TEMP_PFC,
 CHG_LIB_REG_RATED_POWER,
 CHG_LIB_REG_RATED_CURRENT,
 /* AC Input phases */
 CHG_LIB_REG_AC_PHASE_A,
 CHG_LIB_REG_AC_PHASE_B,
 CHG_LIB_REG_AC_PHASE_C,
};

/* ============== CAN Frame ID Builder ============== */

/**
 * @brief  Build 29-bit CAN Extended ID theo Maxwell protocol
 * @note   Format: PROTNO(9b) | PTP(1b) | DSTADDR(8b) | SRCADDR(8b) | GRP(3b)
 * @param  dst_addr  Địa chỉ đích (0-63)
 * @param  src_addr  Địa chỉ nguồn (0xF0 cho controller)
 * @param  ptp       1=Point-to-Point, 0=Broadcast
 * @param  group     Group number (0-7)
 * @retval 29-bit CAN ID
 */
static uint32_t mxr_build_frame_id(uint8_t dst_addr, uint8_t src_addr,
                                   uint8_t ptp, uint8_t group)
{
    uint32_t id = 0;
    id |= ((uint32_t)MXR_PROTNO & 0x1FFU) << 20;    /* bits 20-28: PROTNO */
    id |= ((uint32_t)ptp & 0x01U) << 19;            /* bit 19: PTP */
    id |= ((uint32_t)dst_addr & 0xFFU) << 11;        /* bits 11-18: DSTADDR */
    id |= ((uint32_t)src_addr & 0xFFU) << 3;        /* bits 3-10: SRCADDR */
    id |= ((uint32_t)group & 0x07U);                /* bits 0-2: GRP */
    return id;
}

/* ============== Helpers ============== */
/* now_tick() removed — use CHG_LIB_NowTick() from shared helpers */

static bool send_frame(MXR_Internal_t *m, uint8_t func, uint16_t reg, const uint8_t *payload4)
{
    uint8_t frame_data[8];
    frame_data[0] = func;
    frame_data[1] = 0x00;
    frame_data[2] = (uint8_t)(reg >> 8);
    frame_data[3] = (uint8_t)(reg & 0xFF);
    frame_data[4] = payload4[0];
    frame_data[5] = payload4[1];
    frame_data[6] = payload4[2];
    frame_data[7] = payload4[3];
    uint32_t ext_id = mxr_build_frame_id(m->view.addr, MXR_ADDR_CONTROLLER,
                                          MXR_PTP_POINT, m->view.group);
    if (CHG_LIB_CanBackend_Transmit(ext_id, frame_data, 8)) {
        m->view.stats.tx_count++;
        m->view.last_tx_tick = CHG_LIB_NowTick();
        return true;
    }
    return false;
}

static bool send_set_float(MXR_Internal_t *m, uint16_t reg, float val)
{
 uint8_t payload[4];
 CHG_LIB_ProtocolFloatToBE(val, payload);
 return send_frame(m, MXR_FUNC_SET, reg, payload);
}

static bool send_set_u32(MXR_Internal_t *m, uint16_t reg, uint32_t val)
{
 uint8_t payload[4];
 CHG_LIB_ProtocolU32ToBE(val, payload);
 return send_frame(m, MXR_FUNC_SET, reg, payload);
}

static bool send_read(MXR_Internal_t *m, uint16_t reg)
{
 uint8_t payload[4] = {0, 0, 0, 0};
 return send_frame(m, MXR_FUNC_READ, reg, payload);
}

static float rated_current_or_fallback(const MXR_Internal_t *m)
{
 float rated = m->rated_current_a;
 if (rated <= 0.0f) rated = m->view.rated_current;
 if (rated <= 0.0f) rated = 20.0f;  /* fallback if PC has not configured rated current */
 return rated;
}

static float current_limit_to_ratio(const MXR_Internal_t *m)
{
 float ratio = m->setpoint.current_limit / rated_current_or_fallback(m);
 if (ratio < 0.0f) ratio = 0.0f;
 if (ratio > 1.0f) ratio = 1.0f;
 return ratio;
}

static bool send_set_current_limit(MXR_Internal_t *m)
{
 return send_set_float(m, CHG_LIB_REG_SET_CURR_LIMIT, current_limit_to_ratio(m));
}

static void set_state(MXR_Internal_t *m, CHG_LIB_State_t new_state, uint32_t now)
{
    if (m->view.state == new_state) return;

    m->view.state = new_state;
    m->state_enter_tick = now;
    m->retry_count = 0;

    if (new_state == CHG_LIB_STATE_STARTING) {
        m->start_attempts++;
    } else if (new_state == CHG_LIB_STATE_IDLE || new_state == CHG_LIB_STATE_OFFLINE || new_state == CHG_LIB_STATE_FAULT) {
        m->start_attempts = 0;
    }

    /* Unified online/running flag management */
    switch (new_state) {
        case CHG_LIB_STATE_IDLE:
        case CHG_LIB_STATE_STARTING:
        case CHG_LIB_STATE_STOPPING:
        case CHG_LIB_STATE_FAULT:
        case CHG_LIB_STATE_WARNING:
            m->view.running = (new_state == CHG_LIB_STATE_WARNING); /* WARNING counts as running internally to maintain active count */
            m->view.online = (m->view.last_rx_tick != 0 &&
                              (now - m->view.last_rx_tick) <= MXR_OFFLINE_TIMEOUT_MS);
            break;

        case CHG_LIB_STATE_RUNNING:
            m->view.running = true;
            m->view.online = true;
            break;

        case CHG_LIB_STATE_OFFLINE:
        case CHG_LIB_STATE_RECOVERING:
            m->view.running = false;
            m->view.online = false;
            if (new_state == CHG_LIB_STATE_RECOVERING) {
                m->recovery_start_rx_count = m->view.stats.rx_count;
            }
            break;
    }
}

/**
 * @brief Check for communication timeout and transition to OFFLINE if needed
 * @note Called FIRST in process_module() for all states except OFFLINE/RECOVERING
 */
static void check_offline_timeout(MXR_Internal_t *mod, uint32_t now) {
    if (mod->view.state == CHG_LIB_STATE_OFFLINE ||
        mod->view.state == CHG_LIB_STATE_RECOVERING) {
        return;
    }
    
    uint32_t since_rx = now - mod->view.last_rx_tick;
    
    if (since_rx > MXR_OFFLINE_TIMEOUT_MS) {
        mod->view.stats.timeout_count++;
        set_state(mod, CHG_LIB_STATE_OFFLINE, now);
    } else if (since_rx > MXR_WARNING_TIMEOUT_MS) {
        if (mod->view.state == CHG_LIB_STATE_RUNNING || mod->view.state == CHG_LIB_STATE_STARTING) {
            set_state(mod, CHG_LIB_STATE_WARNING, now);
        }
    } else {
        if (mod->view.state == CHG_LIB_STATE_WARNING) {
            set_state(mod, mod->setpoint.should_run ? CHG_LIB_STATE_RUNNING : CHG_LIB_STATE_IDLE, now);
        }
    }
}

static MXR_Internal_t *find_by_addr(uint8_t addr)
{
 for (uint8_t i = 0; i < g_module_count; i++) {
 if (g_modules[i].view.enabled && g_modules[i].view.addr == addr) {
 return &g_modules[i];
 }
 }
 return NULL;
}

static CHG_LIB_AlarmFlag_t parse_maxwell_alarm(uint32_t raw)
{
 CHG_LIB_AlarmFlag_t flags = CHG_LIB_ALARM_NONE;
 if (raw & MXR_ALARM_MODULE_FAULT) flags |= CHG_LIB_ALARM_HW_FAULT;
 if (raw & MXR_ALARM_MODULE_PROTECT) flags |= CHG_LIB_ALARM_HW_FAULT;
 if (raw & MXR_ALARM_SCI_FAILURE) flags |= CHG_LIB_ALARM_COMM_FAIL;
 if (raw & MXR_ALARM_INPUT_ERROR) flags |= CHG_LIB_ALARM_HW_FAULT;
 if (raw & MXR_ALARM_INPUT_MISMATCH) flags |= CHG_LIB_ALARM_HW_FAULT;
 if (raw & MXR_ALARM_DCDC_OV) flags |= CHG_LIB_ALARM_OVER_VOLTAGE_OUT;
 if (raw & MXR_ALARM_PFC_ABNORMAL) flags |= CHG_LIB_ALARM_HW_FAULT;
 if (raw & MXR_ALARM_AC_OV) flags |= CHG_LIB_ALARM_OVER_VOLTAGE_OUT;
 if (raw & MXR_ALARM_AC_UNDERVOLTAGE) flags |= CHG_LIB_ALARM_AC_UNDER_VOLT;
 if (raw & MXR_ALARM_CAN_FAILURE) flags |= CHG_LIB_ALARM_COMM_FAIL;
 if (raw & MXR_ALARM_CURR_IMBALANCE) flags |= CHG_LIB_ALARM_OVER_CURR_OUT;
 if (raw & MXR_ALARM_DCDC_OFF) flags |= CHG_LIB_ALARM_HW_FAULT;
 if (raw & MXR_ALARM_POWER_LIMIT) flags |= CHG_LIB_ALARM_HW_FAULT;
 if (raw & MXR_ALARM_TEMP_DERATING) flags |= CHG_LIB_ALARM_OVER_TEMP;
 if (raw & MXR_ALARM_AC_POWER_LIMIT) flags |= CHG_LIB_ALARM_HW_FAULT;
 if (raw & MXR_ALARM_FAN_FAILURE) flags |= CHG_LIB_ALARM_HW_FAULT;
 if (raw & MXR_ALARM_SHORT_CIRCUIT) flags |= CHG_LIB_ALARM_SHORT_CIRCUIT;
 if (raw & MXR_ALARM_DCDC_OVERTEMP) flags |= CHG_LIB_ALARM_OVER_TEMP;
 if (raw & MXR_ALARM_DCDC_OUTPUT_OV) flags |= CHG_LIB_ALARM_OVER_VOLTAGE_OUT;
 return flags;
}

/* ============== Response handler ============== */

static void apply_response(MXR_Internal_t *m, const uint8_t *data, uint32_t now)
{
 uint8_t data_type = data[0];
 uint8_t error_code = data[1];
 uint16_t reg = ((uint16_t)data[2] << 8) | data[3];

 if (data_type != MXR_RESP_FLOAT && data_type != MXR_RESP_INT) {
 m->view.stats.error_count++;
 return;
 }

 if (error_code != MXR_RESP_OK) {
 m->view.stats.error_count++;
 return;
 }

 switch (reg) {
 case CHG_LIB_REG_VOLTAGE: {
     m->view.voltage = CHG_LIB_ProtocolBEToFloat(&data[4]);
     break;
 }
 case CHG_LIB_REG_CURRENT: m->view.current = CHG_LIB_ProtocolBEToFloat(&data[4]); break;
 case CHG_LIB_REG_CURR_LIMIT: m->view.current_limit = CHG_LIB_ProtocolBEToFloat(&data[4]) * rated_current_or_fallback(m); break;
 case CHG_LIB_REG_TEMP_DCDC: m->view.temp_dcdc = CHG_LIB_ProtocolBEToFloat(&data[4]); break;
 case CHG_LIB_REG_TEMP_AMBIENT: m->view.temp_ambient = CHG_LIB_ProtocolBEToFloat(&data[4]); break;
 case CHG_LIB_REG_ALARM_STATUS: {
     m->view.alarm_status = CHG_LIB_ProtocolBEToU32(&data[4]);
     /* Preserve COMM_FAIL from software timeout */
     m->view.alarm_flags = parse_maxwell_alarm(m->view.alarm_status) | (m->view.alarm_flags & CHG_LIB_ALARM_COMM_FAIL);
     break;
 }
 case CHG_LIB_REG_INPUT_POWER: {
     uint32_t raw = CHG_LIB_ProtocolBEToU32(&data[4]);
     if (raw <= CHG_LIB_INPUT_POWER_MAX_W) {
         m->view.input_power = raw;
     }
     break;
 }
 case CHG_LIB_REG_PFC0_VOLTAGE: m->view.pfc_bus_pos_voltage = CHG_LIB_ProtocolBEToFloat(&data[4]); break;
 case CHG_LIB_REG_PFC1_VOLTAGE: m->view.pfc_bus_neg_voltage = CHG_LIB_ProtocolBEToFloat(&data[4]); break;
 case CHG_LIB_REG_TEMP_PFC: m->view.temp_pfc = CHG_LIB_ProtocolBEToFloat(&data[4]); break;
 case CHG_LIB_REG_RATED_POWER: m->view.rated_power = CHG_LIB_ProtocolBEToFloat(&data[4]); break;
 case CHG_LIB_REG_RATED_CURRENT: m->view.rated_current = CHG_LIB_ProtocolBEToFloat(&data[4]); break;
 case CHG_LIB_REG_AC_PHASE_A: m->view.ac_phase_a_voltage = CHG_LIB_ProtocolBEToFloat(&data[4]); break;
 case CHG_LIB_REG_AC_PHASE_B: m->view.ac_phase_b_voltage = CHG_LIB_ProtocolBEToFloat(&data[4]); break;
 case CHG_LIB_REG_AC_PHASE_C: m->view.ac_phase_c_voltage = CHG_LIB_ProtocolBEToFloat(&data[4]); break;
 case CHG_LIB_REG_SET_POWER:
 case CHG_LIB_REG_SET_VOLTAGE:
 case CHG_LIB_REG_SET_CURR_LIMIT:
 case CHG_LIB_REG_SET_OVP:
 case CHG_LIB_REG_ON_OFF:
     if (m->view.state == CHG_LIB_STATE_STOPPING) {
         set_state(m, CHG_LIB_STATE_IDLE, now);
     }
     break; /* Valid write ACK */
 case CHG_LIB_REG_SHORT_RESET:
 case CHG_LIB_REG_INPUT_MODE_SET:
     break; /* Valid write ACK */
 default: return; /* Unknown reg: count as error */
 }

 m->view.online = true;
 m->view.last_rx_tick = now;
 m->view.stats.rx_count++;

 /* Startup confirmation: After sending ON command and receiving a valid response,
  * transition to RUNNING. We require:
  *   1. In STARTING state
  *   2. Already sent ON command (retry_count >= 3)
  *   3. Have output voltage > 0 (module is actually producing power)
  * If the module responds but has no output voltage, stay in STARTING until
  * either voltage appears or timeout causes retry_count to exceed MXR_START_CONFIRM_READS. */
 if (m->view.state == CHG_LIB_STATE_STARTING && m->retry_count >= 3U) {
     if (m->view.voltage > 0.0f) {
         set_state(m, CHG_LIB_STATE_RUNNING, now);
     }
     /* If voltage is still 0, keep STARTING state - will retry until timeout */
 }


 /* Critical alarm -> stop ngay */
 if (m->view.alarm_flags != CHG_LIB_ALARM_NONE && m->view.state == CHG_LIB_STATE_RUNNING) {
 set_state(m, CHG_LIB_STATE_FAULT, now);
 send_set_u32(m, CHG_LIB_REG_ON_OFF, MXR_CMD_STOP);
 }
}

/* ============== FSM per module ============== */

static void process_module(MXR_Internal_t *m, uint32_t now)
{
    if (!m->view.enabled) return;

    /* GATEKEEPER: Check timeout FIRST */
    check_offline_timeout(m, now);

    uint32_t since_state = now - m->state_enter_tick;

    switch (m->view.state) {

    case CHG_LIB_STATE_IDLE:
        /* Periodic poll for keepalive */
        if ((now - m->last_poll_tick) >= 1000) {  /* 1 second poll */
            send_read(m, g_poll_regs[m->poll_step]);
            m->poll_step = (m->poll_step + 1) % MXR_POLL_REG_COUNT;
            m->last_poll_tick = now;
        }
        if (m->setpoint.should_run) {
            set_state(m, CHG_LIB_STATE_STARTING, now);
        }
        break;

 case CHG_LIB_STATE_STARTING:
        if (m->start_attempts > MXR_START_MAX_ATTEMPTS) {
            m->view.alarm_flags |= CHG_LIB_ALARM_COMM_FAIL; /* Timeout */
            set_state(m, CHG_LIB_STATE_FAULT, now);
            break;
        }
        if (m->retry_count == 0) {
            send_set_float(m, CHG_LIB_REG_SET_VOLTAGE, m->setpoint.voltage_v);
            m->retry_count++;
            m->state_enter_tick = now;
        } else if (m->retry_count == 1 && (now - m->state_enter_tick) >= MXR_START_VOLTAGE_DELAY_MS) {
            send_set_current_limit(m);
            m->retry_count++;
            m->state_enter_tick = now;
        } else if (m->retry_count == 2 && (now - m->state_enter_tick) >= MXR_START_CURRENT_DELAY_MS) {
            send_set_u32(m, CHG_LIB_REG_ON_OFF, MXR_CMD_START);
            m->retry_count = 3;
            m->view.last_tx_tick = now;
        } else if (m->retry_count >= 3 && (now - m->view.last_tx_tick) >= MXR_START_CONFIRM_WAIT_MS) {
            if ((m->retry_count - 3U) >= MXR_START_CONFIRM_READS) {
                m->view.stats.timeout_count++;
                m->view.alarm_flags |= CHG_LIB_ALARM_COMM_FAIL;
                set_state(m, CHG_LIB_STATE_FAULT, now);
            } else {
                send_read(m, CHG_LIB_REG_ALARM_STATUS);
                m->retry_count++;
            }
        }
 break;

 case CHG_LIB_STATE_RUNNING:
 send_read(m, g_poll_regs[m->poll_step]);
 m->poll_step = (m->poll_step + 1) % MXR_POLL_REG_COUNT;

 if (!m->setpoint.should_run) {
 send_set_u32(m, CHG_LIB_REG_ON_OFF, MXR_CMD_STOP);
 set_state(m, CHG_LIB_STATE_STOPPING, now);
 }
 break;

 case CHG_LIB_STATE_STOPPING:
 /* Wait for stop confirmation - periodically retry */
 if ((now - m->state_enter_tick) >= 500) {  /* 500ms timeout */
     if (m->retry_count < 5) {
         send_set_u32(m, CHG_LIB_REG_ON_OFF, MXR_CMD_STOP);
         m->retry_count++;
         m->state_enter_tick = now;
     } else {
         m->view.alarm_flags |= CHG_LIB_ALARM_COMM_FAIL;
         set_state(m, CHG_LIB_STATE_FAULT, now);
     }
 }
 break;

 case CHG_LIB_STATE_OFFLINE:
 if (since_state > MXR_RECOVERY_DELAY_MS) {
 set_state(m, CHG_LIB_STATE_RECOVERING, now);
 }
 break;

 case CHG_LIB_STATE_RECOVERING:
 if ((now - m->view.last_rx_tick) <= MXR_WARNING_TIMEOUT_MS) {
     if ((m->view.stats.rx_count - m->recovery_start_rx_count) >= 5) {
         set_state(m, m->setpoint.should_run ? CHG_LIB_STATE_STARTING : CHG_LIB_STATE_IDLE, now);
         break;
     }
 }
 /* Keep polling until 5 RX received — do NOT abort after 3 retries (SRS 6.2) */
 send_read(m, CHG_LIB_REG_ALARM_STATUS);
 break;

 case CHG_LIB_STATE_FAULT:
 send_read(m, CHG_LIB_REG_ALARM_STATUS);
 if (m->view.alarm_flags == CHG_LIB_ALARM_NONE) {
 set_state(m, m->setpoint.should_run ? CHG_LIB_STATE_STARTING : CHG_LIB_STATE_IDLE, now);
 }
 break;
 default: /* Should not happen */ break;
 }
}

/* ============== CHG_LIB_DriverOps_t implementation ============== */

static void mx_init(void)
{
 memset(g_modules, 0, sizeof(g_modules));
 g_module_count = 0;
 g_rr_index = 0;
}

static int8_t mx_add_module(uint8_t addr, uint8_t group)
{
 if (g_module_count >= MXR_MAX_MODULES) return -1;
 if (find_by_addr(addr) != NULL) return -1;

 MXR_Internal_t *m = &g_modules[g_module_count];
 memset(m, 0, sizeof(*m));
 m->view.addr = addr;
 m->view.group = group;
 m->view.enabled = true;
 m->view.state = CHG_LIB_STATE_IDLE;
 m->view.current_limit = 1.0f;
 m->setpoint.voltage_v = 0.0f;
 m->setpoint.current_limit = 1.0f;
 m->setpoint.should_run = false;
 m->setpoint.voltage_v = 0.0f;
 m->recovery_start_rx_count = 0;
 m->last_poll_tick = 0;
 m->rated_current_a = 0.0f;

 return (int8_t)(g_module_count++);
}

static bool mx_set_config(uint8_t idx, float rated_current_a)
{
    if (idx >= g_module_count || !g_modules[idx].view.enabled) return false;
    if (rated_current_a <= 0.0f) return false;
    g_modules[idx].rated_current_a = rated_current_a;
    return true;
}

 static void mx_remove_module(uint8_t idx)
{
 if (idx >= g_module_count) return;
 __disable_irq();
 MXR_Internal_t *m = &g_modules[idx];
 if (m->view.state == CHG_LIB_STATE_RUNNING || m->view.state == CHG_LIB_STATE_STARTING) {
  __enable_irq();
  send_set_u32(m, CHG_LIB_REG_ON_OFF, MXR_CMD_STOP);
  __disable_irq();
 }
 /* Compact array: shift tail left */
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

static bool mx_set_voltage(uint8_t idx, float voltage_v)
{
 if (idx >= g_module_count || !g_modules[idx].view.enabled) return false;
 g_modules[idx].setpoint.voltage_v = voltage_v;
 /* Keep view.voltage as measured telemetry only; do not replace it with the
  * requested setpoint. The controller uses this field for No-BMS completion. */
 if (g_modules[idx].view.state == CHG_LIB_STATE_RUNNING) {
 return send_set_float(&g_modules[idx], CHG_LIB_REG_SET_VOLTAGE, voltage_v);
 }
 return true;
}

static bool mx_set_current_limit(uint8_t idx, float current_a)
{
 if (idx >= g_module_count || !g_modules[idx].view.enabled) return false;
 if (current_a < 0.0f) current_a = 0.0f;

 g_modules[idx].setpoint.current_limit = current_a;
 g_modules[idx].view.current_limit = current_a;
 if (g_modules[idx].view.state == CHG_LIB_STATE_RUNNING) {
     return send_set_current_limit(&g_modules[idx]);
 }
 return true;
}

static bool mx_start(uint8_t idx)
{
    uint32_t now = CHG_LIB_NowTick();
    if (idx >= g_module_count || !g_modules[idx].view.enabled) return false;
    g_modules[idx].setpoint.should_run = true;
    if (g_modules[idx].view.state == CHG_LIB_STATE_IDLE) {
        set_state(&g_modules[idx], CHG_LIB_STATE_STARTING, now);
    }
    return true;
}

static bool mx_stop(uint8_t idx)
{
    uint32_t now = CHG_LIB_NowTick();
    if (idx >= g_module_count || !g_modules[idx].view.enabled) return false;
    g_modules[idx].setpoint.should_run = false;
    MXR_Internal_t *m = &g_modules[idx];
    if (m->view.state == CHG_LIB_STATE_RUNNING || m->view.state == CHG_LIB_STATE_STARTING) {
        send_set_u32(m, CHG_LIB_REG_ON_OFF, MXR_CMD_STOP);
        set_state(m, CHG_LIB_STATE_STOPPING, now);
    }
    return true;
}

static void mx_set_voltage_all(float voltage_v)
{
 for (uint8_t i = 0; i < g_module_count; i++) (void)mx_set_voltage(i, voltage_v);
}

static void mx_set_current_limit_all(float current_a)
{
 for (uint8_t i = 0; i < g_module_count; i++) (void)mx_set_current_limit(i, current_a);
}

static void mx_start_all(void)
{
 for (uint8_t i = 0; i < g_module_count; i++) (void)mx_start(i);
}

static void mx_stop_all(void)
{
 for (uint8_t i = 0; i < g_module_count; i++) (void)mx_stop(i);
}

static void mx_emergency_stop(void)
{
    uint32_t now = CHG_LIB_NowTick();
    for (uint8_t i = 0; i < g_module_count; i++) {
        MXR_Internal_t *m = &g_modules[i];
        if (!m->view.enabled) continue;
        m->setpoint.should_run = false;
        send_set_u32(m, CHG_LIB_REG_ON_OFF, MXR_CMD_STOP);
        set_state(m, CHG_LIB_STATE_STOPPING, now);
    }
}

static void mx_process(uint32_t now)
{
 if (g_module_count == 0) return;
 /* Round-robin: protect index and view access */
 __disable_irq();
 uint8_t idx = g_rr_index;
 __enable_irq();
 if (idx >= g_module_count) idx = 0;
 MXR_Internal_t *m = &g_modules[idx];
 /* Copy enabled flag atomically */
 bool enabled;
 __disable_irq();
 enabled = m->view.enabled;
 __enable_irq();
 if (enabled) {
 process_module(m, now);
 }
 __disable_irq();
 if (g_module_count > 0) g_rr_index = (g_rr_index + 1) % g_module_count;
 __enable_irq();
}

static void mx_feed_frame(uint32_t ext_id, const uint8_t *data, uint8_t dlc)
{
 if (dlc < 8 || data == 0) return;

 uint16_t protno = (uint16_t)((ext_id >> 20) & 0x1FFU);
 uint8_t ptp = (uint8_t)((ext_id >> 19) & 0x01U);
 uint8_t dst_addr = (uint8_t)((ext_id >> 11) & 0xFFU);
 uint8_t group = (uint8_t)(ext_id & 0x07U);
 if (protno != MXR_PROTNO || ptp != MXR_PTP_POINT || dst_addr != MXR_ADDR_CONTROLLER) {
 return;
 }

 uint8_t src_addr = (uint8_t)((ext_id >> 3) & 0xFF);
 MXR_Internal_t *m = find_by_addr(src_addr);
 if (m == NULL) return;
 if (group != m->view.group) return;

 apply_response(m, data, CHG_LIB_NowTick());
}

static void mx_get_system_summary(CHG_LIB_SystemSummary_t *summary)
{
 if (summary == 0) return;
 memset(summary, 0, sizeof(*summary));

 for (uint8_t i = 0; i < g_module_count; i++) {
 CHG_LIB_ModuleView_t *v = &g_modules[i].view;
 if (!v->enabled) continue;
 if (v->online && (v->state == CHG_LIB_STATE_RUNNING || v->state == CHG_LIB_STATE_STARTING || v->state == CHG_LIB_STATE_IDLE || v->state == CHG_LIB_STATE_WARNING)) {
     summary->modules_online++;
     summary->total_current += v->current;
     summary->total_power_in += (float)v->input_power;
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

static uint8_t mx_get_module_count(void)
{
 return g_module_count;
}

static bool mx_get_module_view(uint8_t idx, CHG_LIB_ModuleView_t *view)
{
 if (view == 0) return false;
 __disable_irq();
 if (idx >= g_module_count) { __enable_irq(); return false; }
 CHG_LIB_ModuleView_t tmp = g_modules[idx].view;
 __enable_irq();
 *view = tmp;
 return true;
}

/* ============== Public API ============== */

static const CHG_LIB_DriverOps_t g_maxwell_ops = {
 .name = "maxwell",
 .init = mx_init,
 .deinit = mx_init,
 .add_module = mx_add_module,
 .set_config = mx_set_config,
 .remove_module = mx_remove_module,
 .set_voltage = mx_set_voltage,
 .set_current_limit = mx_set_current_limit,
 .start = mx_start,
 .stop = mx_stop,
 .set_voltage_all = mx_set_voltage_all,
 .set_current_limit_all = mx_set_current_limit_all,
 .start_all = mx_start_all,
 .stop_all = mx_stop_all,
 .emergency_stop = mx_emergency_stop,
 .process = mx_process,
 .feed_frame = mx_feed_frame,
 .get_system_summary = mx_get_system_summary,
 .get_module_count = mx_get_module_count,
 .get_module_view = mx_get_module_view,
};

const CHG_LIB_DriverOps_t *CHG_LIB_MaxwellDriverOps(void)
{
 return &g_maxwell_ops;
}
