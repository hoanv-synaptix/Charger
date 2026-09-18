/**
 * @file sim_can_modules.c
 * @brief Host-side charger-module simulators — see sim_can_modules.h.
 *
 * Byte layouts and protocol constants below are copied from the firmware
 * source they simulate (Modules/chg_lib/chg_lib_maxwell.c /
 * chg_lib_lianming.c / chg_lib_tonhe.c); those constants are `static` in
 * the driver .c files (private, not part of the public API), so they are
 * re-declared here rather than shared -- if a driver's wire format ever
 * changes, this file must be updated to match (that's the whole point:
 * catching a real drift between what the firmware sends/expects and what
 * a real module does).
 */
#include "sim_can_modules.h"
#include "priv/chg_lib_protocol.h"
#include <string.h>

SimModuleState_t g_sim_modules[SIM_MAX_MODULES];

/* How many of g_sim_modules[] are "installed" -- only the Maxwell
 * transmit/tick functions iterate/match across this range; Lianming/TonHe
 * remain hardcoded to g_sim_modules[0] (g_sim_module), matching the
 * single-module scenarios they're used in. Defaults to 1 so every
 * existing single-module scenario (sim_module_reset(&g_sim_module, ...))
 * keeps working unchanged. */
static uint8_t g_sim_module_count = 1;

void sim_module_reset(SimModuleState_t *m, uint8_t addr, uint8_t group)
{
    memset(m, 0, sizeof(*m));
    m->addr = addr;
    m->group = group;
    m->rated_current = 100.0f;
    m->rated_power = 30000.0f;
    /* Every existing scenario calls this (not sim_module_reset_n()) to set
     * up a single module -- reset the active count back to 1 so a prior
     * scenario's sim_module_reset_n(N, ...) can't leak into this one. */
    g_sim_module_count = 1;
}

void sim_module_reset_n(uint8_t count, uint8_t base_addr, uint8_t group)
{
    if (count > SIM_MAX_MODULES) count = SIM_MAX_MODULES;
    if (count == 0) count = 1;
    /* sim_module_reset() itself resets g_sim_module_count to 1 (so a
     * single-module scenario can't inherit a stale count from an earlier
     * multi-module one) -- set the real count only after every slot has
     * been reset, not before. */
    for (uint8_t i = 0; i < count; i++) {
        sim_module_reset(&g_sim_modules[i], (uint8_t)(base_addr + i), group);
    }
    g_sim_module_count = count;
}

static SimModuleState_t *find_sim_module(uint8_t addr)
{
    for (uint8_t i = 0; i < g_sim_module_count; i++) {
        if (g_sim_modules[i].addr == addr) return &g_sim_modules[i];
    }
    return NULL;
}

/* ============================================================= */
/* Maxwell — request/response, big-endian, register map          */
/* ============================================================= */

#define MXR_PROTNO           0x060U
#define MXR_PTP_POINT        1U
#define MXR_ADDR_CONTROLLER  0xF0U
#define MXR_FUNC_SET         0x03U
#define MXR_FUNC_READ        0x10U
#define MXR_RESP_FLOAT       0x41U
#define MXR_RESP_INT         0x42U
#define MXR_RESP_OK          0xF0U
#define MXR_CMD_STOP_U32     0x00010000U

static uint32_t mxr_id(uint8_t dst_addr, uint8_t src_addr, uint8_t group)
{
    uint32_t id = 0;
    id |= (MXR_PROTNO & 0x1FFU) << 20;
    id |= (MXR_PTP_POINT & 0x01U) << 19;
    id |= ((uint32_t)dst_addr & 0xFFU) << 11;
    id |= ((uint32_t)src_addr & 0xFFU) << 3;
    id |= ((uint32_t)group & 0x07U);
    return id;
}

static bool sim_maxwell_transmit(uint32_t ext_id, const uint8_t *data, uint8_t dlc)
{
    if (dlc < 8) return false;
    uint8_t dst_addr = (uint8_t)((ext_id >> 11) & 0xFFU); /* module we're addressing */
    SimModuleState_t *sm = find_sim_module(dst_addr);
    if (sm == NULL) return true; /* not for any of our simulated modules */

    uint8_t func = data[0];
    uint16_t reg = ((uint16_t)data[2] << 8) | data[3];

    if (func == MXR_FUNC_SET) {
        uint32_t u = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                     ((uint32_t)data[6] << 8) | data[7];
        float f = CHG_LIB_ProtocolBEToFloat(&data[4]);
        if (reg == CHG_LIB_REG_SET_VOLTAGE && !sm->voltage_override) {
            /* Apply immediately -- a real module regulates fast enough that
             * the next poll already sees the new setpoint. */
            sm->voltage = f;
        } else if (reg == CHG_LIB_REG_SET_CURR_LIMIT) {
            sm->last_set_curr_limit_ratio = f;
        } else if (reg == CHG_LIB_REG_ON_OFF) {
            sm->actually_on = (u != MXR_CMD_STOP_U32);
            if (!sm->actually_on) {
                sm->voltage = 0.0f;
                sm->current = 0.0f;
            } else if (sm->voltage <= 0.0f) {
                sm->voltage = 1.0f; /* module reporting non-zero output */
            }
            /* No synthetic confirmation frame needed: chg_lib_maxwell.c's
             * STARTING confirm loop now polls VOLTAGE (not just
             * ALARM_STATUS), so a real module's normal poll/response cycle
             * is enough to refresh m->view.voltage -- see the BUGFIX
             * comment there. */
        }
    }

    sm->pending = true;
    sm->pending_func = func;
    sm->pending_reg = reg;
    return true;
}

static void sim_maxwell_tick_one(SimModuleState_t *sm)
{
    if (sm->silent) return;
    if (!sm->pending) return;
    sm->pending = false;

    uint8_t resp[8];
    resp[0] = MXR_RESP_FLOAT;
    resp[1] = MXR_RESP_OK;
    resp[2] = (uint8_t)(sm->pending_reg >> 8);
    resp[3] = (uint8_t)(sm->pending_reg & 0xFF);

    if (sm->actually_on && !sm->current_override) {
        sm->current = sm->rated_current * 0.5f;
    }

    switch (sm->pending_reg) {
        case CHG_LIB_REG_VOLTAGE:
            CHG_LIB_ProtocolFloatToBE(sm->voltage, &resp[4]);
            break;
        case CHG_LIB_REG_CURRENT:
            CHG_LIB_ProtocolFloatToBE(sm->current, &resp[4]);
            break;
        case CHG_LIB_REG_CURR_LIMIT:
            CHG_LIB_ProtocolFloatToBE(1.0f, &resp[4]); /* ratio: full limit */
            break;
        case CHG_LIB_REG_TEMP_DCDC:
            CHG_LIB_ProtocolFloatToBE(35.0f, &resp[4]);
            break;
        case CHG_LIB_REG_TEMP_AMBIENT:
            CHG_LIB_ProtocolFloatToBE(28.0f, &resp[4]);
            break;
        case CHG_LIB_REG_TEMP_PFC:
            CHG_LIB_ProtocolFloatToBE(40.0f, &resp[4]);
            break;
        case CHG_LIB_REG_PFC0_VOLTAGE:
            CHG_LIB_ProtocolFloatToBE(400.0f, &resp[4]);
            break;
        case CHG_LIB_REG_PFC1_VOLTAGE:
            CHG_LIB_ProtocolFloatToBE(-400.0f, &resp[4]);
            break;
        case CHG_LIB_REG_RATED_POWER:
            CHG_LIB_ProtocolFloatToBE(sm->rated_power, &resp[4]);
            break;
        case CHG_LIB_REG_RATED_CURRENT:
            CHG_LIB_ProtocolFloatToBE(sm->rated_current, &resp[4]);
            break;
        case CHG_LIB_REG_AC_PHASE_A:
        case CHG_LIB_REG_AC_PHASE_B:
        case CHG_LIB_REG_AC_PHASE_C:
            CHG_LIB_ProtocolFloatToBE(230.0f, &resp[4]);
            break;
        case CHG_LIB_REG_INPUT_DC_VOLTAGE:
            CHG_LIB_ProtocolFloatToBE(400.0f, &resp[4]);
            break;
        case CHG_LIB_REG_INPUT_MODE_RD:
            resp[0] = MXR_RESP_INT;
            CHG_LIB_ProtocolU32ToBE(3U, &resp[4]); /* 3 = three-phase AC */
            break;
        case CHG_LIB_REG_ALARM_STATUS:
            resp[0] = MXR_RESP_INT;
            CHG_LIB_ProtocolU32ToBE(sm->maxwell_alarm_raw, &resp[4]);
            break;
        case CHG_LIB_REG_INPUT_POWER:
            resp[0] = MXR_RESP_INT;
            CHG_LIB_ProtocolU32ToBE((uint32_t)(sm->voltage * sm->current), &resp[4]);
            break;
        case CHG_LIB_REG_SET_VOLTAGE:
        case CHG_LIB_REG_SET_CURR_LIMIT:
        case CHG_LIB_REG_SET_OVP:
        case CHG_LIB_REG_ON_OFF:
        case CHG_LIB_REG_SHORT_RESET:
        case CHG_LIB_REG_INPUT_MODE_SET:
            memset(&resp[4], 0, 4); /* write ack: value unused by apply_response() */
            break;
        default:
            return; /* unknown register: no reply (matches a real module ignoring it) */
    }

    uint32_t resp_id = mxr_id(MXR_ADDR_CONTROLLER, sm->addr, sm->group);
    CHG_LIB_FeedCanFrame(resp_id, resp, 8);
}

static void sim_maxwell_tick(uint32_t now_tick)
{
    (void)now_tick;
    for (uint8_t i = 0; i < g_sim_module_count; i++) {
        sim_maxwell_tick_one(&g_sim_modules[i]);
    }
}

/* ============================================================= */
/* Lianming — request/response, big-endian, fixed command bytes  */
/* ============================================================= */

#define LM_CMD_BASE        0x1907C080U
#define LM_RESP_BASE       0x1807C080U
#define LM_ADDR_MASK       0x7FU
#define LM_CMD_SET_OUTPUT  0x00U
#define LM_CMD_READ_INFO   0x01U
#define LM_CMD_START_STOP  0x02U
#define LM_START_VALUE     0x55U
#define LM_STOP_VALUE      0xAAU

static bool sim_lianming_transmit(uint32_t ext_id, const uint8_t *data, uint8_t dlc)
{
    if (dlc < 8) return false;
    uint32_t id_base = ext_id & ~LM_ADDR_MASK;
    if (id_base != LM_CMD_BASE) return true; /* AC/temp diagnostic reads: not simulated */
    uint8_t addr = (uint8_t)(ext_id & LM_ADDR_MASK);
    if (addr != g_sim_module.addr) return true;

    uint8_t cmd = data[0];
    if (cmd == LM_CMD_START_STOP) {
        g_sim_module.actually_on = (data[7] == LM_START_VALUE);
        if (!g_sim_module.actually_on) {
            g_sim_module.voltage = 0.0f;
            g_sim_module.current = 0.0f;
        } else if (g_sim_module.voltage <= 0.0f) {
            g_sim_module.voltage = 1.0f;
        }
    } else if (cmd == LM_CMD_SET_OUTPUT) {
        /* Byte1-3 current(mA), Byte4-7 voltage(mV) per header -- not needed
         * for state-machine assertions, skip decoding the exact scale. */
    }

    g_sim_module.pending = true;
    g_sim_module.pending_func = cmd;
    return true;
}

static void sim_lianming_tick(uint32_t now_tick)
{
    (void)now_tick;
    if (g_sim_module.silent) return;
    if (!g_sim_module.pending) return;
    g_sim_module.pending = false;

    uint32_t resp_id = LM_RESP_BASE | g_sim_module.addr;

    if (g_sim_module.pending_func == LM_CMD_START_STOP ||
        g_sim_module.pending_func == LM_CMD_SET_OUTPUT) {
        uint8_t resp[8] = { g_sim_module.pending_func, 0x01, 0, 0, 0, 0, 0, 0 };
        CHG_LIB_FeedCanFrame(resp_id, resp, 8);
        return;
    }

    /* LM_CMD_READ_INFO: bytes 2-3=current(0.1A/bit BE), 4-5=voltage(0.1V/bit BE),
     * 6-7=status_flags (bit0=0 means running; see chg_lib_lianming.c) */
    if (g_sim_module.actually_on) {
        g_sim_module.current = g_sim_module.rated_current * 0.5f;
    }
    uint16_t curr_raw = (uint16_t)(g_sim_module.current * 10.0f);
    uint16_t volt_raw = (uint16_t)(g_sim_module.voltage * 10.0f);
    uint16_t status = g_sim_module.lianming_status_raw;
    if (g_sim_module.actually_on) status &= (uint16_t)~0x01U;
    else status |= 0x01U;

    uint8_t resp[8];
    resp[0] = LM_CMD_READ_INFO;
    resp[1] = 0x00;
    resp[2] = (uint8_t)(curr_raw >> 8);
    resp[3] = (uint8_t)(curr_raw & 0xFF);
    resp[4] = (uint8_t)(volt_raw >> 8);
    resp[5] = (uint8_t)(volt_raw & 0xFF);
    resp[6] = (uint8_t)(status >> 8);
    resp[7] = (uint8_t)(status & 0xFF);
    CHG_LIB_FeedCanFrame(resp_id, resp, 8);
}

/* ============================================================= */
/* TonHe — J1939 broadcast, little-endian                        */
/* ============================================================= */

#define TONHE_ADDR_CONTROLLER  0xA0U
#define TONHE_PRIORITY_STATUS  6U
#define TONHE_STATUS_NORMAL_OFF 0x00U
#define TONHE_STATUS_ON         0x01U
#define TONHE_CMD_STOP          0x55U
#define TONHE_CMD_START         0xAAU
#define TONHE_VOLTAGE_SCALE     0.1f
#define TONHE_CURRENT_SCALE     0.01f

static uint32_t tonhe_id(uint8_t pf, uint8_t ps, uint8_t sa, uint8_t priority)
{
    return (((uint32_t)priority & 0x07U) << 26) |
           (((uint32_t)pf & 0xFFU) << 16) |
           (((uint32_t)ps & 0xFFU) << 8) |
           ((uint32_t)sa & 0xFFU);
}

static bool sim_tonhe_transmit(uint32_t ext_id, const uint8_t *data, uint8_t dlc)
{
    if (dlc < 8) return false;
    uint8_t pf = (uint8_t)((ext_id >> 16) & 0xFFU);
    uint8_t ps = (uint8_t)((ext_id >> 8) & 0xFFU); /* destination = module addr for pf=0x06 */

    if (pf == 0x06 && ps == g_sim_module.addr) {
        /* C_M_24: specific module start/stop */
        g_sim_module.actually_on = (data[0] == TONHE_CMD_START);
        if (!g_sim_module.actually_on) {
            g_sim_module.voltage = 0.0f;
            if (!g_sim_module.current_override) {
                g_sim_module.current = 0.0f;
            }
        } else if (g_sim_module.voltage <= 0.0f) {
            g_sim_module.voltage = 1.0f;
        }
    } else if (pf == 0x03 && (ps == g_sim_module.addr || ps == 0xFFU)) {
        /* C_M_1: broadcast start/stop */
        g_sim_module.actually_on = (data[3] == TONHE_CMD_START);
    }
    /* pf==0x04 (broadcast parameter set) / 0x05 (timing) -- not simulated,
     * the module doesn't need to react for our scenarios. */
    return true;
}

/* Broadcasts the module's M_C_1 status frame unconditionally -- TonHe is a
 * real broadcast protocol (per docs/AUDIT), the module doesn't wait to be
 * asked. Call every simulated tick (App_Loop calls tonhe_process() every
 * iteration too; the driver's own 1s "timing command" cadence doesn't
 * gate the module's own status broadcast). */
static void sim_tonhe_tick(uint32_t now_tick)
{
    (void)now_tick;
    if (g_sim_module.silent) return;
    if (!g_sim_module.current_override) {
        if (g_sim_module.actually_on) {
            g_sim_module.current = g_sim_module.rated_current * 0.5f;
        } else {
            g_sim_module.current = 0.0f;
        }
    }

    uint8_t status = g_sim_module.actually_on ? TONHE_STATUS_ON : TONHE_STATUS_NORMAL_OFF;
    /* TonHe bits 12/13 are warnings and do not turn the module off. */
    const uint16_t tonhe_protection_bits =
        (uint16_t)(g_sim_module.tonhe_fault_bits & (uint16_t)~((1U << 12) | (1U << 13)));
    if (tonhe_protection_bits != 0U || g_sim_module.tonhe_pfc_bits != 0U) {
        status = 0x11U; /* TONHE_STATUS_FAULT_OFF */
    }

    uint16_t voltage_raw = (uint16_t)(g_sim_module.voltage / TONHE_VOLTAGE_SCALE);
    uint16_t current_raw = (uint16_t)(g_sim_module.current / TONHE_CURRENT_SCALE);

    uint8_t data[8];
    data[0] = status;
    data[1] = (uint8_t)(voltage_raw & 0xFF);
    data[2] = (uint8_t)(voltage_raw >> 8);
    data[3] = (uint8_t)(current_raw & 0xFF);
    data[4] = (uint8_t)(current_raw >> 8);
    data[5] = (uint8_t)(g_sim_module.tonhe_fault_bits & 0xFF);
    data[6] = (uint8_t)(g_sim_module.tonhe_fault_bits >> 8);
    data[7] = g_sim_module.tonhe_pfc_bits;

    uint32_t id = tonhe_id(0x01, TONHE_ADDR_CONTROLLER, g_sim_module.addr, TONHE_PRIORITY_STATUS);
    CHG_LIB_FeedCanFrame(id, data, 8);
}

/* ============================================================= */
/* Public API                                                     */
/* ============================================================= */

static const CHG_LIB_CanBackend_t g_maxwell_backend  = { .transmit = sim_maxwell_transmit,  .now_tick = 0 };
static const CHG_LIB_CanBackend_t g_lianming_backend = { .transmit = sim_lianming_transmit, .now_tick = 0 };
static const CHG_LIB_CanBackend_t g_tonhe_backend    = { .transmit = sim_tonhe_transmit,    .now_tick = 0 };

static uint32_t sim_now_tick_holder = 0;
static uint32_t sim_now_tick(void) { return sim_now_tick_holder; }

void sim_install_backend(SimDriverKind_t kind)
{
    CHG_LIB_CanBackend_t backend;
    switch (kind) {
        case SIM_DRV_MAXWELL:  backend = g_maxwell_backend;  break;
        case SIM_DRV_LIANMING: backend = g_lianming_backend; break;
        default:                backend = g_tonhe_backend;    break;
    }
    backend.now_tick = sim_now_tick;
    /* CHG_LIB_CanBackend_Set() stores the pointer as-is, so it must point
     * at storage that outlives this call -- keep one static per kind. */
    static CHG_LIB_CanBackend_t s_active[3];
    s_active[kind] = backend;
    CHG_LIB_CanBackend_Set(&s_active[kind]);
}

void sim_module_tick(SimDriverKind_t kind, uint32_t now_tick)
{
    sim_now_tick_holder = now_tick;
    switch (kind) {
        case SIM_DRV_MAXWELL:  sim_maxwell_tick(now_tick);  break;
        case SIM_DRV_LIANMING: sim_lianming_tick(now_tick); break;
        case SIM_DRV_TONHE:    sim_tonhe_tick(now_tick);    break;
    }
}
