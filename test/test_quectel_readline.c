/**
 * @file test_quectel_readline.c
 * @brief Host unit test suite for BSP_Quectel_ReadLine logic
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

#define QUECTEL_RX_BUF_SIZE 2048U

static uint8_t s_rx_buf[QUECTEL_RX_BUF_SIZE];
static volatile uint16_t s_rx_head = 0U;
static volatile uint16_t s_rx_tail = 0U;
static volatile uint32_t s_rx_line_overflow_count = 0U;
static volatile bool s_discarding_line = false;

static void sim_push_byte(uint8_t b)
{
    uint16_t next = (uint16_t)((s_rx_head + 1U) % QUECTEL_RX_BUF_SIZE);
    assert(next != s_rx_tail && "Buffer overflow in test");
    s_rx_buf[s_rx_head] = b;
    s_rx_head = next;
}

static void sim_push_string(const char *str)
{
    while (*str) {
        sim_push_byte((uint8_t)*str++);
    }
}

static void sim_clear_rx(void)
{
    s_rx_tail = s_rx_head;
    s_discarding_line = false;
    s_rx_line_overflow_count = 0U;
}

static bool test_readline(char *line, uint16_t max_len)
{
    if (line == NULL || max_len < 2U) {
        return false;
    }

    /* Phase 1: If currently in discard mode, swallow bytes up to '\n' */
    if (s_discarding_line) {
        while (s_rx_tail != s_rx_head) {
            uint8_t b = s_rx_buf[s_rx_tail];
            s_rx_tail = (uint16_t)((s_rx_tail + 1U) % QUECTEL_RX_BUF_SIZE);
            if (b == '\n') {
                s_discarding_line = false;
                break;
            }
        }
        if (s_discarding_line) {
            return false; /* Still discarding unclosed long line */
        }
    }

    /* Phase 2: Scan buffer for newline '\n' without exceeding max_len-1 */
    uint16_t tail = s_rx_tail;
    uint16_t head = s_rx_head;
    bool found_newline = false;
    uint16_t line_len = 0U;

    while (tail != head) {
        uint8_t b = s_rx_buf[tail];
        tail = (uint16_t)((tail + 1U) % QUECTEL_RX_BUF_SIZE);
        line_len++;
        if (b == '\n') {
            found_newline = true;
            break;
        }
        if (line_len >= (max_len - 1U)) {
            /* Line is too long for destination buffer.
             * Discard all bytes scanned so far and enter discard mode until '\n' */
            s_discarding_line = true;
            ++s_rx_line_overflow_count;
            s_rx_tail = tail;

            /* Opportunistically scan the rest of already-buffered bytes for '\n' */
            while (s_rx_tail != head) {
                uint8_t db = s_rx_buf[s_rx_tail];
                s_rx_tail = (uint16_t)((s_rx_tail + 1U) % QUECTEL_RX_BUF_SIZE);
                if (db == '\n') {
                    s_discarding_line = false;
                    break;
                }
            }
            return false;
        }
    }

    if (!found_newline) {
        return false;
    }

    /* Extract line up to '\n' */
    uint16_t out_idx = 0U;
    while (s_rx_tail != head) {
        uint8_t b = s_rx_buf[s_rx_tail];
        s_rx_tail = (uint16_t)((s_rx_tail + 1U) % QUECTEL_RX_BUF_SIZE);

        if (b == '\r') {
            continue; /* Strip CR */
        }
        if (b == '\n') {
            break; /* End of line reached */
        }
        if (out_idx < (max_len - 1U)) {
            line[out_idx++] = (char)b;
        }
    }
    line[out_idx] = '\0';
    return true;
}

int main(void)
{
    char line[128];
    memset(line, 0, sizeof(line));

    puts("=== Running Quectel ReadLine Host Unit Tests ===");

    /* Test 1: Normal short line */
    sim_clear_rx();
    sim_push_string("AT+CPIN?\r\nOK\r\n");
    assert(test_readline(line, sizeof(line)) == true);
    assert(strcmp(line, "AT+CPIN?") == 0);
    assert(test_readline(line, sizeof(line)) == true);
    assert(strcmp(line, "OK") == 0);
    assert(test_readline(line, sizeof(line)) == false);
    puts("  [PASS] Test 1: Normal short lines");

    /* Test 2: Long line exceeding buffer with newline already buffered */
    sim_clear_rx();
    /* Push 150 'X' chars + \r\n + followed by "AFTER_OVERFLOW\r\n" */
    for (int i = 0; i < 150; i++) sim_push_byte('X');
    sim_push_string("\r\nAFTER_OVERFLOW\r\n");

    /* First call should detect overflow, discard the long line, and return false */
    bool res = test_readline(line, sizeof(line));
    assert(res == false);
    assert(s_rx_line_overflow_count == 1);
    assert(s_discarding_line == false); /* Opportunistic scan caught the newline */

    /* Second call MUST read the next valid line without getting stuck */
    assert(test_readline(line, sizeof(line)) == true);
    assert(strcmp(line, "AFTER_OVERFLOW") == 0);
    assert(test_readline(line, sizeof(line)) == false);
    puts("  [PASS] Test 2: Long line discarded with newline already buffered");

    /* Test 3: Long line arriving in chunks (no newline initially) */
    sim_clear_rx();
    /* Push 140 'Y' chars with NO newline */
    for (int i = 0; i < 140; i++) sim_push_byte('Y');

    /* Call readline -> triggers discard mode */
    assert(test_readline(line, sizeof(line)) == false);
    assert(s_rx_line_overflow_count == 1);
    assert(s_discarding_line == true);

    /* Push more garbage chars still without newline */
    for (int i = 0; i < 50; i++) sim_push_byte('Z');
    assert(test_readline(line, sizeof(line)) == false);
    assert(s_discarding_line == true);

    /* Finally push newline + new line "RECOVERED\r\n" */
    sim_push_string("\r\nRECOVERED\r\n");
    /* This call finishes discard AND immediately extracts "RECOVERED" */
    assert(test_readline(line, sizeof(line)) == true);
    assert(s_discarding_line == false);
    assert(strcmp(line, "RECOVERED") == 0);
    assert(test_readline(line, sizeof(line)) == false);
    puts("  [PASS] Test 3: Long line arriving in chunks across multiple calls");

    /* Test 4: Ring buffer wrap-around */
    sim_clear_rx();
    /* Advance tail/head near the end of buffer */
    s_rx_head = QUECTEL_RX_BUF_SIZE - 20U;
    s_rx_tail = QUECTEL_RX_BUF_SIZE - 20U;

    sim_push_string("WRAP_AROUND_LINE\r\n");
    assert(test_readline(line, sizeof(line)) == true);
    assert(strcmp(line, "WRAP_AROUND_LINE") == 0);
    puts("  [PASS] Test 4: Buffer wrap-around handled smoothly");

    puts(">>> ALL QUECTEL READLINE TESTS PASSED! <<<");
    return 0;
}
