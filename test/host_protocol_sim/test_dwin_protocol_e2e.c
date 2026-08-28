/**
 * @file test_dwin_protocol_e2e.c
 * @brief Host-compiled end-to-end test for Modules/hmi/dwin_protocol.c --
 *        previously ZERO automated test coverage (confirmed: no reference
 *        to dwin_protocol anywhere under test/ before this file). Compiles
 *        the real dwin_protocol.c, feeds real byte streams into the real
 *        DWIN_ParseRX(), and asserts on the real DWIN_OnCommandReceived()
 *        callback and DWIN_SendInt()/DWIN_SendString() wire output.
 *
 * dwin_protocol.c has no STM32 HAL dependency at all -- it only needs
 * UART_Transmit_To_DWIN() (extern, stubbed below) and defines a weak
 * DWIN_OnCommandReceived() this file overrides -- so no mock_hal scaffolding
 * is needed beyond that.
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
/* UART_Transmit_To_DWIN stub -- captures the last frame DWIN_SendInt()/  */
/* DWIN_SendString() built, so tests can assert on real wire bytes.       */
/* ================================================================== */

static uint8_t g_last_tx[64];
static uint16_t g_last_tx_len = 0;

void UART_Transmit_To_DWIN(uint8_t *data, uint16_t len)
{
    uint16_t copy_len = (len < sizeof(g_last_tx)) ? len : (uint16_t)sizeof(g_last_tx);
    memcpy(g_last_tx, data, copy_len);
    g_last_tx_len = len;
}

/* DWIN_OnCommandReceived() override -- captures the last command
 * DWIN_ParseRX() dispatched, so tests can assert the real parser correctly
 * recognized a button-press frame from the touchscreen. */
static uint16_t g_last_command = 0xFFFF;
static uint16_t g_command_count = 0;

void DWIN_OnCommandReceived(uint16_t command)
{
    g_last_command = command;
    g_command_count++;
}

static void reset_capture(void)
{
    memset(g_last_tx, 0, sizeof(g_last_tx));
    g_last_tx_len = 0;
    g_last_command = 0xFFFF;
    g_command_count = 0;
}

/* Builds a real DWIN "write VP" RX frame -- [0x5A][0xA5][LEN][0x83][VP_H]
 * [VP_L][word_count][data...] -- matching what a real DWIN touchscreen
 * sends after a button press (VP_CMD_CTRL write), per DWIN_ParseRX(). */
static void build_dwin_cmd_frame(uint8_t *buf, uint16_t vp_addr, uint16_t cmd_val)
{
    buf[0] = DWIN_HEADER_1;
    buf[1] = DWIN_HEADER_2;
    buf[2] = 0x06; /* len: CMD(1) + ADDR(2) + WORD_COUNT(1) + DATA(2) */
    buf[3] = DWIN_READ;
    buf[4] = (uint8_t)(vp_addr >> 8);
    buf[5] = (uint8_t)(vp_addr & 0xFF);
    buf[6] = 0x01; /* word_count */
    buf[7] = (uint8_t)(cmd_val >> 8);
    buf[8] = (uint8_t)(cmd_val & 0xFF);
}

/* ================================================================== */

static bool test_send_int_builds_correct_wire_frame(void)
{
    printf("Running test_send_int_builds_correct_wire_frame...\n");
    reset_capture();

    DWIN_SendInt(VP_DC_VOLT, 4805); /* e.g. 480.5V encoded x10 */

    ASSERT(g_last_tx_len == 8, "DWIN_SendInt should transmit exactly 8 bytes");
    ASSERT(g_last_tx[0] == DWIN_HEADER_1 && g_last_tx[1] == DWIN_HEADER_2, "wrong DWIN header");
    ASSERT(g_last_tx[2] == 0x05, "payload length byte should be 5 (CMD+ADDR(2)+DATA(2))");
    ASSERT(g_last_tx[3] == DWIN_WRITE, "DWIN_SendInt must use the WRITE command byte");
    uint16_t addr = ((uint16_t)g_last_tx[4] << 8) | g_last_tx[5];
    ASSERT(addr == VP_DC_VOLT, "VP address should round-trip correctly");
    uint16_t value = ((uint16_t)g_last_tx[6] << 8) | g_last_tx[7];
    ASSERT(value == 4805, "value should round-trip correctly, big-endian per DWIN protocol");

    printf("[PASS] test_send_int_builds_correct_wire_frame\n");
    return true;
}

static bool test_parse_rx_dispatches_command(void)
{
    printf("Running test_parse_rx_dispatches_command...\n");
    reset_capture();

    uint8_t frame[9];
    build_dwin_cmd_frame(frame, VP_CMD_CTRL, 1 /* e.g. Start button */);
    DWIN_ParseRX(frame, sizeof(frame));

    ASSERT(g_command_count == 1, "exactly one command should be dispatched for one valid frame");
    ASSERT(g_last_command == 1, "dispatched command value should match the frame's payload");

    printf("[PASS] test_parse_rx_dispatches_command\n");
    return true;
}

/* DWIN_ParseRX() is written to be fed byte-by-byte from a UART RX
 * interrupt (its state is `static` across calls) -- confirm it correctly
 * reassembles a command when called one byte at a time, not just when
 * handed the whole frame in one call. */
static bool test_parse_rx_byte_by_byte(void)
{
    printf("Running test_parse_rx_byte_by_byte...\n");
    reset_capture();

    uint8_t frame[9];
    build_dwin_cmd_frame(frame, VP_CMD_CTRL, 2 /* e.g. Stop button */);
    for (uint16_t i = 0; i < sizeof(frame); i++) {
        DWIN_ParseRX(&frame[i], 1);
    }

    ASSERT(g_command_count == 1, "byte-by-byte feed should still dispatch exactly one command");
    ASSERT(g_last_command == 2, "byte-by-byte feed should reassemble the correct command value");

    printf("[PASS] test_parse_rx_byte_by_byte\n");
    return true;
}

/* A write-VP frame from a DIFFERENT vp_addr must not be mistaken for a
 * VP_CMD_CTRL button press -- DWIN_ParseRX() only dispatches when
 * vp_addr == VP_CMD_CTRL (see the `if (vp_addr == VP_CMD_CTRL ...)` gate). */
static bool test_parse_rx_ignores_other_vp_addr(void)
{
    printf("Running test_parse_rx_ignores_other_vp_addr...\n");
    reset_capture();

    uint8_t frame[9];
    build_dwin_cmd_frame(frame, VP_DC_VOLT /* not VP_CMD_CTRL */, 1);
    DWIN_ParseRX(frame, sizeof(frame));

    ASSERT(g_command_count == 0, "a read/write of a non-VP_CMD_CTRL address must not fire the command callback");

    printf("[PASS] test_parse_rx_ignores_other_vp_addr\n");
    return true;
}

/* Regression-style test for the header-resync branch in DWIN_ParseRX()'s
 * rx_idx==1 handling: a stray HEADER_1 byte where HEADER_2 was expected
 * must be treated as the start of a new frame, not desync the parser
 * permanently (same class of bug as pc_protocol.c's B-02 SOF resync). */
static bool test_parse_rx_resyncs_on_stray_header1(void)
{
    printf("Running test_parse_rx_resyncs_on_stray_header1...\n");
    reset_capture();

    uint8_t good_frame[9];
    build_dwin_cmd_frame(good_frame, VP_CMD_CTRL, 3);

    uint8_t stream[16];
    uint16_t n = 0;
    stream[n++] = DWIN_HEADER_1; /* stray, would-be HEADER_2 slot corrupted below */
    stream[n++] = DWIN_HEADER_1; /* wrong byte where HEADER_2 was expected */
    memcpy(&stream[n], good_frame, sizeof(good_frame));
    n += sizeof(good_frame);

    DWIN_ParseRX(stream, n);

    ASSERT(g_command_count == 1, "parser should resync on the second HEADER_1 and still parse the real frame");
    ASSERT(g_last_command == 3, "resynced frame should carry the correct command value");

    printf("[PASS] test_parse_rx_resyncs_on_stray_header1\n");
    return true;
}

static bool test_parse_rx_rejects_oversized_length(void)
{
    printf("Running test_parse_rx_rejects_oversized_length...\n");
    reset_capture();

    /* Length byte > 60 must be rejected (DWIN_ParseRX's own bound) --
     * confirm it doesn't overrun rx_buf[64] or wedge the parser. */
    uint8_t stream[16] = {0};
    stream[0] = DWIN_HEADER_1;
    stream[1] = DWIN_HEADER_2;
    stream[2] = 0xFF; /* invalid length */
    DWIN_ParseRX(stream, 3);

    /* Parser should have reset (rx_idx back to 0) and be ready to accept a
     * fresh, valid frame right after -- prove it by feeding one. */
    uint8_t good_frame[9];
    build_dwin_cmd_frame(good_frame, VP_CMD_CTRL, 4);
    DWIN_ParseRX(good_frame, sizeof(good_frame));

    ASSERT(g_command_count == 1, "parser must recover after an oversized-length frame, not stay wedged");
    ASSERT(g_last_command == 4, "the frame following the rejected one should parse correctly");

    printf("[PASS] test_parse_rx_rejects_oversized_length\n");
    return true;
}

static bool test_send_string_truncates_at_32_chars(void)
{
    printf("Running test_send_string_truncates_at_32_chars...\n");
    reset_capture();

    /* A 40-char string must be capped at 32 (DWIN_SendString()'s own bound)
     * -- otherwise it would write past its 64-byte stack buffer relative
     * to the 6-byte header. */
    const char *long_str = "0123456789012345678901234567890123456789"; /* 40 chars */
    DWIN_SendString(VP_CHARGE_TIME, long_str);

    ASSERT(g_last_tx_len == 6 + 32, "DWIN_SendString should cap payload at 32 chars (38 bytes total)");
    ASSERT(g_last_tx[2] == 3 + 32, "length byte should reflect the capped 32-char payload");
    ASSERT(memcmp(&g_last_tx[6], long_str, 32) == 0, "the first 32 chars should be transmitted unmodified");

    printf("[PASS] test_send_string_truncates_at_32_chars\n");
    return true;
}

/* ================================================================== */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== DWIN Protocol E2E Simulation Test (real byte stream -> real dwin_protocol.c) ===\n");
    bool pass = true;

    pass &= test_send_int_builds_correct_wire_frame();
    pass &= test_parse_rx_dispatches_command();
    pass &= test_parse_rx_byte_by_byte();
    pass &= test_parse_rx_ignores_other_vp_addr();
    pass &= test_parse_rx_resyncs_on_stray_header1();
    pass &= test_parse_rx_rejects_oversized_length();
    pass &= test_send_string_truncates_at_32_chars();

    if (pass) {
        printf("ALL TESTS PASSED.\n");
        return 0;
    }
    printf("TESTS FAILED.\n");
    return 1;
}
