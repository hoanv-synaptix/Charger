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
static uint8_t  g_tx[MAX_FRAMES][64];
static uint16_t g_tx_len[MAX_FRAMES];
static int      g_tx_count = 0;

void UART_Transmit_To_DWIN(uint8_t *data, uint16_t len)
{
    if (g_tx_count < MAX_FRAMES) {
        uint16_t n = (len < 64U) ? len : 64U;
        memcpy(g_tx[g_tx_count], data, n);
        g_tx_len[g_tx_count] = len;
    }
    g_tx_count++;
}

static uint16_t g_last_keyval = 0xFFFF;
static int      g_action_count = 0;

void DWIN_OnActionButton(uint16_t keyval)
{
    g_last_keyval = keyval;
    g_action_count++;
}

static void reset_capture(void)
{
    memset(g_tx, 0, sizeof(g_tx));
    memset(g_tx_len, 0, sizeof(g_tx_len));
    g_tx_count = 0;
    g_last_keyval = 0xFFFF;
    g_action_count = 0;
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

static bool test_parse_rx_dispatches(void)
{
    printf("Running test_parse_rx_dispatches...\n");
    reset_capture();

    uint8_t f[9];
    build_touch_frame(f, VP_SYS_BUTTON, 1);
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
    build_touch_frame(f, VP_SYS_BUTTON, 2);
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
    build_touch_frame(f, VP_SYS_BUTTON, 0); /* keyval 0 = nothing pressed */
    DWIN_ParseRX(f, sizeof(f));
    ASSERT(g_action_count == 0 && g_tx_count == 0, "keyval 0 -> no dispatch, no clear");

    build_touch_frame(f, VP_DC_VOLTAGE, 1);     /* not the action button VP */
    DWIN_ParseRX(f, sizeof(f));
    ASSERT(g_action_count == 0, "other VP -> no dispatch");

    printf("[PASS] test_parse_rx_ignores_zero_and_other_vp\n");
    return true;
}

static bool test_parse_rx_resyncs_on_stray_header1(void)
{
    printf("Running test_parse_rx_resyncs_on_stray_header1...\n");
    reset_capture();

    uint8_t good[9];
    build_touch_frame(good, VP_SYS_BUTTON, 3);

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
    build_touch_frame(good, VP_SYS_BUTTON, 4);
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
    d.dc_voltage_x10 = 521;
    d.temp_battery_c_x10 = -50;  /* -5.0 degC */
    d.status_icon = DWIN_STATUS_CHARGING;
    d.btn_mode = DWIN_BTN_STOP;

    /* First full cycle: every step sends (no previous snapshot). */
    int frames_first_cycle = 0;
    for (int i = 0; i < 8; i++) {
        reset_capture();
        DWIN_UpdateData(&d);
        frames_first_cycle += g_tx_count;
        ASSERT(g_tx_count <= 1, "at most one frame per call");
    }
    ASSERT(frames_first_cycle == 8, "first cycle pushes all 8 field groups");

    /* Second cycle, unchanged data: nothing is re-sent. */
    int frames_second_cycle = 0;
    for (int i = 0; i < 8; i++) {
        reset_capture();
        DWIN_UpdateData(&d);
        frames_second_cycle += g_tx_count;
    }
    ASSERT(frames_second_cycle == 0, "unchanged data is diff-suppressed");

    /* Change one field: exactly one frame next cycle, on the temp step. */
    d.temp_charge_c_x10 = 425;  /* 42.5 degC */
    int frames_after_change = 0;
    for (int i = 0; i < 8; i++) {
        reset_capture();
        DWIN_UpdateData(&d);
        frames_after_change += g_tx_count;
    }
    ASSERT(frames_after_change == 1, "one changed field -> one frame");

    /* DWIN_ForceFullRefresh(): next full cycle re-sends every group even
     * though nothing changed. */
    DWIN_ForceFullRefresh();
    uint16_t seen = 0;
    for (int i = 0; i < 8; i++) {
        reset_capture();
        DWIN_UpdateData(&d);
        ASSERT(g_tx_count == 1, "forced cycle: one frame per call");
        uint16_t vp = ((uint16_t)g_tx[0][4] << 8) | g_tx[0][5];
        if (vp == VP_DC_VOLTAGE)          seen |= 1u << 0;
        else if (vp == VP_BAT_PACK_VOLT)  seen |= 1u << 1;
        else if (vp == VP_BAT_CHARGED_AH) seen |= 1u << 2;
        else if (vp == VP_AC_PHASE_L1)    seen |= 1u << 3;
        else if (vp == VP_TEMP_BATTERY)   seen |= 1u << 4;
        else if (vp == VP_SOC_VALUE)      seen |= 1u << 5;
        else if (vp == VP_SYS_BUTTON)   seen |= 1u << 6;
        else if (vp == VP_SET_UPTIME)     seen |= 1u << 7;
    }
    ASSERT(seen == 0xFF, "forced refresh re-sends all 8 field groups");

    /* The force is one-shot: the cycle after it is diff-suppressed again. */
    int frames_after_force = 0;
    for (int i = 0; i < 8; i++) {
        reset_capture();
        DWIN_UpdateData(&d);
        frames_after_force += g_tx_count;
    }
    ASSERT(frames_after_force == 0, "force is one-shot, not sticky");

    printf("[PASS] test_update_data_scatter\n");
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
    pass &= test_parse_rx_dispatches();
    pass &= test_parse_rx_byte_by_byte();
    pass &= test_parse_rx_ignores_zero_and_other_vp();
    pass &= test_parse_rx_resyncs_on_stray_header1();
    pass &= test_parse_rx_rejects_oversized_length();
    pass &= test_update_data_scatter();

    if (pass) {
        printf("ALL TESTS PASSED.\n");
        return 0;
    }
    printf("TESTS FAILED.\n");
    return 1;
}
