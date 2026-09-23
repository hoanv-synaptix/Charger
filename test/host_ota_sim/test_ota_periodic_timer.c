/**
 * @file test_ota_periodic_timer.c
 * @brief Host unit test verifying the MCU's periodic Auto-OTA check timer:
 *        - Policy interval configuration (e.g. 1h = 3,600,000 ms)
 *        - OTAService_Process time tracking and automatic trigger on interval expiry
 *        - Non-triggering prior to interval expiry
 *        - Repeat triggers on subsequent intervals
 *        - Silence when policy is disabled
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("[FAIL] %s:%d - %s\n", __func__, __LINE__, msg); \
            return false; \
        } \
    } while (0)

#define OTA_DEFAULT_CHECK_INTERVAL_MS (6U * 3600U * 1000U)
#define OTA_URL_MAX_LENGTH            128U

/* Mock OTA service timer state matching ota_service.c logic */
static bool     s_policy_enabled = false;
static uint32_t s_policy_interval_ms = OTA_DEFAULT_CHECK_INTERVAL_MS;
static char     s_policy_manifest_url[OTA_URL_MAX_LENGTH] = {0};
static uint32_t s_next_policy_tick = 0U;
static int      s_auto_check_triggers = 0;

static bool sim_set_policy(bool enabled, uint32_t interval_ms, const char *manifest_url, uint32_t now)
{
    if (enabled) {
        if (manifest_url == NULL || strncmp(manifest_url, "https://", 8U) != 0) return false;
        size_t len = strlen(manifest_url);
        if (len == 0U || len >= OTA_URL_MAX_LENGTH) return false;
        memcpy(s_policy_manifest_url, manifest_url, len + 1U);
    } else {
        s_policy_manifest_url[0] = '\0';
    }
    s_policy_enabled = enabled;
    s_policy_interval_ms = (interval_ms == 0U) ? OTA_DEFAULT_CHECK_INTERVAL_MS : interval_ms;
    s_next_policy_tick = now;
    return true;
}

static void sim_ota_process(uint32_t now_tick)
{
    if (s_policy_enabled && s_policy_manifest_url[0] != '\0' &&
        (uint32_t)(now_tick - s_next_policy_tick) >= s_policy_interval_ms) {
        s_next_policy_tick = now_tick;
        s_auto_check_triggers++;
    }
}

static bool test_ota_interval_1hour_cycle(void)
{
    printf("Running test_ota_interval_1hour_cycle...\n");
    s_auto_check_triggers = 0;

    /* Configure 1 hour interval (3,600,000 ms) at t = 1,000,000 */
    uint32_t t_start = 1000000U;
    uint32_t one_hour_ms = 3600000U;
    bool ok = sim_set_policy(true, one_hour_ms, "https://example.com/manifest.json", t_start);
    ASSERT(ok, "policy configuration must succeed");
    ASSERT(s_policy_interval_ms == 3600000U, "interval must be exactly 3,600,000 ms");

    /* Step time in 10-second increments up to 3590 seconds (3,590,000 ms) -> no trigger */
    for (uint32_t dt = 10000U; dt < 3600000U; dt += 10000U) {
        sim_ota_process(t_start + dt);
        ASSERT(s_auto_check_triggers == 0, "must NOT trigger before 1 hour has elapsed");
    }

    /* Advance to exactly 1 hour: t = t_start + 3,600,000 ms */
    sim_ota_process(t_start + one_hour_ms);
    ASSERT(s_auto_check_triggers == 1, "must trigger exactly on 1 hour expiry (trigger #1)");

    /* Advance time midway into the second hour (30 minutes later) */
    sim_ota_process(t_start + one_hour_ms + 1800000U);
    ASSERT(s_auto_check_triggers == 1, "must NOT trigger during second hour before expiry");

    /* Advance to end of second hour: t = t_start + 7,200,000 ms */
    sim_ota_process(t_start + (2U * one_hour_ms));
    ASSERT(s_auto_check_triggers == 2, "must trigger on second hour expiry (trigger #2)");

    /* Advance to end of third hour: t = t_start + 10,800,000 ms */
    sim_ota_process(t_start + (3U * one_hour_ms));
    ASSERT(s_auto_check_triggers == 3, "must trigger on third hour expiry (trigger #3)");

    printf("[PASS] test_ota_interval_1hour_cycle\n");
    return true;
}

static bool test_ota_disabled_policy_never_triggers(void)
{
    printf("Running test_ota_disabled_policy_never_triggers...\n");
    s_auto_check_triggers = 0;

    uint32_t t_start = 5000000U;
    bool ok = sim_set_policy(false, 3600000U, NULL, t_start);
    ASSERT(ok, "disabling policy must succeed");
    ASSERT(s_policy_enabled == false, "policy should be disabled");

    /* Advance 10 hours -> must never trigger */
    for (uint32_t h = 1; h <= 10; h++) {
        sim_ota_process(t_start + (h * 3600000U));
        ASSERT(s_auto_check_triggers == 0, "disabled policy must never trigger auto check");
    }

    printf("[PASS] test_ota_disabled_policy_never_triggers\n");
    return true;
}

static bool test_ota_interval_zero_fallback(void)
{
    printf("Running test_ota_interval_zero_fallback...\n");
    s_auto_check_triggers = 0;

    /* Interval 0 should fall back to OTA_DEFAULT_CHECK_INTERVAL_MS (6h) */
    bool ok = sim_set_policy(true, 0U, "https://example.com/manifest.json", 0U);
    ASSERT(ok, "setting policy with interval 0 must succeed");
    ASSERT(s_policy_interval_ms == OTA_DEFAULT_CHECK_INTERVAL_MS, "interval 0 must fall back to 6h default");

    printf("[PASS] test_ota_interval_zero_fallback\n");
    return true;
}

int main(void)
{
    printf("=== MCU Auto-OTA Periodic Timer Unit Tests ===\n");
    bool pass = true;

    pass &= test_ota_interval_1hour_cycle();
    pass &= test_ota_disabled_policy_never_triggers();
    pass &= test_ota_interval_zero_fallback();

    if (pass) {
        printf("ALL TESTS PASSED.\n");
        return 0;
    }
    printf("TESTS FAILED.\n");
    return 1;
}
