#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Mock HAL */
uint32_t mock_tick = 0;
uint32_t HAL_GetTick(void) { return mock_tick; }
uint32_t CHG_LIB_CanBackend_NowTick(void) { return mock_tick; }

/* Mock BSP (bsp_sys.h) -- bms_protocol.c calls BSP_GetTick() instead of
 * HAL_GetTick() directly (AGENTS.md sec 5-6: Modules must not include the
 * HAL directly, only Platform headers). No entry point in this test uses
 * BSP_EnterCritical/BSP_ExitCritical or BSP_Delay; stub them anyway so the
 * link succeeds if that ever changes. */
uint32_t BSP_GetTick(void) { return mock_tick; }
void BSP_Delay(uint32_t delay_ms) { (void)delay_ms; }
void BSP_EnterCritical(void) {}
void BSP_ExitCritical(void) {}

/* Includes to test */
#include "bms_protocol.h"
#include "priv/chg_lib_core_priv.h"

/* Assert Macro */
#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("[FAIL] %s:%d - %s\n", __func__, __LINE__, msg); \
            return false; \
        } \
    } while(0)

/* ========================================================================= */
/* Test BMS Parser (Component 1) */
/* ========================================================================= */
bool test_bms_parser(void) {
    printf("Running test_bms_parser...\n");
    BMS_Data_t bms;
    memset(&bms, 0, sizeof(bms));

    /* 1. Simulate BATT_ST1 (ID: 0x02F4) */
    /* raw_volt (2), raw_curr (2), soc (1), soh (1), alarm_status (2) */
    uint8_t batt_st1_data[8] = { 0xE8, 0x03, 0xD0, 0x07, 50, 100, 0x00, 0x00 }; 
    /* Volt: 0x03E8 = 1000 -> 100.0V */
    /* Curr: 0x07D0 = 2000 -> offset 400? Wait, the parser doesn't do offset, just raw */
    
    mock_tick = 100;
    bool parsed = BMS_ParseFrame(0, BMS_ID_BATT_ST1, batt_st1_data, 5, &bms);
    ASSERT(parsed == true, "Failed to parse BATT_ST1");
    ASSERT(bms.batt_st1.raw_volt == 1000, "Volt mismatch");
    ASSERT(bms.last_rx_tick[BMS_FRAME_BATT_ST1] == 100, "Tick not updated for BATT_ST1");

    /* 2. Simulate CELL_VOLT (ID: 0x04F4) */
    uint8_t cell_volt_data[6] = { 0x50, 0x0C, 0x60, 0x0C, 0x10, 0x00 };
    mock_tick = 250;
    parsed = BMS_ParseFrame(0, BMS_ID_CELL_VOLT, cell_volt_data, 6, &bms);
    ASSERT(parsed == true, "Failed to parse CELL_VOLT");
    ASSERT(bms.last_rx_tick[BMS_FRAME_CELL_VOLT] == 250, "Tick not updated for CELL_VOLT");

    /* 3. Unknown ID and short known frame must be diagnosable and must not
     * refresh any BMS watchdog timestamp. */
    BMS_ParseRejectReason_t reason = BMS_PARSE_OK;
    BMS_FrameType_t frame_type = BMS_FRAME_MAX;
    mock_tick = 400;
    parsed = BMS_ParseFrameEx(0, 0x0123, batt_st1_data, 8, &bms,
                              &reason, &frame_type);
    ASSERT(parsed == false, "Unknown BMS ID must be rejected");
    ASSERT(reason == BMS_PARSE_REJECT_UNKNOWN_ID, "Unknown ID reason mismatch");
    ASSERT(frame_type == BMS_FRAME_MAX, "Rejected frame type must be empty");
    ASSERT(bms.last_rx_tick[BMS_FRAME_BATT_ST1] == 100,
           "Unknown ID must not refresh watchdog");

    mock_tick = 500;
    parsed = BMS_ParseFrameEx(0, BMS_ID_CELL_VOLT, cell_volt_data, 2, &bms,
                              &reason, &frame_type);
    ASSERT(parsed == false, "Short BMS frame must be rejected");
    ASSERT(reason == BMS_PARSE_REJECT_DLC, "Short frame reason mismatch");
    ASSERT(bms.last_rx_tick[BMS_FRAME_CELL_VOLT] == 250,
           "Short frame must not refresh watchdog");

    printf("[PASS] test_bms_parser\n");
    return true;
}

/* ========================================================================= */
/* Test FSM Timeout Logic (Component 2) */
/* ========================================================================= */
bool test_fsm_timeout(void) {
    printf("Running test_fsm_timeout...\n");
    
    CHG_LIB_State_t state = CHG_LIB_STATE_RUNNING;
    uint32_t state_tick = 0;
    uint32_t last_rx = 1000;
    bool should_run = true;
    CHG_LIB_State_t new_state;
    bool timeout_flag = false;

    /* Test 1: Active, within warning timeout */
    mock_tick = 1500;
    CHG_LIB_FSM_CheckOfflineTimeout(&state, last_rx, should_run, 5000, 2000, mock_tick, &new_state, &timeout_flag);
    ASSERT(new_state == CHG_LIB_STATE_RUNNING, "State should remain RUNNING");
    ASSERT(timeout_flag == false, "Timeout flag should be false");

    /* Test 2: Warning threshold crossed */
    mock_tick = 3500;
    CHG_LIB_FSM_CheckOfflineTimeout(&state, last_rx, should_run, 5000, 2000, mock_tick, &new_state, &timeout_flag);
    ASSERT(new_state == CHG_LIB_STATE_WARNING, "State should be WARNING");
    
    /* Test 3: Offline threshold crossed */
    mock_tick = 7000;
    CHG_LIB_FSM_CheckOfflineTimeout(&state, last_rx, should_run, 5000, 2000, mock_tick, &new_state, &timeout_flag);
    ASSERT(new_state == CHG_LIB_STATE_OFFLINE, "State should be OFFLINE");
    ASSERT(timeout_flag == true, "Timeout flag should be true");

    printf("[PASS] test_fsm_timeout\n");
    return true;
}

int main(void) {
    printf("=== Native Logic Test ===\n");
    bool pass = true;
    pass &= test_bms_parser();
    pass &= test_fsm_timeout();
    
    if (pass) {
        printf("ALL TESTS PASSED.\n");
        return 0;
    } else {
        printf("TESTS FAILED.\n");
        return 1;
    }
}
