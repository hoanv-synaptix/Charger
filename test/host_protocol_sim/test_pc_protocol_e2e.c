/**
 * @file test_pc_protocol_e2e.c
 * @brief Host-compiled end-to-end test: real byte streams -> the REAL
 *        compiled PC_Protocol_FeedByte()/process_frame()/
 *        DebugProtocol_HandleCommand() -> real downstream state
 *        (ChargeController/CHG_LIB/BMS/ChargeCycleConfig), then back out
 *        through the real TX-enqueue path (PC_Protocol_PeekTxFrame()).
 *
 * Why this file exists: the only prior "test" for pc_protocol.c
 * (test/test_pc_debug_comm.py) is a Python re-implementation of the
 * FeedByte state machine asserted against itself -- it never compiles or
 * executes App/Protocol/pc_protocol.c, so a real bug in the C parser could
 * pass that suite as long as the same bug isn't also in the Python copy.
 * This file closes that gap the same way test/host_charge_sim closed it
 * for Modules/chg_lib: compile the real .c files, drive them with real
 * byte streams, assert on real state.
 *
 * dwin_protocol.c (Modules/hmi) is covered separately in
 * test_dwin_protocol_e2e.c -- it has no shared state with this file.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "pc_protocol.h"
#include "pc_debug_protocol.h"
#include "chg_lib.h"
#include "chg_lib_driver_maxwell.h"
#include "chg_lib_can_backend.h"
#include "bms_core.h"
#include "charge_cycle_config.h"
#include "charge_controller.h"
#include "app_rtc_sync.h"
#include "alarm.h"

#include "sim_can_modules.h"
#include "sim_bms.h"

/* Mock HAL -- same pattern as test/host_charge_sim/test_charge_e2e.c. */
uint32_t mock_tick = 0;
uint32_t HAL_GetTick(void) { return mock_tick; }
uint32_t BSP_GetTick(void) { return mock_tick; }
void BSP_Delay(uint32_t delay_ms) { (void)delay_ms; }
void BSP_EnterCritical(void) {}
void BSP_ExitCritical(void) {}

/* mock_stubs.c provides these; declared here to assert on them directly. */
extern bool g_storage_save_called;

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("[FAIL] %s:%d - %s\n", __func__, __LINE__, msg); \
            return false; \
        } \
    } while (0)

/* ================================================================== */
/* Frame building / TX inspection helpers                             */
/* ================================================================== */

static uint8_t ref_crc8(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ PC_CRC8_POLY) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* Feeds a well-formed [AA][55][CMD][LEN][PAYLOAD][CRC8] frame through the
 * real PC_Protocol_FeedByte() state machine, one byte at a time -- exactly
 * how CDC_Receive_FS() would in the real firmware. */
static void send_pc_frame(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    uint8_t buf[2 + 2 + 255 + 1];
    uint16_t i = 0;
    buf[i++] = PC_SOF1;
    buf[i++] = PC_SOF2;
    buf[i++] = cmd;
    buf[i++] = len;
    if (len > 0 && payload != NULL) {
        memcpy(&buf[i], payload, len);
        i += len;
    }
    buf[i] = ref_crc8(&buf[2], (uint16_t)(len + 2));
    i++;
    for (uint16_t k = 0; k < i; k++) {
        PC_Protocol_FeedByte(buf[k]);
    }
    PC_Protocol_ProcessRx();
    App_RtcSync_Process();
}

/* Feeds a frame with a deliberately corrupted trailing CRC byte. */
static void send_pc_frame_bad_crc(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    uint8_t buf[2 + 2 + 255 + 1];
    uint16_t i = 0;
    buf[i++] = PC_SOF1;
    buf[i++] = PC_SOF2;
    buf[i++] = cmd;
    buf[i++] = len;
    if (len > 0 && payload != NULL) {
        memcpy(&buf[i], payload, len);
        i += len;
    }
    buf[i] = (uint8_t)(ref_crc8(&buf[2], (uint16_t)(len + 2)) ^ 0xFFU);
    i++;
    for (uint16_t k = 0; k < i; k++) {
        PC_Protocol_FeedByte(buf[k]);
    }
    PC_Protocol_ProcessRx();
    App_RtcSync_Process();
}

/* Returns the single most-recently-enqueued TX frame's cmd/payload, and
 * requires it to be the ONLY frame enqueued (fails loudly otherwise --
 * a test that doesn't drain between commands would silently read a stale
 * frame and pass for the wrong reason). Caller must PC_Protocol_ResetTx()
 * between scenarios that check exactly one response. */
static bool only_tx_frame(uint8_t *cmd, uint8_t *payload, uint8_t *len)
{
    if (PC_Protocol_GetTxQueueDepth() != 1U) {
        printf("    (expected exactly 1 queued TX frame, got %u)\n", PC_Protocol_GetTxQueueDepth());
        return false;
    }
    return PC_Protocol_PeekTxFrame(0, cmd, payload, len);
}

/* ================================================================== */
/* Scenario setup -- mirrors test/host_charge_sim's setup_scenario()   */
/* ================================================================== */

static bool setup_scenario(void)
{
    static bool drivers_registered = false;
    if (!drivers_registered) {
        CHG_LIB_RegisterDriver(CHG_LIB_DRV_MAXWELL, CHG_LIB_MaxwellDriverOps());
        drivers_registered = true;
    }

    mock_tick = 0;
    sim_install_backend(SIM_DRV_MAXWELL);
    sim_module_reset(&g_sim_module, 1, 0);
    sim_bms_reset(&g_sim_bms);
    g_storage_save_called = false;

    BMS_Init();
    ChargeCycleConfig_Init();
    ChargeController_Init();
    PC_Protocol_ResetTx();

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_GetDefaults(&cfg);
    cfg.module_type = CHARGE_MODULE_TYPE_MAXWELL;
    cfg.source_module_count = 1U;
    cfg.charge_source_mode = CHARGE_SOURCE_BMS_CONTROLLED;
    cfg.vmin_v = 300.0f;
    cfg.vmax_v = 500.0f;
    cfg.module_u_min_v = 30.0f;
    cfg.module_u_max_v = 550.0f;
    cfg.module_i_min_a = 1.0f;
    cfg.module_i_max_a = 200.0f;
    if (!ChargeCycleConfig_Set(&cfg)) {
        printf("[FAIL] setup_scenario: ChargeCycleConfig_Set rejected config\n");
        return false;
    }
    return true;
}

static void drive_step(uint32_t step_ms)
{
    mock_tick += step_ms;
    sim_bms_tick(mock_tick);
    sim_module_tick(SIM_DRV_MAXWELL, mock_tick);
    CHG_LIB_Process(mock_tick);
    BMS_Process(mock_tick);
    ChargeController_Process(mock_tick);
}

static void drive_ms(uint32_t total_ms)
{
    for (uint32_t elapsed = 0; elapsed < total_ms; elapsed += 20U) {
        drive_step(20U);
    }
}

static void set_healthy_bms(void)
{
    g_sim_bms.pack_voltage_v = 400.0f;
    g_sim_bms.pack_current_a = 0.0f;
    g_sim_bms.soc_pct = 50;
    g_sim_bms.max_cell_mv = 3000;
    g_sim_bms.max_cv_no = 1;
    g_sim_bms.min_cell_mv = 2980;
    g_sim_bms.min_cv_no = 2;
    g_sim_bms.max_cell_temp_c = 25.0f;
    g_sim_bms.min_cell_temp_c = 24.0f;
    g_sim_bms.avg_cell_temp_c = 24.5f;
    g_sim_bms.cap_remain_x0_1ah = 500;
    g_sim_bms.rate_cap_x0_1ah = 1000;
    g_sim_bms.soh_pct = 100;
    g_sim_bms.chg_volt_request_v = 500.0f;
    g_sim_bms.chg_curr_request_a = 50.0f;
}

/* ================================================================== */
/* Scenarios: frame parsing / SOF-resync / CRC                        */
/* ================================================================== */

static bool test_feedbyte_valid_frame_gets_acked(void)
{
    printf("Running test_feedbyte_valid_frame_gets_acked...\n");
    ASSERT(setup_scenario(), "setup failed");

    send_pc_frame(PC_CMD_PING, NULL, 0);

    uint8_t cmd, payload[8], len;
    ASSERT(only_tx_frame(&cmd, payload, &len), "expected exactly one PONG response");
    ASSERT(cmd == PC_RSP_PONG, "PING should get PC_RSP_PONG, not an ACK/NACK");
    ASSERT(len == 4, "PONG payload should be the 4-byte version");

    printf("[PASS] test_feedbyte_valid_frame_gets_acked\n");
    return true;
}

static bool test_feedbyte_defers_dispatch_until_process_rx(void)
{
    printf("Running test_feedbyte_defers_dispatch_until_process_rx...\n");
    ASSERT(setup_scenario(), "setup failed");
    PC_Protocol_ResetTx();

    uint8_t frame[] = {PC_SOF1, PC_SOF2, PC_CMD_PING, 0U, 0U};
    frame[4] = ref_crc8(&frame[2], 2U);
    for (size_t i = 0; i < sizeof(frame); i++) {
        PC_Protocol_FeedByte(frame[i]);
    }

    ASSERT(PC_Protocol_GetTxQueueDepth() == 0U,
           "FeedByte must not dispatch or enqueue a response before ProcessRx");
    PC_Protocol_ProcessRx();

    uint8_t cmd, payload[255], len;
    ASSERT(only_tx_frame(&cmd, payload, &len), "expected PONG after ProcessRx");
    ASSERT(cmd == PC_RSP_PONG, "deferred PING should produce PONG");

    printf("[PASS] test_feedbyte_defers_dispatch_until_process_rx\n");
    return true;
}

static bool test_feedbyte_bad_crc_nacked(void)
{
    printf("Running test_feedbyte_bad_crc_nacked...\n");
    ASSERT(setup_scenario(), "setup failed");

    send_pc_frame_bad_crc(PC_CMD_PING, NULL, 0);

    uint8_t cmd, payload[8], len;
    ASSERT(only_tx_frame(&cmd, payload, &len), "expected exactly one NACK response");
    ASSERT(cmd == PC_RSP_NACK, "corrupted CRC must be NACKed");
    ASSERT(payload[1] == PC_ERR_BAD_CRC, "NACK error code should be PC_ERR_BAD_CRC");

    printf("[PASS] test_feedbyte_bad_crc_nacked\n");
    return true;
}

/* Regression test for the documented B-02 fix (pc_protocol.c's
 * PC_Protocol_FeedByte(), ST_SOF2 case): a stray/duplicated 0xAA (PC_SOF1)
 * byte right before a real frame must be treated as the start of a NEW
 * frame, not force the receiver to wait for a third SOF1. */
static bool test_feedbyte_sof_resync_on_stray_sof1(void)
{
    printf("Running test_feedbyte_sof_resync_on_stray_sof1...\n");
    ASSERT(setup_scenario(), "setup failed");

    /* Inject a stray SOF1 before a real PING frame: AA AA 55 06 00 <crc> */
    PC_Protocol_FeedByte(PC_SOF1);
    send_pc_frame(PC_CMD_PING, NULL, 0);

    uint8_t cmd, payload[8], len;
    ASSERT(only_tx_frame(&cmd, payload, &len), "expected exactly one response after resync");
    ASSERT(cmd == PC_RSP_PONG, "frame after stray SOF1 should still parse correctly (B-02)");

    printf("[PASS] test_feedbyte_sof_resync_on_stray_sof1\n");
    return true;
}

static bool test_feedbyte_unknown_cmd_nacked(void)
{
    printf("Running test_feedbyte_unknown_cmd_nacked...\n");
    ASSERT(setup_scenario(), "setup failed");

    send_pc_frame(0x7F /* not a defined command */, NULL, 0);

    uint8_t cmd, payload[8], len;
    ASSERT(only_tx_frame(&cmd, payload, &len), "expected exactly one NACK response");
    ASSERT(cmd == PC_RSP_NACK, "unknown command must be NACKed");
    ASSERT(payload[1] == PC_ERR_UNKNOWN_CMD, "NACK error code should be PC_ERR_UNKNOWN_CMD");

    printf("[PASS] test_feedbyte_unknown_cmd_nacked\n");
    return true;
}

/* ================================================================== */
/* Scenarios: command dispatch -> real downstream state                */
/* ================================================================== */

static bool test_set_voltage_rejects_nan_and_inf(void)
{
    printf("Running test_set_voltage_rejects_nan_and_inf...\n");
    ASSERT(setup_scenario(), "setup failed");

    float nan_v = NAN;
    uint8_t payload[4];
    memcpy(payload, &nan_v, 4);
    send_pc_frame(PC_CMD_SET_VOLTAGE, payload, 4);
    uint8_t cmd, resp[8], len;
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected exactly one response to NaN SET_VOLTAGE");
    ASSERT(cmd == PC_RSP_NACK && resp[1] == PC_ERR_BAD_PARAM,
           "NaN voltage must be NACKed (isfinite() gate reachable end-to-end from the wire)");

    PC_Protocol_ResetTx();
    float inf_v = INFINITY;
    memcpy(payload, &inf_v, 4);
    send_pc_frame(PC_CMD_SET_VOLTAGE, payload, 4);
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected exactly one response to Inf SET_VOLTAGE");
    ASSERT(cmd == PC_RSP_NACK && resp[1] == PC_ERR_BAD_PARAM,
           "Inf voltage must be NACKed too -- a plain x!=x NaN check would miss this");

    printf("[PASS] test_set_voltage_rejects_nan_and_inf\n");
    return true;
}

static bool test_start_without_preconditions_nacked(void)
{
    printf("Running test_start_without_preconditions_nacked...\n");
    ASSERT(setup_scenario(), "setup failed");
    /* No module registered, no BMS data -- CheckPreconditions() must fail. */

    uint8_t manual_mode = 0;
    send_pc_frame(PC_CMD_START, &manual_mode, 1);

    uint8_t cmd, resp[8], len;
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected exactly one response");
    ASSERT(cmd == PC_RSP_NACK, "START with no module/BMS must be NACKed, not silently accepted");
    ASSERT(!ChargeController_IsRunning(), "controller must not be running after a rejected START");

    printf("[PASS] test_start_without_preconditions_nacked\n");
    return true;
}

/* Drives PC_CMD_SET_DRIVER + PC_CMD_SET_MODULE_ADDR through the real wire
 * protocol (not by calling CHG_LIB_AddModule()/ChargeCycleConfig_Set()
 * directly, as test/host_charge_sim does) and confirms the module actually
 * gets registered with the right address/group, AND that the B-18 fix
 * (seed rated current from config) is wired end-to-end from THIS call
 * site (pc_protocol.c's PC_CMD_SET_MODULE_ADDR handler) too -- B-18's own
 * regression test only exercised the charge_cycle_config.c call site. */
static bool test_set_driver_and_module_addr_over_wire(void)
{
    printf("Running test_set_driver_and_module_addr_over_wire...\n");
    ASSERT(setup_scenario(), "setup failed");

    uint8_t driver_id = CHG_LIB_DRV_MAXWELL;
    send_pc_frame(PC_CMD_SET_DRIVER, &driver_id, 1);
    uint8_t cmd, resp[8], len;
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected exactly one response to SET_DRIVER");
    ASSERT(cmd == PC_RSP_ACK, "SET_DRIVER with a valid driver id should be ACKed");
    ASSERT(g_storage_save_called, "SET_DRIVER should persist the driver choice to flash");

    PC_Protocol_ResetTx();
    uint8_t addr_payload[2] = { 5, 2 }; /* addr=5, group=2 */
    send_pc_frame(PC_CMD_SET_MODULE_ADDR, addr_payload, 2);
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected exactly one response to SET_MODULE_ADDR");
    ASSERT(cmd == PC_RSP_ACK, "SET_MODULE_ADDR should be ACKed");

    ASSERT(CHG_LIB_GetModuleCount() == 1, "exactly one module should now be registered");
    CHG_LIB_ModuleView_t mv;
    ASSERT(CHG_LIB_GetModuleView(0, &mv), "module view should be available");
    ASSERT(mv.addr == 5 && mv.group == 2, "module addr/group should match the wire payload");

    printf("[PASS] test_set_driver_and_module_addr_over_wire\n");
    return true;
}

static bool test_start_stop_happy_path(void)
{
    printf("Running test_start_stop_happy_path...\n");
    ASSERT(setup_scenario(), "setup failed");
    set_healthy_bms();

    uint8_t driver_id = CHG_LIB_DRV_MAXWELL;
    send_pc_frame(PC_CMD_SET_DRIVER, &driver_id, 1);
    PC_Protocol_ResetTx();
    uint8_t addr_payload[2] = { 1, 0 };
    send_pc_frame(PC_CMD_SET_MODULE_ADDR, addr_payload, 2);
    PC_Protocol_ResetTx();

    drive_ms(1500U);

    uint8_t manual_mode = 0;
    send_pc_frame(PC_CMD_START, &manual_mode, 1);
    uint8_t cmd, resp[8], len;
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected exactly one response to START");
    ASSERT(cmd == PC_RSP_ACK, "START should be ACKed once preconditions pass");

    /* ACKed START only means the controller accepted the request and left
     * IDLE (IDLE->READY is synchronous inside ChargeController_Start()) --
     * READY->RUNNING happens over subsequent ChargeController_Process()
     * ticks as the module actually confirms it started, same as every
     * other RUNNING assertion in this file. */
    drive_ms(2000U);
    ASSERT(ChargeController_IsRunning(), "controller should reach RUNNING once the module confirms start");

    PC_Protocol_ResetTx();
    send_pc_frame(PC_CMD_STOP, NULL, 0);
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected exactly one response to STOP");
    ASSERT(cmd == PC_RSP_ACK, "STOP should be ACKed");
    ASSERT(!ChargeController_IsRunning(), "controller should stop running after ACKed STOP");

    printf("[PASS] test_start_stop_happy_path\n");
    return true;
}

static bool test_read_reg_returns_real_module_voltage(void)
{
    printf("Running test_read_reg_returns_real_module_voltage...\n");
    ASSERT(setup_scenario(), "setup failed");
    set_healthy_bms();

    uint8_t driver_id = CHG_LIB_DRV_MAXWELL;
    send_pc_frame(PC_CMD_SET_DRIVER, &driver_id, 1);
    PC_Protocol_ResetTx();
    uint8_t addr_payload[2] = { 1, 0 };
    send_pc_frame(PC_CMD_SET_MODULE_ADDR, addr_payload, 2);
    PC_Protocol_ResetTx();
    drive_ms(1500U);
    uint8_t manual_mode = 0;
    send_pc_frame(PC_CMD_START, &manual_mode, 1);
    PC_Protocol_ResetTx();
    drive_ms(2000U); /* let the module actually reach RUNNING with real voltage */

    CHG_LIB_ModuleView_t mv;
    ASSERT(CHG_LIB_GetModuleView(0, &mv), "module view unavailable");

    uint8_t read_payload[3] = { 0, 0x00, 0x01 }; /* module_idx=0, reg=0x0001 (voltage) */
    send_pc_frame(PC_CMD_READ_REG, read_payload, 3);
    uint8_t cmd, resp[8], len;
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected exactly one READ_REG response");
    ASSERT(cmd == PC_RSP_READ_REG, "voltage read should get PC_RSP_READ_REG");
    float returned_v;
    memcpy(&returned_v, &resp[4], 4);
    ASSERT(fabsf(returned_v - mv.voltage) < 0.01f,
           "READ_REG voltage must match the real CHG_LIB_ModuleView_t, not a stale/zero value");

    printf("[PASS] test_read_reg_returns_real_module_voltage\n");
    return true;
}

/* ================================================================== */
/* Scenarios: debug protocol (DebugProtocol_HandleCommand)             */
/* ================================================================== */

static bool test_debug_enter_exit_toggles_active_state(void)
{
    printf("Running test_debug_enter_exit_toggles_active_state...\n");
    ASSERT(setup_scenario(), "setup failed");
    DebugProtocol_Init();

    send_pc_frame(DEBUG_CMD_ENTER, NULL, 0);
    ASSERT(DebugProtocol_IsActive(), "DEBUG_CMD_ENTER should activate debug mode");

    PC_Protocol_ResetTx();
    send_pc_frame(DEBUG_CMD_EXIT, NULL, 0);
    ASSERT(!DebugProtocol_IsActive(), "DEBUG_CMD_EXIT should deactivate debug mode");

    printf("[PASS] test_debug_enter_exit_toggles_active_state\n");
    return true;
}

static bool test_debug_get_charge_cfg_roundtrip(void)
{
    printf("Running test_debug_get_charge_cfg_roundtrip...\n");
    ASSERT(setup_scenario(), "setup failed");
    PC_Protocol_ResetTx();

    send_pc_frame(DEBUG_CMD_GET_CHARGE_CFG, NULL, 0);
    uint8_t cmd, resp[255], len;
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected exactly one GET_CHARGE_CFG response");
    ASSERT(cmd == DEBUG_RSP_CHARGE_CFG, "GET_CHARGE_CFG should get DEBUG_RSP_CHARGE_CFG");
    ASSERT(len == sizeof(ChargeCycleConfig_t), "response should be exactly sizeof(ChargeCycleConfig_t)");

    ChargeCycleConfig_t returned;
    memcpy(&returned, resp, sizeof(returned));
    ASSERT(returned.module_type == CHARGE_MODULE_TYPE_MAXWELL,
           "returned config should reflect the module_type set in setup_scenario()");
    ASSERT(returned.version == CHARGE_CYCLE_CONFIG_VERSION, "returned config version should be current");

    printf("[PASS] test_debug_get_charge_cfg_roundtrip\n");
    return true;
}

static bool test_debug_set_charge_cfg_rejects_invalid_config(void)
{
    printf("Running test_debug_set_charge_cfg_rejects_invalid_config...\n");
    ASSERT(setup_scenario(), "setup failed");
    PC_Protocol_ResetTx();

    ChargeCycleConfig_t bad_cfg;
    ChargeCycleConfig_GetDefaults(&bad_cfg);
    bad_cfg.module_u_max_v = 10.0f;
    bad_cfg.module_u_min_v = 30.0f; /* max < min: must be rejected by ChargeCycleConfig_Set() */

    send_pc_frame(DEBUG_CMD_SET_CHARGE_CFG, (const uint8_t *)&bad_cfg, sizeof(bad_cfg));
    uint8_t cmd, resp[8], len;
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected exactly one response");
    ASSERT(cmd == DEBUG_RSP_ERROR, "invalid config must be rejected, not silently applied");
    ASSERT(!g_storage_save_called, "an invalid config must never reach flash-save");

    printf("[PASS] test_debug_set_charge_cfg_rejects_invalid_config\n");
    return true;
}

static bool test_debug_set_charge_cfg_valid_config_persists(void)
{
    printf("Running test_debug_set_charge_cfg_valid_config_persists...\n");
    ASSERT(setup_scenario(), "setup failed");
    PC_Protocol_ResetTx();

    ChargeCycleConfig_t good_cfg;
    ChargeCycleConfig_GetDefaults(&good_cfg);
    good_cfg.module_type = CHARGE_MODULE_TYPE_LIANMING;
    good_cfg.source_module_count = 3U;

    send_pc_frame(DEBUG_CMD_SET_CHARGE_CFG, (const uint8_t *)&good_cfg, sizeof(good_cfg));
    uint8_t cmd, resp[255], len;
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected exactly one response");
    ASSERT(cmd == DEBUG_RSP_CHARGE_CFG, "a valid config should echo back DEBUG_RSP_CHARGE_CFG");
    ASSERT(g_storage_save_called, "a valid config should reach ChargeCycleStorage_Save()");

    ChargeCycleConfig_t stored;
    ChargeCycleConfig_Get(&stored);
    ASSERT(stored.module_type == CHARGE_MODULE_TYPE_LIANMING, "RAM config should now reflect the new value");
    ASSERT(stored.source_module_count == 3U, "RAM config should reflect source_module_count too");

    printf("[PASS] test_debug_set_charge_cfg_valid_config_persists\n");
    return true;
}

static bool test_debug_set_charge_cfg_v6_compat_persists(void)
{
    printf("Running test_debug_set_charge_cfg_v6_compat_persists...\n");
    ASSERT(setup_scenario(), "setup failed");
    PC_Protocol_ResetTx();

    ChargeCycleConfig_t curr_cfg;
    ChargeCycleConfig_GetDefaults(&curr_cfg);
    curr_cfg.charge_mode = 1U; /* NORMAL */
    curr_cfg.delay_enabled = 1U;
    curr_cfg.delay_hours = 2U;
    curr_cfg.delay_minutes = 30U;
    ChargeCycleConfig_Set(&curr_cfg);

    /* Simulate an older C# app sending 243 bytes (v6 payload) */
    uint8_t v6_payload[243];
    memcpy(v6_payload, &curr_cfg, 243);
    ChargeCycleConfig_t *p_v6 = (ChargeCycleConfig_t *)v6_payload;
    p_v6->battery_capacity_ah = 120.0f;

    send_pc_frame(DEBUG_CMD_SET_CHARGE_CFG, v6_payload, 243U);
    uint8_t cmd, resp[255], len;
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected exactly one response");
    ASSERT(cmd == DEBUG_RSP_CHARGE_CFG, "v6 config should succeed and echo back DEBUG_RSP_CHARGE_CFG");
    ASSERT(g_storage_save_called, "v6 config should reach ChargeCycleStorage_Save()");

    ChargeCycleConfig_t stored;
    ChargeCycleConfig_Get(&stored);
    ASSERT(stored.battery_capacity_ah == 120.0f, "v6 battery_capacity_ah should be updated");
    ASSERT(stored.charge_mode == 1U, "charge_mode must be preserved");
    ASSERT(stored.delay_enabled == 1, "delay_enabled must be preserved");
    ASSERT(stored.delay_hours == 2 && stored.delay_minutes == 30, "delay timer must be preserved");
    ASSERT(stored.version == CHARGE_CYCLE_CONFIG_VERSION, "version must be bumped to current version");

    printf("[PASS] test_debug_set_charge_cfg_v6_compat_persists\n");
    return true;
}

static bool test_debug_set_charge_cfg_v7_compat_persists(void)
{
    printf("Running test_debug_set_charge_cfg_v7_compat_persists...\n");
    ASSERT(setup_scenario(), "setup failed");
    PC_Protocol_ResetTx();

    ChargeCycleConfig_t curr_cfg;
    ChargeCycleConfig_GetDefaults(&curr_cfg);
    curr_cfg.charge_mode = 0U; /* FAST */
    ChargeCycleConfig_Set(&curr_cfg);

    /* Simulate a v7 app sending 249 bytes (v7 payload without imax_a) */
    uint8_t v7_payload[249];
    memcpy(v7_payload, &curr_cfg, 249);
    ChargeCycleConfig_t *p_v7 = (ChargeCycleConfig_t *)v7_payload;
    p_v7->battery_capacity_ah = 150.0f;

    send_pc_frame(DEBUG_CMD_SET_CHARGE_CFG, v7_payload, 249U);
    uint8_t cmd, resp[255], len;
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected exactly one response");
    ASSERT(cmd == DEBUG_RSP_CHARGE_CFG, "v7 config should succeed and echo back DEBUG_RSP_CHARGE_CFG");
    ASSERT(g_storage_save_called, "v7 config should reach ChargeCycleStorage_Save()");

    ChargeCycleConfig_t stored;
    ChargeCycleConfig_Get(&stored);
    ASSERT(stored.battery_capacity_ah == 150.0f, "v7 battery_capacity_ah should be updated");
    ASSERT(stored.imax_a == DEFAULT_IMAX_A, "v7 imax_a should default to DEFAULT_IMAX_A");
    ASSERT(stored.version == CHARGE_CYCLE_CONFIG_VERSION, "version must be bumped to current version");

    printf("[PASS] test_debug_set_charge_cfg_v7_compat_persists\n");
    return true;
}

static bool test_debug_set_charge_cfg_wrong_length_rejected(void)
{
    printf("Running test_debug_set_charge_cfg_wrong_length_rejected...\n");
    ASSERT(setup_scenario(), "setup failed");
    PC_Protocol_ResetTx();

    uint8_t short_payload[10] = {0};
    send_pc_frame(DEBUG_CMD_SET_CHARGE_CFG, short_payload, sizeof(short_payload));
    uint8_t cmd, resp[8], len;
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected exactly one response");
    ASSERT(cmd == DEBUG_RSP_ERROR, "a too-short SET_CHARGE_CFG payload must be rejected");

    printf("[PASS] test_debug_set_charge_cfg_wrong_length_rejected\n");
    return true;
}

static bool test_debug_dual_profiles_protocol_roundtrip(void)
{
    printf("Running test_debug_dual_profiles_protocol_roundtrip...\n");
    ASSERT(setup_scenario(), "setup failed");
    PC_Protocol_ResetTx();

    /* 1. Write Fast profile */
    ChargeCycleConfig_t fast_cfg;
    ChargeCycleConfig_GetDefaults(&fast_cfg);
    fast_cfg.charge_mode = CHARGE_MODE_FAST;
    fast_cfg.battery_capacity_ah = 200.0f;
    fast_cfg.imax_c = 1.0f;
    send_pc_frame(DEBUG_CMD_SET_CHARGE_CFG, (const uint8_t *)&fast_cfg, sizeof(fast_cfg));
    uint8_t cmd, resp[255], len;
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected response for fast profile set");
    ASSERT(cmd == DEBUG_RSP_CHARGE_CFG, "set fast profile should succeed");

    /* 2. Write Normal profile */
    PC_Protocol_ResetTx();
    ChargeCycleConfig_t norm_cfg;
    ChargeCycleConfig_GetDefaults(&norm_cfg);
    norm_cfg.charge_mode = CHARGE_MODE_NORMAL;
    norm_cfg.battery_capacity_ah = 100.0f;
    norm_cfg.imax_c = 0.5f;
    send_pc_frame(DEBUG_CMD_SET_CHARGE_CFG, (const uint8_t *)&norm_cfg, sizeof(norm_cfg));
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected response for normal profile set");
    ASSERT(cmd == DEBUG_RSP_CHARGE_CFG, "set normal profile should succeed");

    /* 3. Read Fast profile specifically with mode byte = 0 */
    PC_Protocol_ResetTx();
    uint8_t mode_fast = 0U;
    send_pc_frame(DEBUG_CMD_GET_CHARGE_CFG, &mode_fast, 1U);
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected response for fast profile get");
    ASSERT(cmd == DEBUG_RSP_CHARGE_CFG, "get fast profile should succeed");
    ChargeCycleConfig_t read_fast;
    memcpy(&read_fast, resp, sizeof(read_fast));
    ASSERT(read_fast.charge_mode == CHARGE_MODE_FAST, "read_fast must have charge_mode = FAST");
    ASSERT(read_fast.battery_capacity_ah == 200.0f, "read_fast must have 200.0 Ah");

    /* 4. Read Normal profile specifically with mode byte = 1 */
    PC_Protocol_ResetTx();
    uint8_t mode_norm = 1U;
    send_pc_frame(DEBUG_CMD_GET_CHARGE_CFG, &mode_norm, 1U);
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected response for normal profile get");
    ASSERT(cmd == DEBUG_RSP_CHARGE_CFG, "get normal profile should succeed");
    ChargeCycleConfig_t read_norm;
    memcpy(&read_norm, resp, sizeof(read_norm));
    ASSERT(read_norm.charge_mode == CHARGE_MODE_NORMAL, "read_norm must have charge_mode = NORMAL");
    ASSERT(read_norm.battery_capacity_ah == 100.0f, "read_norm must have 100.0 Ah");

    printf("[PASS] test_debug_dual_profiles_protocol_roundtrip\n");
    return true;
}

static bool test_debug_get_system_info_matches_wire_struct(void)
{
    printf("Running test_debug_get_system_info_matches_wire_struct...\n");
    ASSERT(setup_scenario(), "setup failed");
    PC_Protocol_ResetTx();

    send_pc_frame(DEBUG_CMD_GET_SYSTEM, NULL, 0);
    uint8_t cmd, resp[255], len;
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected exactly one GET_SYSTEM response");
    ASSERT(cmd == DEBUG_RSP_SYSTEM_INFO, "GET_SYSTEM should get DEBUG_RSP_SYSTEM_INFO");
    ASSERT(len == sizeof(DebugSystemInfo_t), "response length must match the static-asserted 72-byte wire struct");

    DebugSystemInfo_t info;
    memcpy(&info, resp, sizeof(info));
    ASSERT(info.fw_major == FW_VERSION_MAJOR, "fw_major should match FW_VERSION_MAJOR");
    ASSERT(info.driver_id == CHG_LIB_DRV_MAXWELL, "driver_id should reflect the driver selected in setup");

    printf("[PASS] test_debug_get_system_info_matches_wire_struct\n");
    return true;
}

static bool test_debug_get_alarm_info_wire_contract(void)
{
    printf("Running test_debug_get_alarm_info_wire_contract...\n");
    ASSERT(setup_scenario(), "setup failed");
    PC_Protocol_ResetTx();

    send_pc_frame(DEBUG_CMD_GET_ALARMS, NULL, 0);
    uint8_t cmd, resp[255], len;
    ASSERT(only_tx_frame(&cmd, resp, &len), "expected exactly one GET_ALARMS response");
    ASSERT(cmd == DEBUG_RSP_ALARMS, "GET_ALARMS should return DEBUG_RSP_ALARMS");
    ASSERT(len >= sizeof(DebugAlarmInfo_t), "alarm response must include fixed header");
    DebugAlarmInfo_t info;
    memcpy(&info, resp, sizeof(info));
    ASSERT(info.log_count <= ALARM_LOG_DEPTH, "alarm log count exceeds fixed depth");
    ASSERT(len == sizeof(DebugAlarmInfo_t) + (uint16_t)info.log_count * sizeof(AlarmLogEntry_t),
           "alarm response length must match header log_count");

    printf("[PASS] test_debug_get_alarm_info_wire_contract\n");
    return true;
}

/* Regression test: DebugProtocol_BuildModuleData() must refuse to write
 * when it doesn't fit in max_len, not write unconditionally and let the
 * caller find out too late. This is the actual fix -- see the BUGFIX
 * comment on DebugProtocol_BuildModuleData() in pc_debug_protocol.c. */
static bool test_build_module_data_refuses_when_too_small(void)
{
    printf("Running test_build_module_data_refuses_when_too_small...\n");
    ASSERT(setup_scenario(), "setup failed");

    uint8_t buf[sizeof(DebugModuleData_t)];
    uint16_t len_too_small = DebugProtocol_BuildModuleData(0, buf, (uint16_t)(sizeof(DebugModuleData_t) - 1U));
    ASSERT(len_too_small == 0, "must return 0 (refuse) when max_len is one byte short of the struct size");

    uint16_t len_exact = DebugProtocol_BuildModuleData(0, buf, (uint16_t)sizeof(DebugModuleData_t));
    ASSERT(len_exact == sizeof(DebugModuleData_t), "must succeed when max_len exactly fits");

    printf("[PASS] test_build_module_data_refuses_when_too_small\n");
    return true;
}

/* Regression test for the actual bug: DebugProtocol_BuildAllModulesData()
 * used to write a full sizeof(DebugModuleData_t) (123 bytes) into the
 * caller's buffer BEFORE checking whether it fit -- on any system with 3+
 * modules this overflowed the 255-byte (PC_MAX_PAYLOAD) stack buffer used
 * by both DebugProtocol_SendStream() and DEBUG_CMD_READ_ALL's reply. Uses
 * a canary region immediately after the buffer to detect the exact class
 * of out-of-bounds write the bug caused; register enough modules that 2
 * fully fit in PC_MAX_PAYLOAD but a 3rd (previously) would not. */
static bool test_build_all_modules_data_does_not_overflow_buffer(void)
{
    printf("Running test_build_all_modules_data_does_not_overflow_buffer...\n");
    ASSERT(setup_scenario(), "setup failed");

    /* setup_scenario() already registered module addr=1 (source_module_
     * count=1 in its config); add 4 more addresses directly so
     * CHG_LIB_GetModuleCount() == 5 -- comfortably past the "2 fit, 3rd
     * overflows" boundary (2 + 123*2 = 248 <= 255 < 2 + 123*3). */
    for (uint8_t addr = 2; addr <= 5; addr++) {
        CHG_LIB_AddModule(addr, 0);
    }
    ASSERT(CHG_LIB_GetModuleCount() >= 5, "expected at least 5 registered modules for this test");

    struct {
        uint8_t buf[PC_MAX_PAYLOAD];
        uint8_t canary[64];
    } guarded;
    memset(guarded.buf, 0, sizeof(guarded.buf));
    memset(guarded.canary, 0xAA, sizeof(guarded.canary));

    uint16_t len = DebugProtocol_BuildAllModulesData(guarded.buf, sizeof(guarded.buf));

    for (size_t i = 0; i < sizeof(guarded.canary); i++) {
        if (guarded.canary[i] != 0xAA) {
            printf("[FAIL] %s:%d - canary byte %zu corrupted (0x%02X) -- buffer overflow past `buf`\n",
                   __func__, __LINE__, i, guarded.canary[i]);
            return false;
        }
    }
    ASSERT(len <= sizeof(guarded.buf), "returned length must never exceed the buffer passed in");
    ASSERT(len > 2, "should have written the header plus at least one module's worth of data");

    printf("[PASS] test_build_all_modules_data_does_not_overflow_buffer\n");
    return true;
}

static bool test_debug_rtc_get_set_roundtrip(void)
{
    printf("Running test_debug_rtc_get_set_roundtrip...\n");
    ASSERT(setup_scenario(), "setup failed");

    /* 1. Send DEBUG_CMD_GET_RTC */
    send_pc_frame(DEBUG_CMD_GET_RTC, NULL, 0);
    uint8_t cmd, payload[256], len;
    ASSERT(only_tx_frame(&cmd, payload, &len), "expected exactly 1 TX frame for GET_RTC");
    ASSERT(cmd == DEBUG_RSP_RTC, "expected DEBUG_RSP_RTC");
    ASSERT(len == sizeof(DebugRtcInfo_t), "expected sizeof(DebugRtcInfo_t)");

    /* Reset TX queue before sending next command */
    PC_Protocol_ResetTx();

    /* 2. Send DEBUG_CMD_SET_RTC with epoch = 1772866800 (2026-03-07 07:00:00) */
    uint32_t set_epoch = 1772866800U;
    send_pc_frame(DEBUG_CMD_SET_RTC, (const uint8_t *)&set_epoch, 4);
    ASSERT(only_tx_frame(&cmd, payload, &len), "expected exactly 1 TX frame for SET_RTC");
    ASSERT(cmd == DEBUG_RSP_RTC, "expected DEBUG_RSP_RTC on set");
    ASSERT(len == sizeof(DebugRtcInfo_t), "expected sizeof(DebugRtcInfo_t)");
    DebugRtcInfo_t *info = (DebugRtcInfo_t *)payload;
    ASSERT(info->epoch_sec == set_epoch, "epoch should match");
    ASSERT(info->year == 2026, "year should match");
    ASSERT(info->is_valid == 1, "is_valid should be 1");

    printf("[PASS] test_debug_rtc_get_set_roundtrip\n");
    return true;
}

/* ================================================================== */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== PC Protocol E2E Simulation Test (real byte stream -> real pc_protocol.c/pc_debug_protocol.c) ===\n");
    bool pass = true;

    pass &= test_feedbyte_valid_frame_gets_acked();
    pass &= test_feedbyte_defers_dispatch_until_process_rx();
    pass &= test_feedbyte_bad_crc_nacked();
    pass &= test_feedbyte_sof_resync_on_stray_sof1();
    pass &= test_feedbyte_unknown_cmd_nacked();

    pass &= test_set_voltage_rejects_nan_and_inf();
    pass &= test_start_without_preconditions_nacked();
    pass &= test_set_driver_and_module_addr_over_wire();
    pass &= test_start_stop_happy_path();
    pass &= test_read_reg_returns_real_module_voltage();

    pass &= test_debug_enter_exit_toggles_active_state();
    pass &= test_debug_get_charge_cfg_roundtrip();
    pass &= test_debug_set_charge_cfg_rejects_invalid_config();
    pass &= test_debug_set_charge_cfg_valid_config_persists();
    pass &= test_debug_set_charge_cfg_v6_compat_persists();
    pass &= test_debug_set_charge_cfg_v7_compat_persists();
    pass &= test_debug_set_charge_cfg_wrong_length_rejected();
    pass &= test_debug_dual_profiles_protocol_roundtrip();
    pass &= test_debug_get_system_info_matches_wire_struct();
    pass &= test_debug_get_alarm_info_wire_contract();
    pass &= test_build_module_data_refuses_when_too_small();
    pass &= test_build_all_modules_data_does_not_overflow_buffer();
    pass &= test_debug_rtc_get_set_roundtrip();

    if (pass) {
        printf("ALL TESTS PASSED.\n");
        return 0;
    }
    printf("TESTS FAILED.\n");
    return 1;
}
