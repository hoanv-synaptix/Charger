/**
 * @file test_quectel_time_sync.c
 * @brief Host unit test suite for Quectel QLTS time synchronization logic
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

#define BSP_RTC_TIMEZONE_HOURS   (+7)
#define BSP_RTC_TIMEZONE_SEC     ((int32_t)(BSP_RTC_TIMEZONE_HOURS * 3600))

typedef struct {
    uint16_t year;    /* 2000..2099 */
    uint8_t  month;   /* 1..12 */
    uint8_t  day;     /* 1..31 */
    uint8_t  hour;    /* 0..23 */
    uint8_t  minute;  /* 0..59 */
    uint8_t  second;  /* 0..59 */
    uint8_t  weekday; /* 1..7 (1=Mon .. 7=Sun) */
} BSP_RTC_DateTime_t;

static const uint16_t s_days_before_month[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

static inline bool is_leap_year(uint16_t y)
{
    return ((y % 4U == 0U && y % 100U != 0U) || (y % 400U == 0U));
}

static uint32_t simulated_DateTimeToEpoch(const BSP_RTC_DateTime_t *dt)
{
    if (dt == NULL || dt->year < 1970U || dt->month < 1U || dt->month > 12U ||
        dt->day < 1U || dt->day > 31U || dt->hour > 23U || dt->minute > 59U || dt->second > 59U) {
        return 0U;
    }

    uint32_t days = 0U;
    for (uint16_t y = 1970U; y < dt->year; y++) {
        days += is_leap_year(y) ? 366U : 365U;
    }

    days += s_days_before_month[dt->month - 1U];
    if (dt->month > 2U && is_leap_year(dt->year)) {
        days += 1U;
    }
    days += (dt->day - 1U);

    return ((days * 24U + dt->hour) * 60U + dt->minute) * 60U + dt->second;
}

static void simulated_EpochToDateTime(uint32_t epoch, BSP_RTC_DateTime_t *dt)
{
    if (dt == NULL) return;

    uint32_t sec = epoch % 60U;
    epoch /= 60U;
    uint32_t min = epoch % 60U;
    epoch /= 60U;
    uint32_t hour = epoch % 24U;
    uint32_t days = epoch / 24U;

    dt->weekday = (uint8_t)((days + 3U) % 7U + 1U);

    uint16_t y = 1970U;
    while (1) {
        uint16_t diy = is_leap_year(y) ? 366U : 365U;
        if (days < diy) break;
        days -= diy;
        y++;
    }
    dt->year = y;

    bool leap = is_leap_year(y);
    uint8_t m = 1U;
    for (; m <= 12U; m++) {
        uint8_t dim;
        if (m == 2U) {
            dim = leap ? 29U : 28U;
        } else if (m == 4U || m == 6U || m == 9U || m == 11U) {
            dim = 30U;
        } else {
            dim = 31U;
        }
        if (days < dim) break;
        days -= dim;
    }
    dt->month = m;
    dt->day = (uint8_t)(days + 1U);
    dt->hour = (uint8_t)hour;
    dt->minute = (uint8_t)min;
    dt->second = (uint8_t)sec;
}

/* Simulated parser function matching quectel_at_engine.c */
typedef enum {
    PARSE_QLTS_SUCCESS = 0,
    PARSE_QLTS_EMPTY,
    PARSE_QLTS_INVALID_FORMAT,
    PARSE_QLTS_OUT_OF_RANGE,
    PARSE_QLTS_REJECTED_ROLLBACK
} ParseQltsResult_t;

static ParseQltsResult_t parse_and_validate_qlts(
    const char *line,
    bool rtc_valid,
    uint32_t cur_epoch,
    uint32_t *out_utc_epoch,
    BSP_RTC_DateTime_t *out_local_dt)
{
    if (strncmp(line, "+QLTS:", 6) != 0) {
        return PARSE_QLTS_INVALID_FORMAT;
    }

    const char *p = strchr(line, '"');
    if (p == NULL) {
        return PARSE_QLTS_INVALID_FORMAT;
    }

    if (*(p + 1) == '"') {
        return PARSE_QLTS_EMPTY;
    }

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    p++; /* skip opening quote */
    if (sscanf(p, "%d/%d/%d,%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
        return PARSE_QLTS_INVALID_FORMAT;
    }

    if (year < 2024 || year > 2099 ||
        month < 1 || month > 12 ||
        day < 1 || day > 31 ||
        hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        return PARSE_QLTS_OUT_OF_RANGE;
    }

    BSP_RTC_DateTime_t dt_utc;
    memset(&dt_utc, 0, sizeof(dt_utc));
    dt_utc.year = (uint16_t)year;
    dt_utc.month = (uint8_t)month;
    dt_utc.day = (uint8_t)day;
    dt_utc.hour = (uint8_t)hour;
    dt_utc.minute = (uint8_t)minute;
    dt_utc.second = (uint8_t)second;
    dt_utc.weekday = 1U;

    uint32_t utc_epoch = simulated_DateTimeToEpoch(&dt_utc);
    if (utc_epoch == 0U) {
        return PARSE_QLTS_OUT_OF_RANGE;
    }

    if (rtc_valid) {
        int32_t diff = (int32_t)(utc_epoch - cur_epoch);
        if (diff < -300) {
            return PARSE_QLTS_REJECTED_ROLLBACK;
        }
    }

    if (out_utc_epoch != NULL) {
        *out_utc_epoch = utc_epoch;
    }

    if (out_local_dt != NULL) {
        uint32_t local_epoch = utc_epoch + (uint32_t)BSP_RTC_TIMEZONE_SEC;
        simulated_EpochToDateTime(local_epoch, out_local_dt);
    }

    return PARSE_QLTS_SUCCESS;
}

int main(void)
{
    printf("=== RUNNING QUECTEL QLTS TIME SYNC UNIT TESTS ===\n");

    /* TEST 1: Standard QLTS format from Quectel EC200U */
    {
        const char *line = "+QLTS: \"2026/09/21,00:30:15+28,0\"";
        uint32_t epoch = 0;
        BSP_RTC_DateTime_t local_dt;
        ParseQltsResult_t res = parse_and_validate_qlts(line, false, 0, &epoch, &local_dt);
        assert(res == PARSE_QLTS_SUCCESS);
        assert(epoch > 0);
        /* In UTC+7, 00:30:15 UTC becomes 07:30:15 local */
        assert(local_dt.year == 2026);
        assert(local_dt.month == 9);
        assert(local_dt.day == 21);
        assert(local_dt.hour == 7);
        assert(local_dt.minute == 30);
        assert(local_dt.second == 15);
        printf("[TEST 1 PASS] Standard QLTS format parsed and converted to UTC+7 successfully\n");
    }

    /* TEST 2: Empty QLTS response (modem hasn't received NITZ yet) */
    {
        const char *line = "+QLTS: \"\"";
        uint32_t epoch = 0;
        ParseQltsResult_t res = parse_and_validate_qlts(line, false, 0, &epoch, NULL);
        assert(res == PARSE_QLTS_EMPTY);
        printf("[TEST 2 PASS] Empty QLTS response correctly detected as not ready\n");
    }

    /* TEST 3: Leap year handling (2028-02-29 12:00:00 UTC) */
    {
        const char *line = "+QLTS: \"2028/02/29,12:00:00+28,0\"";
        uint32_t epoch = 0;
        BSP_RTC_DateTime_t local_dt;
        ParseQltsResult_t res = parse_and_validate_qlts(line, false, 0, &epoch, &local_dt);
        assert(res == PARSE_QLTS_SUCCESS);
        assert(local_dt.year == 2028);
        assert(local_dt.month == 2);
        assert(local_dt.day == 29);
        assert(local_dt.hour == 19); /* 12 + 7 = 19 */
        printf("[TEST 3 PASS] Leap year date (Feb 29) handled properly\n");
    }

    /* TEST 4: Out of range detection */
    {
        /* Factory default year 1980 */
        const char *old_year = "+QLTS: \"1980/01/06,00:00:00+00,0\"";
        assert(parse_and_validate_qlts(old_year, false, 0, NULL, NULL) == PARSE_QLTS_OUT_OF_RANGE);

        /* Invalid month 13 */
        const char *bad_month = "+QLTS: \"2026/13/01,10:00:00+28,0\"";
        assert(parse_and_validate_qlts(bad_month, false, 0, NULL, NULL) == PARSE_QLTS_OUT_OF_RANGE);

        /* Invalid day 32 */
        const char *bad_day = "+QLTS: \"2026/05/32,10:00:00+28,0\"";
        assert(parse_and_validate_qlts(bad_day, false, 0, NULL, NULL) == PARSE_QLTS_OUT_OF_RANGE);

        /* Invalid hour 24 */
        const char *bad_hour = "+QLTS: \"2026/05/10,24:00:00+28,0\"";
        assert(parse_and_validate_qlts(bad_hour, false, 0, NULL, NULL) == PARSE_QLTS_OUT_OF_RANGE);

        printf("[TEST 4 PASS] All out-of-range bounds rejected\n");
    }

    /* TEST 5: Anti-rollback protection */
    {
        /* Suppose current RTC is at 2026-09-21 07:30:00 local (UTC epoch ~ 1790037000) */
        BSP_RTC_DateTime_t cur_dt = { .year = 2026, .month = 9, .day = 21, .hour = 0, .minute = 30, .second = 0 };
        uint32_t cur_utc = simulated_DateTimeToEpoch(&cur_dt);

        /* Case 5A: Modem sends time from 1 day in the past (rollback) */
        const char *past_time = "+QLTS: \"2026/09/20,07:30:00+28,0\"";
        ParseQltsResult_t res1 = parse_and_validate_qlts(past_time, true, cur_utc, NULL, NULL);
        assert(res1 == PARSE_QLTS_REJECTED_ROLLBACK);

        /* Case 5B: Modem sends small clock correction (-5 seconds drift compensation) */
        /* cur_utc is at 00:30:00 UTC, network says 00:29:55 UTC (diff = -5s >= -300s) */
        const char *slight_drift = "+QLTS: \"2026/09/21,00:29:55+28,0\"";
        uint32_t drift_epoch = 0;
        ParseQltsResult_t res2 = parse_and_validate_qlts(slight_drift, true, cur_utc, &drift_epoch, NULL);
        assert(res2 == PARSE_QLTS_SUCCESS);
        assert((int32_t)(drift_epoch - cur_utc) == -5);

        /* Case 5C: Forward progression (1 hour later) */
        const char *next_hour = "+QLTS: \"2026/09/21,01:30:00+28,0\"";
        uint32_t next_epoch = 0;
        ParseQltsResult_t res3 = parse_and_validate_qlts(next_hour, true, cur_utc, &next_epoch, NULL);
        assert(res3 == PARSE_QLTS_SUCCESS);
        assert((int32_t)(next_epoch - cur_utc) == 3600);

        printf("[TEST 5 PASS] Anti-rollback protected against backward jumps while allowing drift adjustment\n");
    }

    /* TEST 6: Format variation without trailing DST */
    {
        const char *simple_line = "+QLTS: \"2026/10/15,14:22:33\"";
        uint32_t epoch = 0;
        ParseQltsResult_t res = parse_and_validate_qlts(simple_line, false, 0, &epoch, NULL);
        assert(res == PARSE_QLTS_SUCCESS);
        printf("[TEST 6 PASS] Format variation without trailing fields parsed smoothly\n");
    }

    printf("\n>>> ALL 6 QUECTEL TIME SYNC UNIT TESTS PASSED 100%%! <<<\n");
    return 0;
}
