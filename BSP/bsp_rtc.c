/**
 * @file    bsp_rtc.c
 * @brief   BSP layer for STM32G0 internal RTC (Real-Time Clock).
 */
#include "bsp_rtc.h"
#include "stm32g0xx_hal.h"
#include <stdio.h>
#include <string.h>

static RTC_HandleTypeDef s_hrtc;
static bool s_time_valid = false;
static bool s_initialized = false;

static const uint16_t s_days_before_month[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

static inline bool is_leap_year(uint16_t y)
{
    return ((y % 4U == 0U && y % 100U != 0U) || (y % 400U == 0U));
}

uint32_t BSP_RTC_DateTimeToEpoch(const BSP_RTC_DateTime_t *dt)
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

void BSP_RTC_EpochToDateTime(uint32_t epoch, BSP_RTC_DateTime_t *dt)
{
    if (dt == NULL) {
        return;
    }

    uint32_t sec = epoch % 60U;
    epoch /= 60U;
    uint32_t min = epoch % 60U;
    epoch /= 60U;
    uint32_t hour = epoch % 24U;
    uint32_t days = epoch / 24U;

    /* 1970-01-01 was Thursday (4). Monday=1 -> (days + 3) % 7 + 1 */
    dt->weekday = (uint8_t)((days + 3U) % 7U + 1U);

    uint16_t y = 1970U;
    while (1) {
        uint16_t diy = is_leap_year(y) ? 366U : 365U;
        if (days < diy) {
            break;
        }
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

        if (days < dim) {
            break;
        }
        days -= dim;
    }
    dt->month = m;
    dt->day = (uint8_t)(days + 1U);
    dt->hour = (uint8_t)hour;
    dt->minute = (uint8_t)min;
    dt->second = (uint8_t)sec;
}

bool BSP_RTC_Init(void)
{
    if (s_initialized) {
        return true;
    }

    /* 1. Enable PWR peripheral clock and enable access to Backup domain */
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    /* 2. Configure LSI oscillator if not already ON */
    RCC_OscInitTypeDef osc_init = {0};
    osc_init.OscillatorType = RCC_OSCILLATORTYPE_LSI;
    osc_init.LSIState = RCC_LSI_ON;
    if (HAL_RCC_OscConfig(&osc_init) != HAL_OK) {
        return false;
    }

    /* 3. Configure RTC clock source: LSI */
    RCC_PeriphCLKInitTypeDef periph_clk = {0};
    periph_clk.PeriphClockSelection = RCC_PERIPHCLK_RTC;
    periph_clk.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
    if (HAL_RCCEx_PeriphCLKConfig(&periph_clk) != HAL_OK) {
        return false;
    }

    /* 4. Enable RTC peripheral clock */
    __HAL_RCC_RTC_ENABLE();

    /* 5. Initialize RTC handle */
    s_hrtc.Instance = RTC;
    s_hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
    /* LSI nominal 32 kHz: AsynchPrediv=127, SynchPrediv=249 -> (127+1)*(249+1) = 32000 */
    s_hrtc.Init.AsynchPrediv = 127U;
    s_hrtc.Init.SynchPrediv = 249U;
    s_hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
    s_hrtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
    s_hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
    s_hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
    s_hrtc.Init.OutPutPullUp = RTC_OUTPUT_PULLUP_NONE;

    /* Check if already initialized in backup register */
    uint32_t bkp = HAL_RTCEx_BKUPRead(&s_hrtc, RTC_BKP_DR0);
    if (bkp == BSP_RTC_MAGIC_VALID) {
        s_time_valid = true;
    } else {
        /* Cold boot / uninitialized: initialize RTC hardware */
        if (HAL_RTC_Init(&s_hrtc) != HAL_OK) {
            return false;
        }

        /* Set default date 2026-01-01 00:00:00 */
        BSP_RTC_DateTime_t dt_def = {
            .year = 2026U, .month = 1U, .day = 1U,
            .hour = 0U, .minute = 0U, .second = 0U,
            .weekday = 4U
        };
        (void)BSP_RTC_SetDateTime(&dt_def);
        /* Mark valid false until explicitly synced from external source */
        s_time_valid = false;
        HAL_RTCEx_BKUPWrite(&s_hrtc, RTC_BKP_DR0, 0U);
    }

    s_initialized = true;
    return true;
}

bool BSP_RTC_IsTimeValid(void)
{
    return s_time_valid;
}

bool BSP_RTC_SetDateTime(const BSP_RTC_DateTime_t *dt)
{
    if (dt == NULL || dt->year < 2000U || dt->year > 2099U ||
        dt->month < 1U || dt->month > 12U || dt->day < 1U || dt->day > 31U ||
        dt->hour > 23U || dt->minute > 59U || dt->second > 59U) {
        return false;
    }

    RTC_TimeTypeDef sTime = {0};
    RTC_DateTypeDef sDate = {0};

    sTime.Hours = dt->hour;
    sTime.Minutes = dt->minute;
    sTime.Seconds = dt->second;
    sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    sTime.StoreOperation = RTC_STOREOPERATION_RESET;

    if (HAL_RTC_SetTime(&s_hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK) {
        return false;
    }

    sDate.WeekDay = (dt->weekday >= 1U && dt->weekday <= 7U) ? dt->weekday : RTC_WEEKDAY_MONDAY;
    sDate.Month = dt->month;
    sDate.Date = dt->day;
    sDate.Year = (uint8_t)((dt->year >= 2000U) ? (dt->year - 2000U) : 0U);

    if (HAL_RTC_SetDate(&s_hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK) {
        return false;
    }

    s_time_valid = true;
    HAL_RTCEx_BKUPWrite(&s_hrtc, RTC_BKP_DR0, BSP_RTC_MAGIC_VALID);
    return true;
}

bool BSP_RTC_GetDateTime(BSP_RTC_DateTime_t *dt)
{
    if (dt == NULL || !s_initialized) {
        return false;
    }

    RTC_TimeTypeDef sTime = {0};
    RTC_DateTypeDef sDate = {0};

    /* Note: HAL requires GetTime() followed by GetDate() to unlock shadow registers */
    if (HAL_RTC_GetTime(&s_hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK) {
        return false;
    }
    if (HAL_RTC_GetDate(&s_hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK) {
        return false;
    }

    dt->year = (uint16_t)sDate.Year + 2000U;
    dt->month = sDate.Month;
    dt->day = sDate.Date;
    dt->weekday = sDate.WeekDay;

    dt->hour = sTime.Hours;
    dt->minute = sTime.Minutes;
    dt->second = sTime.Seconds;

    return true;
}

bool BSP_RTC_SetEpoch(uint32_t epoch)
{
    BSP_RTC_DateTime_t dt;
    /* Convert incoming standard UTC epoch to local Vietnam time (UTC+7) */
    if (epoch > (UINT32_MAX - (uint32_t)BSP_RTC_TIMEZONE_SEC)) {
        return false;
    }
    uint32_t local_epoch = epoch + (uint32_t)BSP_RTC_TIMEZONE_SEC;
    BSP_RTC_EpochToDateTime(local_epoch, &dt);
    if (dt.year < 2000U || dt.year > 2099U) {
        return false;
    }
    return BSP_RTC_SetDateTime(&dt);
}

uint32_t BSP_RTC_GetEpoch(void)
{
    BSP_RTC_DateTime_t dt;
    if (!BSP_RTC_GetDateTime(&dt)) {
        return 0U;
    }
    uint32_t local_epoch = BSP_RTC_DateTimeToEpoch(&dt);
    if (local_epoch < (uint32_t)BSP_RTC_TIMEZONE_SEC) {
        return local_epoch;
    }
    return local_epoch - (uint32_t)BSP_RTC_TIMEZONE_SEC;
}

void BSP_RTC_FormatTime(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0U) {
        return;
    }

    BSP_RTC_DateTime_t dt;
    if (BSP_RTC_GetDateTime(&dt)) {
        (void)snprintf(buf, buf_size, "%02u:%02u:%02u",
                       (unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second);
    } else {
        (void)snprintf(buf, buf_size, "--:--:--");
    }
}

void BSP_RTC_FormatDateTime(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0U) {
        return;
    }

    BSP_RTC_DateTime_t dt;
    if (BSP_RTC_GetDateTime(&dt)) {
        (void)snprintf(buf, buf_size, "%04u-%02u-%02u %02u:%02u:%02u",
                       (unsigned)dt.year, (unsigned)dt.month, (unsigned)dt.day,
                       (unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second);
    } else {
        (void)snprintf(buf, buf_size, "----/--/-- --:--:--");
    }
}
