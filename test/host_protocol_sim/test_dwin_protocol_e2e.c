/**
 * @file test_dwin_protocol_e2e.c
 * @brief Host-compiled end-to-end test for Modules/hmi/dwin_protocol.c.
 *        Compiles the real dwin_protocol.c, feeds real byte streams into the
 *        real DWIN_ParseRX(), and asserts on the real DWIN_OnActionButton()
 *        callback and the real DWIN_SendWords()/DWIN_SendString()/
 *        DWIN_SetPage()/DWIN_UpdateData() wire output.
 *
 * dwin_protocol.c has no STM32 HAL dependency -- it only needs
 * UART_Transmit_To_DWIN() (extern, stubbed below) and defines a weak
 * DWIN_OnActionButton() this file overrides.
 */
#include <stdio.h>
#include <string.h>

#include "dwin_protocol.h"

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("[FAIL] %s:%d - %s\n", __func__, __LINE__, msg); \
            return false; \
        } \
    } while (0)

/* ================================================================== */
/* UART_Transmit_To_DWIN stub -- records every frame so tests can       */
/* assert on real wire bytes and on how many frames a call emitted.     */
/* ================================================================== */

#define MAX_FRAMES 16
static uint8_t  g_tx[MAX_FRAMES][128];
static uint16_t g_tx_len[MAX_FRAMES];
static int      g_tx_count = 0;

void UART_Transmit_To_DWIN(uint8_t *data, uint16_t len)
{
    if (g_tx_count < MAX_FRAMES) {
        uint16_t n = (len < 128U) ? len : 128U;
        memcpy(g_tx[g_tx_count], data, n);
        g_tx_len[g_tx_count] = len;
    }
    g_tx_count++;
}

static uint16_t g_last_keyval = 0xFFFF;
static int      g_action_count = 0;
static uint16_t g_last_event_vp = 0xFFFF;
static uint16_t g_last_event_key = 0xFFFF;
static int      g_key_event_count = 0;

void DWIN_OnActionButton(uint16_t keyval)
{
    g_last_keyval = keyval;
    g_action_count++;
}

void DWIN_OnKeyEvent(uint16_t vp, uint16_t keyval)
{
    g_last_event_vp = vp;
    g_last_event_key = keyval;
    g_key_event_count++;
}

static void reset_capture(void)
{
    memset(g_tx, 0, sizeof(g_tx));
    memset(g_tx_len, 0, sizeof(g_tx_len));
    g_tx_count = 0;
    g_last_keyval = 0xFFFF;
    g_action_count = 0;
    g_last_event_vp = 0xFFFF;
    g_last_event_key = 0xFFFF;
    g_key_event_count = 0;
}

/* Build a real DGUS touch-upload frame:
 * 5A A5 06 83 <VP_hi> <VP_lo> 01 <val_hi> <val_lo>  (9 bytes). */
static void build_touch_frame(uint8_t *buf, uint16_t vp, uint16_t val)
{
    buf[0] = DWIN_HEADER_1;
    buf[1] = DWIN_HEADER_2;
    buf[2] = 0x06;
    buf[3] = DWIN_CMD_READ;
    buf[4] = (uint8_t)(vp >> 8);
    buf[5] = (uint8_t)(vp & 0xFF);
    buf[6] = 0x01;
    buf[7] = (uint8_t)(val >> 8);
    buf[8] = (uint8_t)(val & 0xFF);
}

/* ================================================================== */

static bool test_send_words_single(void)
{
    printf("Running test_send_words_single...\n");
    reset_capture();

    uint16_t v = 4805; /* 480.5 V x10 */
    DWIN_SendWords(VP_DC_VOLTAGE, &v, 1);

    ASSERT(g_tx_count == 1, "one word -> exactly one frame");
    ASSERT(g_tx_len[0] == 8, "frame should be 8 bytes");
    ASSERT(g_tx[0][0] == DWIN_HEADER_1 && g_tx[0][1] == DWIN_HEADER_2, "header");
    ASSERT(g_tx[0][2] == 0x05, "LEN = cmd + vp(2) + data(2) = 5");
    ASSERT(g_tx[0][3] == DWIN_CMD_WRITE, "write command byte");
    ASSERT((((uint16_t)g_tx[0][4] << 8) | g_tx[0][5]) == VP_DC_VOLTAGE, "VP round-trips");
    ASSERT((((uint16_t)g_tx[0][6] << 8) | g_tx[0][7]) == 4805, "value big-endian");

    printf("[PASS] test_send_words_single\n");
    return true;
}

static bool test_send_words_multi(void)
{
    printf("Running test_send_words_multi...\n");
    reset_capture();

    uint16_t w[3] = { 0x0102, 0x0304, 0x0506 };
    DWIN_SendWords(VP_AC_PHASE_L1, w, 3);

    ASSERT(g_tx_count == 1, "one frame");
    ASSERT(g_tx_len[0] == 12, "6 header + 6 data");
    ASSERT(g_tx[0][2] == 3 + 6, "LEN reflects 3 words");
    ASSERT(g_tx[0][6] == 0x01 && g_tx[0][7] == 0x02, "word 0 big-endian");
    ASSERT(g_tx[0][10] == 0x05 && g_tx[0][11] == 0x06, "word 2 big-endian");

    /* out-of-range word counts are dropped */
    reset_capture();
    DWIN_SendWords(VP_DC_VOLTAGE, w, 0);
    DWIN_SendWords(VP_DC_VOLTAGE, w, 99);
    ASSERT(g_tx_count == 0, "0 and oversized word counts emit nothing");

    printf("[PASS] test_send_words_multi\n");
    return true;
}

static bool test_send_string_pads_field(void)
{
    printf("Running test_send_string_pads_field...\n");
    reset_capture();

    DWIN_SendString(VP_SET_DEVICE_ID, "PKG-1", VP_SET_STR_WORDS); /* 8 words = 16 bytes */

    ASSERT(g_tx_count == 1, "one frame");
    ASSERT(g_tx_len[0] == 6 + 16, "whole 16-byte field written");
    ASSERT(g_tx[0][2] == 3 + 16, "LEN reflects the padded field");
    ASSERT(memcmp(&g_tx[0][6], "PKG-1", 5) == 0, "text copied");
    ASSERT(g_tx[0][6 + 5] == 0x00 && g_tx[0][6 + 15] == 0x00, "tail zero-padded");

    /* a string longer than the field is truncated, not overflowed */
    reset_capture();
    DWIN_SendString(VP_SET_HW_VER, "0123456789ABCDEF-OVERFLOW", VP_SET_STR_WORDS);
    ASSERT(g_tx_count == 1 && g_tx_len[0] == 6 + 16, "truncated to field width");
    ASSERT(memcmp(&g_tx[0][6], "0123456789ABCDEF", 16) == 0, "first 16 chars kept");

    reset_capture();
    DWIN_SendString(DWIN_SOC_TEXT_VP, "--%", DWIN_TEXT_8_BYTES_WORDS);
    ASSERT(g_tx_count == 1 && g_tx_len[0] == 14, "8-byte SOC text emits one 4-VP frame");
    ASSERT(g_tx[0][2] == 11, "SOC text LEN = 3 + 8 payload bytes");
    ASSERT(memcmp(&g_tx[0][6], "--%", 3) == 0, "unavailable SOC text with unit is transmitted");
    ASSERT(g_tx[0][9] == 0x00 && g_tx[0][13] == 0x00, "SOC text is zero-padded to 8 bytes");

    printf("[PASS] test_send_string_pads_field\n");
    return true;
}

static bool test_set_page_diff_suppressed(void)
{
    printf("Running test_set_page_diff_suppressed...\n");
    reset_capture();

    DWIN_SetPage(DWIN_PAGE_SETTING);
    ASSERT(g_tx_count == 1, "first page switch emits");
    ASSERT(g_tx_len[0] == 10, "5A A5 07 82 00 84 5A 01 00 02");
    ASSERT(g_tx[0][2] == 0x07, "LEN = cmd + vp(2) + 2 words = 7");
    ASSERT(g_tx[0][3] == DWIN_CMD_WRITE, "write");
    ASSERT((((uint16_t)g_tx[0][4] << 8) | g_tx[0][5]) == VP_SYS_PIC_SET, "VP 0x0084");
    ASSERT(g_tx[0][6] == 0x5A && g_tx[0][7] == 0x01, "arm word 0x5A01");
    ASSERT((((uint16_t)g_tx[0][8] << 8) | g_tx[0][9]) == DWIN_PAGE_SETTING, "page id");

    DWIN_SetPage(DWIN_PAGE_SETTING);
    ASSERT(g_tx_count == 1, "repeat page switch is suppressed");

    DWIN_SetPage(DWIN_PAGE_DASH);
    ASSERT(g_tx_count == 2, "a different page emits again");

    printf("[PASS] test_set_page_diff_suppressed\n");
    return true;
}

static bool test_software_reset_and_sync_invalidation(void)
{
    printf("Running test_software_reset_and_sync_invalidation...\n");
    reset_capture();

    DWIN_InvalidateSyncState();
    DWIN_SetPage(DWIN_PAGE_SETTING);
    ASSERT(g_tx_count == 1, "invalidated page cache must emit unchanged page");

    reset_capture();
    DWIN_SendSoftwareReset();
    ASSERT(g_tx_count == 1, "software reset emits one frame");
    ASSERT(g_tx_len[0] == 10, "software reset frame length");
    ASSERT(g_tx[0][0] == DWIN_HEADER_1 && g_tx[0][1] == DWIN_HEADER_2,
           "software reset uses project header");
    ASSERT(g_tx[0][2] == 0x07 && g_tx[0][3] == DWIN_CMD_WRITE,
           "software reset command and length");
    ASSERT(g_tx[0][4] == 0x00 && g_tx[0][5] == 0x04,
           "software reset targets system VP 0x0004");
    ASSERT(g_tx[0][6] == 0x55 && g_tx[0][7] == 0xAA &&
           g_tx[0][8] == 0x5A && g_tx[0][9] == 0xA5,
           "software reset payload is 55 AA 5A A5");

    printf("[PASS] test_software_reset_and_sync_invalidation\n");
    return true;
}

static bool test_parse_rx_dispatches(void)
{
    printf("Running test_parse_rx_dispatches...\n");
    reset_capture();

    uint8_t f[9];
    build_touch_frame(f, VP_SYS_BTN_KEY, 1);
    DWIN_ParseRX(f, sizeof(f));

    ASSERT(g_action_count == 1, "one valid touch frame -> one dispatch");
    ASSERT(g_last_keyval == 1, "keyval passed through");
    /* The parser itself sends nothing -- the app-side override restores the
     * button-label icon (not exercised by this test's weak override). */
    ASSERT(g_tx_count == 0, "parser emits no frame of its own");

    printf("[PASS] test_parse_rx_dispatches\n");
    return true;
}

static bool test_parse_rx_byte_by_byte(void)
{
    printf("Running test_parse_rx_byte_by_byte...\n");
    reset_capture();

    uint8_t f[9];
    build_touch_frame(f, VP_SYS_BTN_KEY, 2);
    for (uint16_t i = 0; i < sizeof(f); i++) {
        DWIN_ParseRX(&f[i], 1);
    }

    ASSERT(g_action_count == 1, "reassembled from single bytes");
    ASSERT(g_last_keyval == 2, "correct keyval");

    printf("[PASS] test_parse_rx_byte_by_byte\n");
    return true;
}

static bool test_parse_rx_ignores_zero_and_other_vp(void)
{
    printf("Running test_parse_rx_ignores_zero_and_other_vp...\n");
    reset_capture();

    uint8_t f[9];
    build_touch_frame(f, VP_SYS_BTN_KEY, 0); /* keyval 0 = nothing pressed */
    DWIN_ParseRX(f, sizeof(f));
    ASSERT(g_action_count == 0 && g_tx_count == 0, "keyval 0 -> no dispatch, no clear");

    build_touch_frame(f, VP_DC_VOLTAGE, 1);     /* not the action button VP */
    DWIN_ParseRX(f, sizeof(f));
    ASSERT(g_action_count == 0, "other VP -> no dispatch");

    printf("[PASS] test_parse_rx_ignores_zero_and_other_vp\n");
    return true;
}

static bool test_parse_rx_dispatches_precharge_keys(void)
{
    printf("Running test_parse_rx_dispatches_precharge_keys...\n");
    reset_capture();

    uint8_t f[9];
    build_touch_frame(f, VP_LOGIN_KEY, DWIN_LOGIN_KEY_DIGIT_0);
    DWIN_ParseRX(f, sizeof(f));
    ASSERT(g_key_event_count == 1, "login digit 0 dispatches as a real key");
    ASSERT(g_last_event_vp == VP_LOGIN_KEY && g_last_event_key == DWIN_LOGIN_KEY_DIGIT_0,
           "login VP/key are preserved");

    build_touch_frame(f, VP_PRECHARGE_ACTION_KEY, DWIN_PRECHARGE_KEY_START);
    DWIN_ParseRX(f, sizeof(f));
    ASSERT(g_key_event_count == 2, "pre-charge action dispatches");
    ASSERT(g_last_event_vp == VP_PRECHARGE_ACTION_KEY &&
           g_last_event_key == DWIN_PRECHARGE_KEY_START, "pre-charge VP/key preserved");
    ASSERT(g_action_count == 0, "new key VPs do not invoke dashboard action callback");

    printf("[PASS] test_parse_rx_dispatches_precharge_keys\n");
    return true;
}

static bool test_parse_rx_resyncs_on_stray_header1(void)
{
    printf("Running test_parse_rx_resyncs_on_stray_header1...\n");
    reset_capture();

    uint8_t good[9];
    build_touch_frame(good, VP_SYS_BTN_KEY, 3);

    uint8_t stream[16];
    uint16_t n = 0;
    stream[n++] = DWIN_HEADER_1;
    stream[n++] = DWIN_HEADER_1; /* wrong byte where HEADER_2 was expected */
    memcpy(&stream[n], good, sizeof(good));
    n += sizeof(good);

    DWIN_ParseRX(stream, n);

    ASSERT(g_action_count == 1, "parser resyncs and still parses the real frame");
    ASSERT(g_last_keyval == 3, "correct keyval after resync");

    printf("[PASS] test_parse_rx_resyncs_on_stray_header1\n");
    return true;
}

static bool test_parse_rx_rejects_oversized_length(void)
{
    printf("Running test_parse_rx_rejects_oversized_length...\n");
    reset_capture();

    uint8_t bad[3] = { DWIN_HEADER_1, DWIN_HEADER_2, 0xFF };
    DWIN_ParseRX(bad, sizeof(bad));

    uint8_t good[9];
    build_touch_frame(good, VP_SYS_BTN_KEY, 4);
    DWIN_ParseRX(good, sizeof(good));

    ASSERT(g_action_count == 1, "parser recovers after an oversized LEN, not wedged");
    ASSERT(g_last_keyval == 4, "the frame after the rejected one parses");

    printf("[PASS] test_parse_rx_rejects_oversized_length\n");
    return true;
}

static bool test_update_data_scatter(void)
{
    printf("Running test_update_data_scatter...\n");
    reset_capture();

    DWIN_SystemData_t d;
    memset(&d, 0, sizeof(d));
    memcpy(d.dc_voltage_text, "521.0 V", 7U);
    strncpy(d.dc_current_text, "12.0 A", sizeof(d.dc_current_text) - 1U);
    strncpy(d.dc_power_text, "6.3 kW", sizeof(d.dc_power_text) - 1U);
    memcpy(d.bat_pack_volt_text, "400.0 V", 7U);
    strncpy(d.bat_cell_volt_text, "3.315", sizeof(d.bat_cell_volt_text) - 1U);
    strncpy(d.bat_cap_text, "50.0 Ah", sizeof(d.bat_cap_text) - 1U);
    strncpy(d.ac_l1_text, "220 V", sizeof(d.ac_l1_text) - 1U);
    strncpy(d.ac_l2_text, "221 V", sizeof(d.ac_l2_text) - 1U);
    strncpy(d.ac_l3_text, "219 V", sizeof(d.ac_l3_text) - 1U);
    strncpy(d.temp_battery_text, "25.0 C", sizeof(d.temp_battery_text) - 1U);
    strncpy(d.temp_charge_text, "40.0 C", sizeof(d.temp_charge_text) - 1U);
    strncpy(d.temp_jack_text, "30.0 C", sizeof(d.temp_jack_text) - 1U);
    strncpy(d.soc_text, "50%", sizeof(d.soc_text) - 1U);
    d.status_icon = DWIN_STATUS_CHARGING;
    d.btn_mode = DWIN_BTN_STOP;
    strncpy(d.topbar_fault_code, "0000", sizeof(d.topbar_fault_code) - 1);
    d.charge_duration_s = 125;
    d.uptime_s = 3600;

    const int steps = 12;
    bool found_cell_voltage = false;

    /* First full cycle: every step sends (no previous snapshot). */
    int frames_first_cycle = 0;
    for (int i = 0; i < steps; i++) {
        reset_capture();
        DWIN_UpdateData(&d);
        frames_first_cycle += g_tx_count;
        for (int frame = 0; frame < g_tx_count && frame < MAX_FRAMES; frame++) {
            uint16_t vp = ((uint16_t)g_tx[frame][4] << 8) | g_tx[frame][5];
            if (vp == VP_BAT_CELL_VOLT_TEXT) {
                ASSERT(g_tx[frame][6] == '3' && g_tx[frame][7] == '.' &&
                       g_tx[frame][8] == '3' && g_tx[frame][9] == '1' &&
                       g_tx[frame][10] == '5',
                       "cell voltage text must carry 3.315");
                found_cell_voltage = true;
            }
        }
        ASSERT(g_tx_count <= 4, "reasonable frame count per call");
    }
    ASSERT(frames_first_cycle >= 8, "first cycle pushes field groups");
    ASSERT(found_cell_voltage, "first full cycle pushes cell voltage text");

    /* The sync invalidation test before this case marks the alarm table dirty
     * as a panel-recovery replay would. Drain those bounded row updates before
     * asserting that an unchanged steady-state snapshot is quiet. */
    for (int cycle = 0; cycle < (int)VP_ALARM_ROW_COUNT; cycle++) {
        for (int i = 0; i < steps; i++) {
            DWIN_UpdateData(&d);
        }
    }

    /* Second cycle, unchanged data: nothing is re-sent. */
    int frames_second_cycle = 0;
    for (int i = 0; i < steps; i++) {
        reset_capture();
        DWIN_UpdateData(&d);
        frames_second_cycle += g_tx_count;
    }
    ASSERT(frames_second_cycle == 0, "unchanged data is diff-suppressed");

    /* Change one field: exactly one frame next cycle, on the temp step. */
    strncpy(d.temp_charge_text, "42.5 C", sizeof(d.temp_charge_text) - 1U);
    int frames_after_change = 0;
    for (int i = 0; i < steps; i++) {
        reset_capture();
        DWIN_UpdateData(&d);
        frames_after_change += g_tx_count;
    }
    ASSERT(frames_after_change == 1, "one changed field -> one frame");

    /* DWIN_ForceFullRefresh(): next full cycle re-sends every group */
    DWIN_ForceFullRefresh();
    uint16_t seen = 0;
    for (int i = 0; i < steps; i++) {
        reset_capture();
        DWIN_UpdateData(&d);
        for (int frame = 0; frame < g_tx_count && frame < MAX_FRAMES; frame++) {
            uint16_t vp = ((uint16_t)g_tx[frame][4] << 8) | g_tx[frame][5];
            if (vp == VP_DC_VOLTAGE)              seen |= 1u << 0;
            else if (vp == VP_BAT_PACK_VOLT_TEXT) seen |= 1u << 1;
            else if (vp == VP_BAT_CHARGED_AH_TEXT) seen |= 1u << 2;
            else if (vp == VP_AC_PHASE_L1)        seen |= 1u << 3;
            else if (vp == VP_TEMP_BATTERY_TEXT)  seen |= 1u << 4;
            else if (vp == DWIN_SOC_TEXT_VP)      seen |= 1u << 5;
            else if (vp == VP_SYS_BTN_ICON)       seen |= 1u << 6;
            else if (vp == VP_TOPBAR_FAULT_CODE)  seen |= 1u << 7;
            else if (vp == VP_CHG_DURATION)       seen |= 1u << 8;
        }
    }
    ASSERT((seen & 0x01FB) == 0x01FB, "forced refresh re-sends all dashboard groups");

    /* ForceFullRefresh marks all 12 alarm rows dirty; STEP_ALARM_ROW services 1 row
     * per 11-step cycle to avoid UART congestion. Drain the remaining rows: */
    for (int c = 0; c < (int)VP_ALARM_ROW_COUNT - 1; c++) {
        for (int i = 0; i < steps; i++) {
            DWIN_UpdateData(&d);
        }
    }

    /* All forced fields and alarm rows are now sent: subsequent cycle is diff-suppressed. */
    int frames_after_force = 0;
    for (int i = 0; i < steps; i++) {
        reset_capture();
        DWIN_UpdateData(&d);
        frames_after_force += g_tx_count;
    }
    ASSERT(frames_after_force == 0, "force is one-shot, not sticky");

    printf("[PASS] test_update_data_scatter\n");
    return true;
}

static bool test_alarm_fifo_push(void)
{
    printf("Running test_alarm_fifo_push...\n");
    reset_capture();

    /* Push 5 alarms sequentially; since buffer is 4 rows, oldest is dropped */
    const uint16_t desc1[] = { 'L', '1', 0 };
    const uint16_t desc2[] = { 'L', '2', 0 };
    const uint16_t desc3[] = { 'L', '3', 0 };
    const uint16_t desc4[] = { 'L', '4', 0 };
    const uint16_t desc5[] = { 'L', '5', 0 };

    DWIN_Alarm_Push("10:00:01", "E001", desc1, 2);
    DWIN_Alarm_Push("10:00:02", "E002", desc2, 2);
    DWIN_Alarm_Push("10:00:03", "E003", desc3, 2);
    DWIN_Alarm_Push("10:00:04", "E004", desc4, 2);
    DWIN_Alarm_Push("10:00:05", "E005", desc5, 2);

    DWIN_SystemData_t d;
    memset(&d, 0, sizeof(d));

    /* Run scatter steps until all 12 alarm rows are emitted */
    reset_capture();
    for (int i = 0; i < 150; i++) {
        DWIN_UpdateData(&d);
    }
    ASSERT(g_tx_count >= 36, "all 12 alarm rows emitted");

    /* Check newest alarm at row 0 (base 0x1200) has "E005" */
    bool found_e005 = false;
    for (int i = 0; i < g_tx_count; i++) {
        uint16_t vp = ((uint16_t)g_tx[i][4] << 8) | g_tx[i][5];
        if (vp == (VP_ALARM_ROW_1 + ALARM_OFFSET_CODE)) {
            if (memcmp(&g_tx[i][6], "E005", 4) == 0) {
                found_e005 = true;
            }
        }
    }
    ASSERT(found_e005, "row 0 has the newest alarm E005");

    printf("[PASS] test_alarm_fifo_push\n");
    return true;
}

static bool test_soc_color_write_and_cache(void)
{
    printf("Running test_soc_color_write_and_cache...\n");
    reset_capture();

    DWIN_SetSocColor(DWIN_SOC_COLOR_CRITICAL);
    ASSERT(g_tx_count == 1 && g_tx_len[0] == 8, "SOC color emits one WORD frame");
    ASSERT(g_tx[0][2] == 5 && g_tx[0][3] == DWIN_CMD_WRITE, "SOC color frame is a DWIN write");
    ASSERT((((uint16_t)g_tx[0][4] << 8) | g_tx[0][5]) == DWIN_SOC_COLOR_ADDR,
           "SOC color uses SP WORD offset address");
    ASSERT(g_tx[0][6] == 0xF8 && g_tx[0][7] == 0x00, "SOC critical color is RGB565 red");

    reset_capture();
    DWIN_SetSocColor(DWIN_SOC_COLOR_CRITICAL);
    ASSERT(g_tx_count == 0, "unchanged SOC color is suppressed");

    DWIN_ForceFullRefresh();
    reset_capture();
    DWIN_SetSocColor(DWIN_SOC_COLOR_CRITICAL);
    ASSERT(g_tx_count == 1, "full refresh invalidates cached SOC color");

    reset_capture();
    DWIN_SetSocColor(DWIN_SOC_COLOR_LOW);
    ASSERT(g_tx_count == 1 && g_tx[0][6] == 0xFD && g_tx[0][7] == 0x20,
           "SOC low color is RGB565 orange");
    reset_capture();
    DWIN_SetSocColor(DWIN_SOC_COLOR_MEDIUM);
    ASSERT(g_tx_count == 1 && g_tx[0][6] == 0xD5 && g_tx[0][7] == 0x20,
           "SOC medium color is RGB565 muted amber");
    reset_capture();
    DWIN_SetSocColor(DWIN_SOC_COLOR_NORMAL);
    ASSERT(g_tx_count == 1 && g_tx[0][6] == 0x2C && g_tx[0][7] == 0xEA,
           "SOC normal color is RGB565 muted green");
    reset_capture();
    DWIN_SetSocColor(DWIN_SOC_COLOR_UNAVAILABLE);
    ASSERT(g_tx_count == 1 && g_tx[0][6] == 0x84 && g_tx[0][7] == 0x10,
           "SOC unavailable color is RGB565 gray");

    printf("[PASS] test_soc_color_write_and_cache\n");
    return true;
}

static bool test_dwin_precharge_page_update_and_diff(void)
{
    printf("Running test_dwin_precharge_page_update_and_diff...\n");
    DWIN_SystemData_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.precharge_voltage_text, "45.0 V", sizeof(d.precharge_voltage_text) - 1U);
    strncpy(d.precharge_current_text, "20.0 A", sizeof(d.precharge_current_text) - 1U);
    strncpy(d.topbar_fault_code, "E021", sizeof(d.topbar_fault_code) - 1U);
    d.precharge_status_mode = DWIN_PRECHARGE_STATUS_ACTIVE;
    d.precharge_btn_mode = DWIN_PRECHARGE_BTN_STOP;

    DWIN_ForceFullRefresh();
    const int steps = 11;
    bool found_v = false, found_i = false, found_status = false, found_btn = false;
    bool found_fault_code = false;

    for (int i = 0; i < steps; i++) {
        reset_capture();
        DWIN_UpdateData(&d);
        for (int frame = 0; frame < g_tx_count; frame++) {
            uint16_t vp = ((uint16_t)g_tx[frame][4] << 8) | g_tx[frame][5];
            if (vp == VP_PRECHARGE_VOLTAGE_TEXT) found_v = true;
            if (vp == VP_PRECHARGE_CURRENT_TEXT) found_i = true;
            if (vp == VP_PRECHARGE_STATUS_ICON) {
                found_status = true;
                uint16_t val = ((uint16_t)g_tx[frame][6] << 8) | g_tx[frame][7];
                ASSERT(val == DWIN_PRECHARGE_STATUS_ACTIVE, "status icon active mode value");
            }
            if (vp == VP_PRECHARGE_BTN_ICON) {
                found_btn = true;
                uint16_t val = ((uint16_t)g_tx[frame][6] << 8) | g_tx[frame][7];
                ASSERT(val == DWIN_PRECHARGE_BTN_STOP, "btn icon stop mode value");
            }
            if (vp == VP_TOPBAR_FAULT_CODE) {
                found_fault_code = true;
                ASSERT(g_tx[frame][6] == 'E' && g_tx[frame][7] == '0' &&
                       g_tx[frame][8] == '2' && g_tx[frame][9] == '1',
                       "shared fault code field carries the alarm code");
            }
        }
    }
    ASSERT(found_v && found_i && found_status && found_btn && found_fault_code,
           "precharge fields and shared fault code emitted during full refresh");

    /* Drain any remaining alarm row updates if any */
    for (int c = 0; c < (int)VP_ALARM_ROW_COUNT; c++) {
        for (int i = 0; i < steps; i++) {
            DWIN_UpdateData(&d);
        }
    }

    /* Next full cycle: identical data must produce 0 precharge frames */
    for (int i = 0; i < steps; i++) {
        reset_capture();
        DWIN_UpdateData(&d);
        for (int frame = 0; frame < g_tx_count; frame++) {
            uint16_t vp = ((uint16_t)g_tx[frame][4] << 8) | g_tx[frame][5];
            ASSERT(vp != VP_PRECHARGE_VOLTAGE_TEXT && vp != VP_PRECHARGE_CURRENT_TEXT &&
                   vp != VP_PRECHARGE_STATUS_ICON && vp != VP_PRECHARGE_BTN_ICON,
                   "unchanged precharge data must be diff-suppressed");
        }
    }

    /* Change status icon to ERROR and button to RESET */
    d.precharge_status_mode = DWIN_PRECHARGE_STATUS_ERROR;
    d.precharge_btn_mode = DWIN_PRECHARGE_BTN_RESET;
    found_status = false;
    found_btn = false;
    for (int i = 0; i < steps; i++) {
        reset_capture();
        DWIN_UpdateData(&d);
        for (int frame = 0; frame < g_tx_count; frame++) {
            uint16_t vp = ((uint16_t)g_tx[frame][4] << 8) | g_tx[frame][5];
            if (vp == VP_PRECHARGE_STATUS_ICON) {
                found_status = true;
                uint16_t val = ((uint16_t)g_tx[frame][6] << 8) | g_tx[frame][7];
                ASSERT(val == DWIN_PRECHARGE_STATUS_ERROR, "status icon error mode value");
            }
            if (vp == VP_PRECHARGE_BTN_ICON) {
                found_btn = true;
                uint16_t val = ((uint16_t)g_tx[frame][6] << 8) | g_tx[frame][7];
                ASSERT(val == DWIN_PRECHARGE_BTN_RESET, "btn icon reset mode value");
            }
        }
    }
    ASSERT(found_status && found_btn, "changed precharge icons emitted on cycle");

    printf("[PASS] test_dwin_precharge_page_update_and_diff\n");
    return true;
}

static bool test_dwin_login_keypad_full_matrix(void)
{
    printf("Running test_dwin_login_keypad_full_matrix...\n");
    reset_capture();

    const uint16_t keys[] = {
        DWIN_LOGIN_KEY_DIGIT_0, DWIN_LOGIN_KEY_DIGIT_1, DWIN_LOGIN_KEY_DIGIT_2,
        DWIN_LOGIN_KEY_DIGIT_3, DWIN_LOGIN_KEY_DIGIT_4, DWIN_LOGIN_KEY_DIGIT_5,
        DWIN_LOGIN_KEY_DIGIT_6, DWIN_LOGIN_KEY_DIGIT_7, DWIN_LOGIN_KEY_DIGIT_8,
        DWIN_LOGIN_KEY_DIGIT_9, DWIN_LOGIN_KEY_DELETE, DWIN_LOGIN_KEY_OK, DWIN_LOGIN_KEY_BACK
    };
    uint8_t f[9];

    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        build_touch_frame(f, VP_LOGIN_KEY, keys[i]);
        DWIN_ParseRX(f, sizeof(f));
        ASSERT(g_last_event_vp == VP_LOGIN_KEY, "VP matches login key VP");
        ASSERT(g_last_event_key == keys[i], "key code matches dispatched key");
    }

    printf("[PASS] test_dwin_login_keypad_full_matrix\n");
    return true;
}

static bool test_dwin_precharge_touch_matrix(void)
{
    printf("Running test_dwin_precharge_touch_matrix...\n");
    reset_capture();
    uint8_t f[9];

    /* Action key: 0x0001 (Start/Stop/Reset) */
    build_touch_frame(f, VP_PRECHARGE_ACTION_KEY, DWIN_PRECHARGE_KEY_ACTION);
    DWIN_ParseRX(f, sizeof(f));
    ASSERT(g_last_event_vp == VP_PRECHARGE_ACTION_KEY, "VP matches precharge action VP");
    ASSERT(g_last_event_key == DWIN_PRECHARGE_KEY_ACTION, "Action key 0x0001 dispatched");

    /* Back key: 0x0002 (Safe abort and back to home) */
    build_touch_frame(f, VP_PRECHARGE_ACTION_KEY, DWIN_PRECHARGE_KEY_BACK);
    DWIN_ParseRX(f, sizeof(f));
    ASSERT(g_last_event_vp == VP_PRECHARGE_ACTION_KEY, "VP matches precharge action VP");
    ASSERT(g_last_event_key == DWIN_PRECHARGE_KEY_BACK, "Back key 0x0002 dispatched");

    printf("[PASS] test_dwin_precharge_touch_matrix\n");
    return true;
}

static bool test_dwin_config_page_keys(void)
{
    printf("Running test_dwin_config_page_keys...\n");
    reset_capture();
    uint8_t f[9];

    /* TIME & MODE key: VP 0x1130 = 0x0001 */
    build_touch_frame(f, VP_TIME_MODE_KEY, 0x0001);
    DWIN_ParseRX(f, sizeof(f));
    ASSERT(g_last_event_vp == VP_TIME_MODE_KEY, "VP matches time mode key VP");
    ASSERT(g_last_event_key == 0x0001, "Keyval 0x0001 dispatched");

    /* Config Hours: VP 0x1600 = 5 */
    build_touch_frame(f, VP_CFG_HOURS, 5);
    DWIN_ParseRX(f, sizeof(f));
    ASSERT(g_last_event_vp == VP_CFG_HOURS, "VP matches config hours VP");
    ASSERT(g_last_event_key == 5, "Hours value 5 dispatched");

    /* Config Minutes: VP 0x1602 = 45 */
    build_touch_frame(f, VP_CFG_MINUTES, 45);
    DWIN_ParseRX(f, sizeof(f));
    ASSERT(g_last_event_vp == VP_CFG_MINUTES, "VP matches config minutes VP");
    ASSERT(g_last_event_key == 45, "Minutes value 45 dispatched");

    /* SAVE & APPLY keys 1..4 */
    for (uint16_t k = 1; k <= 4; k++) {
        build_touch_frame(f, VP_CFG_APPLY_KEY, k);
        DWIN_ParseRX(f, sizeof(f));
        ASSERT(g_last_event_vp == VP_CFG_APPLY_KEY, "VP matches config apply key VP");
        ASSERT(g_last_event_key == k, "Apply keycode dispatched");
    }

    /* DWIN_SendReadRequest framing check */
    reset_capture();
    DWIN_SendReadRequest(VP_CFG_HOURS, 2);
    ASSERT(g_tx_count == 1, "SendReadRequest emitted 1 frame");
    ASSERT(g_tx_len[0] == 7, "SendReadRequest frame length is 7 bytes");
    ASSERT(g_tx[0][0] == DWIN_HEADER_1 && g_tx[0][1] == DWIN_HEADER_2, "DWIN header correct");
    ASSERT(g_tx[0][2] == 0x04, "Length field is 4");
    ASSERT(g_tx[0][3] == DWIN_CMD_READ, "Command is 0x83 (read)");
    ASSERT(g_tx[0][4] == 0x16 && g_tx[0][5] == 0x00, "VP is 0x1600");
    ASSERT(g_tx[0][6] == 0x02, "n_words is 2");

    printf("[PASS] test_dwin_config_page_keys\n");
    return true;
}

/* ================================================================== */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== DWIN Protocol E2E (real byte stream <-> real dwin_protocol.c) ===\n");
    bool pass = true;

    pass &= test_send_words_single();
    pass &= test_send_words_multi();
    pass &= test_send_string_pads_field();
    pass &= test_set_page_diff_suppressed();
    pass &= test_software_reset_and_sync_invalidation();
    pass &= test_parse_rx_dispatches();
    pass &= test_parse_rx_byte_by_byte();
    pass &= test_parse_rx_ignores_zero_and_other_vp();
    pass &= test_parse_rx_dispatches_precharge_keys();
    pass &= test_parse_rx_resyncs_on_stray_header1();
    pass &= test_parse_rx_rejects_oversized_length();
    pass &= test_update_data_scatter();
    pass &= test_alarm_fifo_push();
    pass &= test_soc_color_write_and_cache();
    pass &= test_dwin_precharge_page_update_and_diff();
    pass &= test_dwin_login_keypad_full_matrix();
    pass &= test_dwin_precharge_touch_matrix();
    pass &= test_dwin_config_page_keys();

    if (pass) {
        printf("ALL TESTS PASSED.\n");
        return 0;
    }
    printf("TESTS FAILED.\n");
    return 1;
}
