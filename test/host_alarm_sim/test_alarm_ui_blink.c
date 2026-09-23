/**
 * @file test_alarm_ui_blink.c
 * @brief Host unit test for synchronized alarm blinking & buzzer:
 *        - Physical LED_RUN 1 Hz (500ms ON / 500ms OFF)
 *        - DWIN status icon 1 Hz (ERROR 4 / Blank 0xFFFF)
 *        - DWIN topbar fault code 1 Hz ("E006" / "    ")
 *        - Button mode remains solid RESET (no blinking)
 *        - DWIN Buzzer 160ms beep every 1000ms
 *        - Immediate buzzer silence on error reset
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "dwin_protocol.h"
#include "dwin_vp_map.h"

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("[FAIL] %s:%d - %s\n", __func__, __LINE__, msg); \
            return false; \
        } \
    } while (0)

#define ERROR_BLINK_PERIOD_MS        1000U
#define ERROR_BLINK_HALF_PERIOD_MS   500U
#define ERROR_BEEP_DURATION_X8MS     20U

static uint16_t dwin_btn_mode_from_status(uint16_t status_icon)
{
    switch (status_icon) {
        case DWIN_STATUS_READY:     return DWIN_BTN_START;
        case DWIN_STATUS_STARTING:  return DWIN_BTN_STOP;
        case DWIN_STATUS_CHARGING:  return DWIN_BTN_STOP;
        case DWIN_STATUS_ERROR:     return DWIN_BTN_RESET;
        case DWIN_STATUS_COMPLETE:  return DWIN_BTN_START;
        default:                    return DWIN_BTN_START;
    }
}

/* Mock LED state */
static bool s_led_run = false;
static void led_run_on(void)  { s_led_run = true; }
static void led_run_off(void) { s_led_run = false; }

/* Mock DWIN Beep capture */
static uint8_t s_last_beep_val = 0xFF;
static int s_beep_call_count = 0;
void DWIN_Beep(uint8_t duration_x8ms)
{
    s_last_beep_val = duration_x8ms;
    s_beep_call_count++;
}

/* Helper functions simulating app_main logic */
static void sim_led_update(uint32_t now, uint16_t cur_status, uint8_t modules_online, bool pc_charging)
{
    if (cur_status == DWIN_STATUS_ERROR) {
        if ((now % ERROR_BLINK_PERIOD_MS) < ERROR_BLINK_HALF_PERIOD_MS) {
            led_run_on();
        } else {
            led_run_off();
        }
    } else if (modules_online > 0 && pc_charging) {
        led_run_on();
    } else {
        led_run_off();
    }
}

static void sim_dwin_update(uint32_t now, uint16_t raw_status, const char *raw_code,
                            DWIN_SystemData_t *dd, uint32_t *last_beep_tick, bool *was_error)
{
    bool blink_on = ((now % ERROR_BLINK_PERIOD_MS) < ERROR_BLINK_HALF_PERIOD_MS);

    /* Status icon & button mode */
    if (raw_status == DWIN_STATUS_ERROR) {
        dd->status_icon = blink_on ? DWIN_STATUS_ERROR : 0xFFFFU;
    } else {
        dd->status_icon = raw_status;
    }
    dd->btn_mode = dwin_btn_mode_from_status(raw_status);

    /* Topbar fault code */
    if (raw_status == DWIN_STATUS_ERROR && !blink_on) {
        strncpy(dd->topbar_fault_code, "    ", sizeof(dd->topbar_fault_code) - 1U);
        dd->topbar_fault_code[sizeof(dd->topbar_fault_code) - 1U] = '\0';
    } else {
        strncpy(dd->topbar_fault_code, raw_code, sizeof(dd->topbar_fault_code) - 1U);
        dd->topbar_fault_code[sizeof(dd->topbar_fault_code) - 1U] = '\0';
    }

    /* Buzzer */
    if (raw_status == DWIN_STATUS_ERROR) {
        *was_error = true;
        if ((uint32_t)(now - *last_beep_tick) >= ERROR_BLINK_PERIOD_MS) {
            *last_beep_tick = now;
            DWIN_Beep(ERROR_BEEP_DURATION_X8MS);
        }
    } else {
        if (*was_error) {
            *was_error = false;
            DWIN_Beep(0U);
        }
        *last_beep_tick = 0U;
    }
}

static bool test_normal_charging_no_blink(void)
{
    printf("Running test_normal_charging_no_blink...\n");
    s_led_run = false;
    s_beep_call_count = 0;
    uint32_t last_beep = 0;
    bool was_err = false;
    DWIN_SystemData_t dd;
    memset(&dd, 0, sizeof(dd));

    for (uint32_t t = 1000; t <= 3000; t += 100) {
        sim_led_update(t, DWIN_STATUS_CHARGING, 2, true);
        sim_dwin_update(t, DWIN_STATUS_CHARGING, "0000", &dd, &last_beep, &was_err);

        ASSERT(s_led_run == true, "LED_RUN must remain solid ON during charging");
        ASSERT(dd.status_icon == DWIN_STATUS_CHARGING, "Status icon must remain CHARGING (2)");
        ASSERT(dd.btn_mode == DWIN_BTN_STOP, "Button mode must be STOP");
        ASSERT(strcmp(dd.topbar_fault_code, "0000") == 0, "Fault code must be 0000");
    }
    ASSERT(s_beep_call_count == 0, "Buzzer must NOT beep during normal charging");

    printf("[PASS] test_normal_charging_no_blink\n");
    return true;
}

static bool test_error_blinking_and_buzzer(void)
{
    printf("Running test_error_blinking_and_buzzer...\n");
    s_led_run = false;
    s_beep_call_count = 0;
    uint32_t last_beep = 0;
    bool was_err = false;
    DWIN_SystemData_t dd;
    memset(&dd, 0, sizeof(dd));

    /* Test across 2 complete 1-second cycles (t=10000 to t=12000 ms) */
    for (uint32_t t = 10000; t < 12000; t += 50) {
        sim_led_update(t, DWIN_STATUS_ERROR, 2, false);
        sim_dwin_update(t, DWIN_STATUS_ERROR, "E006", &dd, &last_beep, &was_err);

        bool in_on_phase = ((t % 1000U) < 500U);

        if (in_on_phase) {
            ASSERT(s_led_run == true, "LED_RUN must be ON during first 500ms");
            ASSERT(dd.status_icon == DWIN_STATUS_ERROR, "Status icon must be ERROR (4) during first 500ms");
            ASSERT(strcmp(dd.topbar_fault_code, "E006") == 0, "Fault code must show E006 during first 500ms");
        } else {
            ASSERT(s_led_run == false, "LED_RUN must be OFF during second 500ms");
            ASSERT(dd.status_icon == 0xFFFFU, "Status icon must be hidden (0xFFFF) during second 500ms");
            ASSERT(strcmp(dd.topbar_fault_code, "    ") == 0, "Fault code must be blank during second 500ms");
        }

        /* Action button must always stay RESET (no blinking) */
        ASSERT(dd.btn_mode == DWIN_BTN_RESET, "Reset button must remain solid RESET throughout");
    }

    /* In 2000ms duration, buzzer should have triggered exactly twice (t=10000 and t=11000) */
    ASSERT(s_beep_call_count == 2, "Buzzer must beep exactly twice in 2000ms");
    ASSERT(s_last_beep_val == ERROR_BEEP_DURATION_X8MS, "Buzzer duration must be 20 (160ms)");

    /* Now test recovery/reset at t=12050 */
    sim_dwin_update(12050, DWIN_STATUS_READY, "0000", &dd, &last_beep, &was_err);
    ASSERT(s_last_beep_val == 0U, "Buzzer must be immediately silenced (0) on error clear");
    ASSERT(dd.status_icon == DWIN_STATUS_READY, "Status icon returns to READY");
    ASSERT(dd.btn_mode == DWIN_BTN_START, "Button returns to START");

    printf("[PASS] test_error_blinking_and_buzzer\n");
    return true;
}

int main(void)
{
    printf("=== Alarm UI Blinking & Buzzer Unit Tests ===\n");
    bool pass = true;
    pass &= test_normal_charging_no_blink();
    pass &= test_error_blinking_and_buzzer();

    if (pass) {
        printf("ALL TESTS PASSED.\n");
        return 0;
    }
    printf("TESTS FAILED.\n");
    return 1;
}
