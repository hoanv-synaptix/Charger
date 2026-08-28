/**
 * @file pc_debug_protocol.c
 * @brief PC Debug Protocol Implementation
 * @note Handles debug commands from PC and streams module data
 */

#include "pc_debug_protocol.h"
#include "pc_protocol.h"
#include "chg_lib.h"
#include "bms_core.h"
#include "charge_cycle_config.h"
#include "charge_cycle_storage.h"
#include "charge_controller.h"
#include "debug_log.h"
#include "bsp_can.h"
#include "bsp_sys.h"
#include <string.h>

/* ============== Private State ============== */

static bool g_debug_active = false;
static uint32_t g_last_stream_tick = 0;
static uint8_t g_stream_sequence = 0;

/* ============== Public API ============== */

void DebugProtocol_Init(void)
{
    g_debug_active = false;
    g_last_stream_tick = 0;
    g_stream_sequence = 0;
}

void DebugProtocol_Enter(void)
{
    g_debug_active = true;
    g_stream_sequence = 0;
    g_last_stream_tick = BSP_GetTick();
    /* No LOG here — runs in USB ISR context */
}

void DebugProtocol_Exit(void)
{
    g_debug_active = false;
    /* No LOG here — runs in USB ISR context */
}

bool DebugProtocol_IsActive(void)
{
    return g_debug_active;
}

uint16_t DebugProtocol_BuildModuleData(uint8_t idx, uint8_t *data, uint16_t max_len)
{
    /* BUGFIX: this used to have no max_len parameter at all and would
     * unconditionally write sizeof(DebugModuleData_t) (123) bytes into
     * `data`. DebugProtocol_BuildAllModulesData()'s loop calls this at an
     * advancing offset into a 255-byte (PC_MAX_PAYLOAD) stack buffer and
     * only checked the offset fit *after* this function had already
     * written -- on any system with 3+ modules (a normal configuration;
     * see B-10) the 3rd module's write landed past the end of the caller's
     * buffer, corrupting the stack. Checking here, before writing
     * anything, is the actual fix -- the call-site check in
     * BuildAllModulesData() alone cannot prevent this since it runs too
     * late. */
    if (max_len < sizeof(DebugModuleData_t)) {
        return 0;
    }

    CHG_LIB_ModuleView_t view;
    if (!CHG_LIB_GetModuleView(idx, &view)) {
        return 0;
    }

    DebugModuleData_t *mod = (DebugModuleData_t *)data;
    memset(mod, 0, sizeof(DebugModuleData_t));

    /* Identity */
    mod->module_idx = idx;
    mod->driver_id = CHG_LIB_GetActiveDriverId();
    mod->enabled = view.enabled ? 1U : 0U;
    mod->online = view.online ? 1U : 0U;
    mod->running = view.running ? 1U : 0U;
    mod->state = (uint8_t)view.state;

    /* Output */
    mod->voltage = view.voltage;
    mod->current = view.current;
    mod->current_limit = view.current_limit;

    /* Temperatures */
    mod->temp_dcdc = view.temp_dcdc;
    mod->temp_ambient = view.temp_ambient;
    mod->temp_pfc = view.temp_pfc;

    /* AC Input 3-phase */
    mod->ac_phase_a_voltage = view.ac_phase_a_voltage;
    mod->ac_phase_b_voltage = view.ac_phase_b_voltage;
    mod->ac_phase_c_voltage = view.ac_phase_c_voltage;

    /* PFC Bus */
    mod->pfc_bus_pos_voltage = view.pfc_bus_pos_voltage;
    mod->pfc_bus_neg_voltage = view.pfc_bus_neg_voltage;

    /* Power & Ratings */
    mod->input_power = view.input_power;
    mod->rated_power = view.rated_power;
    mod->rated_current = view.rated_current;

    /* Alarms */
    mod->alarm_status = view.alarm_status;
    mod->alarm_flags = (uint32_t)view.alarm_flags;
    mod->pfc_fault = view.pfc_fault;

    /* Address */
    mod->addr = view.addr;
    mod->group = view.group;

    /* Timing */
    mod->last_rx_tick = view.last_rx_tick;
    mod->last_tx_tick = view.last_tx_tick;

    /* Communication Stats */
    mod->tx_count = view.stats.tx_count;
    mod->rx_count = view.stats.rx_count;
    mod->error_count = view.stats.error_count;
    mod->timeout_count = view.stats.timeout_count;
    mod->recovery_count = view.stats.recovery_count;

    /* Vendor extension: Maxwell-only input diagnostics (0x0005 input DC
     * voltage, 0x004B input working mode) -- added 2026-08-29, reusing
     * this already-reserved field instead of growing the wire struct
     * (would break every existing offset in debug_app's Python parser).
     * vendor_data[0..3] = input_dc_voltage (float, native byte order,
     * same as every other float field in this struct); vendor_data[4] =
     * input_mode. Zero/unset on drivers that don't populate these
     * (Lianming/TonHe). */
    memcpy(mod->vendor_data, &view.input_dc_voltage, sizeof(float));
    mod->vendor_data[4] = view.input_mode;
    mod->vendor_data_len = 5U;

    return sizeof(DebugModuleData_t);
}

uint16_t DebugProtocol_BuildAllModulesData(uint8_t *data, uint16_t max_len)
{
    uint16_t written = 0;
    uint8_t module_count = CHG_LIB_GetModuleCount();
    uint8_t modules_written = 0;

    /* Check minimum space for header */
    if (written + 2 > max_len) {
        return 0;
    }

    /* Header: sequence + module count */
    data[written++] = g_stream_sequence++;
    data[written++] = 0U;

    /* Build data for each module. Pass the actual remaining space
     * (max_len - written) so BuildModuleData() can refuse to write past
     * the caller's buffer itself, instead of relying on a check here that
     * runs after the write already happened (see the BUGFIX comment on
     * BuildModuleData()). */
    for (uint8_t i = 0; i < module_count; i++) {
        uint16_t mod_len = DebugProtocol_BuildModuleData(i, &data[written], (uint16_t)(max_len - written));
        if (mod_len == 0) {
            break;
        }
        written += mod_len;
        modules_written++;
    }

    data[1] = modules_written;
    return written;
}

uint16_t DebugProtocol_BuildSystemInfo(uint8_t *data, uint16_t max_len)
{
    if (max_len < sizeof(DebugSystemInfo_t)) {
        return 0;
    }

    DebugSystemInfo_t *info = (DebugSystemInfo_t *)data;
    CHG_LIB_SystemSummary_t summary;
    ChargeCtrlView_t ctrl_view;
    CHG_LIB_ModuleView_t module_view;
    float max_temp_dcdc = 0.0f;

    CHG_LIB_GetSystemSummary(&summary);
    ChargeController_GetView(&ctrl_view);

    BMS_View_t bms_view;
    BMS_GetView(&bms_view);

    memset(info, 0, sizeof(DebugSystemInfo_t));

    info->fw_major = FW_VERSION_MAJOR;
    info->fw_minor = FW_VERSION_MINOR;
    info->fw_patch = FW_VERSION_PATCH;
    info->uptime_ticks = BSP_GetTick();

    info->driver_id = CHG_LIB_GetActiveDriverId();
    info->modules_total = CHG_LIB_GetModuleCount();
    info->modules_online = summary.modules_online;
    info->modules_fault = summary.modules_fault;
    info->charging = ctrl_view.running ? 1U : 0U;
    info->controller_state = (uint8_t)ctrl_view.state;
    info->controller_derating = ctrl_view.derating;
    info->controller_inhibit = ctrl_view.inhibit;
    info->charge_source_mode = ctrl_view.charge_source_mode;
    info->active_limit_source = ctrl_view.active_limit_source;
    info->active_stage_band = ctrl_view.active_stage_band;

    for (uint8_t i = 0; i < CHG_LIB_GetModuleCount(); i++) {
        if (!CHG_LIB_GetModuleView(i, &module_view) || !module_view.enabled) {
            continue;
        }
        if (module_view.temp_dcdc > max_temp_dcdc) {
            max_temp_dcdc = module_view.temp_dcdc;
        }
    }

    info->total_voltage = summary.voltage;
    info->total_current = summary.total_current;
    info->total_power_in = summary.total_power_in;
    info->max_temp_dcdc = max_temp_dcdc;
    info->controller_target_voltage = ctrl_view.target_voltage_v;
    info->controller_target_current_total = ctrl_view.target_current_total_a;
    info->active_limit_current_c = ctrl_view.active_limit_current_c;
    
    /* Go through the accessor BSP already exposes for this instead of a
     * local `extern` reaching straight into bsp_can.c's file-scope
     * globals -- same counters, but through the API BSP_CAN_GetStats()
     * (bsp_can.h) exists specifically to provide.
     * Read into aligned locals first, then assign into the struct:
     * DebugSystemInfo_t is __attribute__((packed)) for the wire format,
     * so &info->canN_xx_count is not guaranteed 4-byte aligned (it isn't,
     * at this struct's actual layout) -- passing that address straight to
     * BSP_CAN_GetStats() for a real uint32_t* store would be an unaligned
     * write, which Cortex-M0+ (this MCU) does not support in hardware.
     * Plain assignment into a packed member, like every other field in
     * this function, is safe -- the compiler emits the correct unaligned
     * store for that case. */
    uint32_t c1tx, c1rx, c2tx, c2rx;
    BSP_CAN_GetStats(&c1tx, &c1rx, &c2tx, &c2rx);
    info->can1_tx_count = c1tx;
    info->can1_rx_count = c1rx;
    info->can2_tx_count = c2tx;
    info->can2_rx_count = c2rx;
    
    info->controller_fault_flags = ctrl_view.fault_flags;
    info->controller_stop_reason = (uint8_t)ctrl_view.stop_reason;
    info->bms_stale = ((bms_view.alarm_flags & BMS_ALARM_STALE_DATA) != 0) ? 1U : 0U;


    return sizeof(DebugSystemInfo_t);
}

uint16_t DebugProtocol_BuildBMSData(uint8_t *data, uint16_t max_len)
{
    /* Must match the actual byte count this function writes below (state/
     * relays 4 + battery 18 + cell volts 4 + temps 8 + charge req 8 +
     * alarm/timing 8 = 50). Was checked against 54 (4 bytes of stale
     * slack from an earlier version of this function) -- harmless today
     * since under-checking here can only reject valid calls, never
     * overflow, but a mismatched guard stops documenting the function's
     * real output size. */
    if (max_len < 50) {
        return 0;
    }

    BMS_View_t bms;
    BMS_GetView(&bms);
    uint16_t written = 0;
    float cap_remain_ah = BMS_RAW_TO_CAP_AH(bms.cap_remain);
    float rate_cap_ah = BMS_RAW_TO_CAP_AH(bms.rate_cap);

    /* State and relay info (4 bytes) */
    data[written++] = (uint8_t)bms.state;
    data[written++] = bms.online ? 1 : 0;
    data[written++] = bms.charge_relay_closed ? 1 : 0;
    data[written++] = bms.discharge_relay_closed ? 1 : 0;

    /* Battery (20 bytes) */
    memcpy(&data[written], &bms.batt_voltage, sizeof(float)); written += 4;
    memcpy(&data[written], &bms.batt_current, sizeof(float)); written += 4;
    memcpy(&data[written], &cap_remain_ah, sizeof(float));    written += 4;
    memcpy(&data[written], &rate_cap_ah, sizeof(float));      written += 4;
    data[written++] = bms.soc;
    data[written++] = bms.soh;

    /* Cell voltages (4 bytes) */
    uint16_t max_cv = (uint16_t)bms.max_cell_volt;
    uint16_t min_cv = (uint16_t)bms.min_cell_volt;
    data[written++] = (uint8_t)(max_cv & 0xFF);
    data[written++] = (uint8_t)(max_cv >> 8);
    data[written++] = (uint8_t)(min_cv & 0xFF);
    data[written++] = (uint8_t)(min_cv >> 8);

    /* Temperatures (8 bytes) */
    memcpy(&data[written], &bms.max_cell_temp, sizeof(float)); written += 4;
    memcpy(&data[written], &bms.min_cell_temp, sizeof(float)); written += 4;

    /* Charging requests (8 bytes) */
    memcpy(&data[written], &bms.chg_volt_request, sizeof(float)); written += 4;
    memcpy(&data[written], &bms.chg_curr_request, sizeof(float)); written += 4;

    /* Alarms + timing (8 bytes) */
    uint32_t alarm = (uint32_t)bms.alarm_flags;
    memcpy(&data[written], &alarm, sizeof(uint32_t)); written += 4;
    memcpy(&data[written], &bms.last_rx_tick, sizeof(uint32_t)); written += 4;

    return written;
}

uint16_t DebugProtocol_BuildCommStats(uint8_t idx, uint8_t *data)
{
    CHG_LIB_ModuleView_t view;
    if (!CHG_LIB_GetModuleView(idx, &view)) {
        return 0;
    }

    uint16_t written = 0;

    /* Module index */
    data[written++] = idx;

    /* Statistics */
    memcpy(&data[written], &view.stats.tx_count, sizeof(uint32_t)); written += 4;
    memcpy(&data[written], &view.stats.rx_count, sizeof(uint32_t)); written += 4;
    memcpy(&data[written], &view.stats.error_count, sizeof(uint32_t)); written += 4;
    memcpy(&data[written], &view.stats.timeout_count, sizeof(uint32_t)); written += 4;
    memcpy(&data[written], &view.stats.recovery_count, sizeof(uint32_t)); written += 4;

    return written;
}

uint16_t DebugProtocol_BuildChargeConfig(uint8_t *data, uint16_t max_len)
{
    ChargeCycleConfig_t config;

    if (max_len < sizeof(config)) {
        return 0;
    }

    ChargeCycleConfig_Get(&config);
    config.version = CHARGE_CYCLE_CONFIG_VERSION;
    memcpy(data, &config, sizeof(config));
    return sizeof(config);
}

void DebugProtocol_SendStream(void)
{
    if (!g_debug_active) {
        return;
    }

    uint32_t now = BSP_GetTick();
    if ((now - g_last_stream_tick) < DEBUG_STREAM_INTERVAL_MS) {
        return;
    }
    g_last_stream_tick = now;

    uint8_t buf[PC_MAX_PAYLOAD];
    uint16_t len;

    len = DebugProtocol_BuildAllModulesData(buf, sizeof(buf));
    if (len > 0) {
        PC_Protocol_SendFrame(DEBUG_RSP_ALL_MODULES, buf, len);
    }

    len = DebugProtocol_BuildSystemInfo(buf, sizeof(buf));
    if (len > 0) {
        PC_Protocol_SendFrame(DEBUG_RSP_SYSTEM_INFO, buf, len);
    }

    len = DebugProtocol_BuildBMSData(buf, sizeof(buf));
    if (len > 0) {
        PC_Protocol_SendFrame(DEBUG_RSP_BMS_DATA, buf, len);
    }
}

bool DebugProtocol_HandleCommand(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    uint8_t reply[PC_MAX_PAYLOAD];

    switch (cmd) {
    case DEBUG_CMD_ENTER:
        PC_Protocol_ResetTx();
        DebugProtocol_Enter();
        PC_Protocol_SendFrame(PC_RSP_ACK, &cmd, 1);
        return true;

    case DEBUG_CMD_EXIT:
        DebugProtocol_Exit();
        PC_Protocol_SendFrame(PC_RSP_ACK, &cmd, 1);
        return true;

    case DEBUG_CMD_READ_ALL: {
        uint16_t data_len = DebugProtocol_BuildAllModulesData(reply, sizeof(reply));
        if (data_len > 0) {
            PC_Protocol_SendFrame(DEBUG_RSP_ALL_MODULES, reply, data_len);
        } else {
            reply[0] = 0x01; /* BAD_PARAM */
            PC_Protocol_SendFrame(DEBUG_RSP_ERROR, reply, 1);
        }
        return true;
    }

    case DEBUG_CMD_READ_ONE: {
        if (len < 1) {
            reply[0] = 0x01; /* BAD_PARAM */
            PC_Protocol_SendFrame(DEBUG_RSP_ERROR, reply, 1);
            return true;
        }
        uint16_t data_len = DebugProtocol_BuildModuleData(payload[0], reply, sizeof(reply));
        if (data_len > 0) {
            PC_Protocol_SendFrame(DEBUG_RSP_MODULE_DATA, reply, data_len);
        } else {
            reply[0] = 0x02; /* MODULE_OFFLINE */
            PC_Protocol_SendFrame(DEBUG_RSP_ERROR, reply, 1);
        }
        return true;
    }

    case DEBUG_CMD_READ_STATS: {
        if (len < 1) {
            reply[0] = 0x01; /* BAD_PARAM */
            PC_Protocol_SendFrame(DEBUG_RSP_ERROR, reply, 1);
            return true;
        }
        uint16_t data_len = DebugProtocol_BuildCommStats(payload[0], reply);
        if (data_len > 0) {
            PC_Protocol_SendFrame(DEBUG_RSP_COMM_STATS, reply, data_len);
        } else {
            reply[0] = 0x02; /* MODULE_OFFLINE */
            PC_Protocol_SendFrame(DEBUG_RSP_ERROR, reply, 1);
        }
        return true;
    }

    case DEBUG_CMD_READ_BMS: {
        uint16_t data_len = DebugProtocol_BuildBMSData(reply, sizeof(reply));
        if (data_len > 0) {
            PC_Protocol_SendFrame(DEBUG_RSP_BMS_DATA, reply, data_len);
        }
        return true;
    }

    case DEBUG_CMD_GET_SYSTEM: {
        uint16_t data_len = DebugProtocol_BuildSystemInfo(reply, sizeof(reply));
        if (data_len > 0) {
            PC_Protocol_SendFrame(DEBUG_RSP_SYSTEM_INFO, reply, data_len);
        }
        return true;
    }

    case DEBUG_CMD_GET_CHARGE_CFG: {
        /* No LOG here — runs in USB ISR context */
        uint16_t data_len = DebugProtocol_BuildChargeConfig(reply, sizeof(reply));
        if (data_len > 0) {
            PC_Protocol_SendFrame(DEBUG_RSP_CHARGE_CFG, reply, data_len);
        } else {
            reply[0] = 0x03; /* NOT_SUPPORTED / BUFFER */
            PC_Protocol_SendFrame(DEBUG_RSP_ERROR, reply, 1);
        }
        return true;
    }

    case DEBUG_CMD_SET_CHARGE_CFG: {
        ChargeCycleConfig_t config;

        if (len != sizeof(config)) {
            reply[0] = 0x01; /* BAD_PARAM */
            PC_Protocol_SendFrame(DEBUG_RSP_ERROR, reply, 1);
            return true;
        }

        memcpy(&config, payload, sizeof(config));

        /* Validate and set to RAM */
        if (!ChargeCycleConfig_Set(&config)) {
            reply[0] = 0x01; /* BAD_PARAM */
            PC_Protocol_SendFrame(DEBUG_RSP_ERROR, reply, 1);
            return true;
        }

        /* Save to flash */
        if (!ChargeCycleStorage_Save(&config)) {
            reply[0] = 0x04; /* FLASH_SAVE_FAIL */
            PC_Protocol_SendFrame(DEBUG_RSP_ERROR, reply, 1);
            return true;
        }

        uint16_t data_len = DebugProtocol_BuildChargeConfig(reply, sizeof(reply));
        if (data_len > 0) {
            PC_Protocol_SendFrame(DEBUG_RSP_CHARGE_CFG, reply, data_len);
        } else {
            PC_Protocol_SendFrame(PC_RSP_ACK, &cmd, 1);
        }
        return true;
    }

    case DEBUG_CMD_WRITE_REG: {
        /* payload: [module_idx(1)][reg_h(1)][reg_l(1)][data(4)] = 7 bytes */
        if (len < 7) {
            reply[0] = 0x01; /* BAD_PARAM */
            PC_Protocol_SendFrame(DEBUG_RSP_ERROR, reply, 1);
            return true;
        }
        uint8_t idx = payload[0];
        uint16_t reg = ((uint16_t)payload[1] << 8) | payload[2];
        float val;
        memcpy(&val, &payload[3], 4);
        bool ok = false;
        switch (reg) {
        case 0x0021: ok = CHG_LIB_SetVoltage(idx, val); break;
        case 0x0012: ok = CHG_LIB_SetCurrentLimit(idx, val); break;
        case 0x0030:
            if (val > 0.0f) ok = CHG_LIB_Start(idx);
            else            ok = CHG_LIB_Stop(idx);
            break;
        default: break;
        }
        reply[0] = ok ? 0x00 : 0x01;
        PC_Protocol_SendFrame(DEBUG_RSP_RAW_CAN_TX, reply, 1);
        return true;
    }

#ifdef CHG_DEBUG_RAW_CAN
    case DEBUG_CMD_SEND_RAW_CAN: {
        /* Bench-debug only: injects an arbitrary frame on either CAN bus,
         * bypassing BMS/chg_lib entirely. Deliberately excluded from
         * Release builds (see CHG_DEBUG_RAW_CAN in CMakeLists.txt) so a
         * production unit's USB debug port cannot be used to send
         * arbitrary commands onto the charger/BMS bus. */
        /* payload: [bus(1)][id(4)][dlc(1)][data(8)] = 14 bytes */
        if (len < 14) {
            reply[0] = 0x01; /* BAD_PARAM */
            PC_Protocol_SendFrame(DEBUG_RSP_ERROR, reply, 1);
            return true;
        }
        BSP_CAN_Frame_t frame;
        uint8_t bus = payload[0];
        memcpy(&frame.ext_id, &payload[1], 4);
        frame.dlc = payload[5];
        memcpy(frame.data, &payload[6], 8);
        bool ok = BSP_CAN_Transmit(bus, &frame);
        reply[0] = ok ? 0x00 : 0x01;
        PC_Protocol_SendFrame(DEBUG_RSP_RAW_CAN_TX, reply, 1);
        return true;
    }
#endif /* CHG_DEBUG_RAW_CAN */

    default:
        /* Not handled - let standard protocol handler try */
        return false;
    }
}

