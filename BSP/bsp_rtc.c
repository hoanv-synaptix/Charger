/**
 * @file    bsp_rtc.c
 * @brief   BSP layer for STM32G0 internal RTC (Real-Time Clock).
 */
#include "bsp_rtc.h"
#include "debug_log.h"
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

/* Calibration defines for STM32G0 internal LSI RTC:
 * Measured LSI frequency: 32000 * (86400 - 1020) / 86400 ≈ 31622.22 Hz (-11805.6 ppm drift).
 * Stage 1: Adjust prescaler from nominal (128 * 250 = 32000) to (128 * 247 = 31616).
 *          AsynchPrediv = 127, SynchPrediv = 246.
 *          Coarse frequency error = (31622.22 - 31616) / 31616 ≈ +196.805 ppm (+17.0 s/day fast).
 * Stage 2: Smooth calibration in 32-second period (2^20 = 1048576 RTCCLK cycles).
 *          Each CALM pulse masks 1 cycle out of 1048576 cycles (approx 0.953674 ppm).
 *          CALM = 196.805 / 0.953674 ≈ 206 pulses.
 *          Calibration effect: -196.457 ppm.
 *          Residual error: +0.348 ppm (~ +0.03 s/day at room temperature).
 */
#define BSP_RTC_LSE_ASYNCH_PREDIV    127U
#define BSP_RTC_LSE_SYNCH_PREDIV     255U

#define BSP_RTC_CALIB_ASYNCH_PREDIV  127U
#define BSP_RTC_CALIB_SYNCH_PREDIV   246U
#define BSP_RTC_CALIB_CALM_PULSES    206U

static bool rtc_update_prescaler_if_needed(uint32_t asynch, uint32_t synch)
{
    const uint32_t desired_prer = (asynch << RTC_PRER_PREDIV_A_Pos) |
                                  (synch << RTC_PRER_PREDIV_S_Pos);
    const uint32_t prer_mask = RTC_PRER_PREDIV_A | RTC_PRER_PREDIV_S;

    if ((RTC->PRER & prer_mask) == desired_prer) {
        return true;
    }

    LOG("BSP_RTC: Updating prescaler (current PRER=0x%08lX -> desired=0x%08lX)...\r\n",
        (unsigned long)RTC->PRER, (unsigned long)desired_prer);

    /* Note: Entering RTC initialization mode stops the calendar counter and discards
     * sub-second fractional counters. This is a one-time operation during boot when
     * prescaler adjustment is required. */
    __HAL_RTC_WRITEPROTECTION_DISABLE(&s_hrtc);

    if (RTC_EnterInitMode(&s_hrtc) != HAL_OK) {
        __HAL_RTC_WRITEPROTECTION_ENABLE(&s_hrtc);
        LOG("BSP_RTC: Failed to enter init mode for prescaler update\r\n");
        return false;
    }

    RTC->PRER = desired_prer;

    if (RTC_ExitInitMode(&s_hrtc) != HAL_OK) {
        __HAL_RTC_WRITEPROTECTION_ENABLE(&s_hrtc);
        LOG("BSP_RTC: Failed to exit init mode after prescaler update\r\n");
        return false;
    }

    __HAL_RTC_WRITEPROTECTION_ENABLE(&s_hrtc);

    if ((RTC->PRER & prer_mask) != desired_prer) {
        LOG("BSP_RTC: Prescaler readback mismatch (PRER=0x%08lX expected=0x%08lX)\r\n",
            (unsigned long)RTC->PRER, (unsigned long)desired_prer);
        return false;
    }

    LOG("BSP_RTC: Prescaler successfully updated to PRER=0x%08lX\r\n", (unsigned long)RTC->PRER);
    return true;
}

static bool rtc_update_smooth_calib_if_needed(void)
{
    const uint32_t desired_calr = (uint32_t)(RTC_SMOOTHCALIB_PERIOD_32SEC |
                                             RTC_SMOOTHCALIB_PLUSPULSES_RESET |
                                             BSP_RTC_CALIB_CALM_PULSES);
    const uint32_t calr_mask = RTC_CALR_CALP | RTC_CALR_CALW8 | RTC_CALR_CALW16 | RTC_CALR_CALM;

    if ((RTC->CALR & calr_mask) == desired_calr) {
        return true;
    }

    LOG("BSP_RTC: Updating smooth calib (current CALR=0x%08lX -> desired=0x%08lX)...\r\n",
        (unsigned long)RTC->CALR, (unsigned long)desired_calr);

    HAL_StatusTypeDef status = HAL_RTCEx_SetSmoothCalib(&s_hrtc,
                                                        RTC_SMOOTHCALIB_PERIOD_32SEC,
                                                        RTC_SMOOTHCALIB_PLUSPULSES_RESET,
                                                        BSP_RTC_CALIB_CALM_PULSES);
    if (status != HAL_OK) {
        LOG("BSP_RTC: HAL_RTCEx_SetSmoothCalib failed (status=%u)\r\n", (unsigned)status);
        return false;
    }

    if ((RTC->CALR & calr_mask) != desired_calr) {
        LOG("BSP_RTC: CALR readback mismatch (CALR=0x%08lX expected=0x%08lX)\r\n",
            (unsigned long)RTC->CALR, (unsigned long)desired_calr);
        return false;
    }

    LOG("BSP_RTC: Smooth calib successfully updated to CALR=0x%08lX\r\n", (unsigned long)RTC->CALR);
    return true;
}

bool BSP_RTC_Init(void)
{
    if (s_initialized) {
        return true;
    }

    /* 1. Enable PWR peripheral clock and enable access to Backup domain */
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    /* 2. Configure RTC oscillator: Prioritize external 32.768kHz quartz crystal (LSE Y202) */
    bool use_lse = false;
    uint32_t current_rtc_src = __HAL_RCC_GET_RTC_SOURCE();

    if (current_rtc_src == RCC_RTCCLKSOURCE_LSE && __HAL_RCC_GET_FLAG(RCC_FLAG_LSERDY)) {
        use_lse = true;
    } else {
        /* Reset backup domain cleanly FIRST before starting LSE to avoid register lockout */
        __HAL_RCC_BACKUPRESET_FORCE();
        __HAL_RCC_BACKUPRESET_RELEASE();
        HAL_PWR_EnableBkUpAccess();

        /* Set LSE Drive to MEDIUMHIGH for 12.5pF crystal (Y202 + C211/C212 12pF) */
        __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_MEDIUMHIGH);

        RCC_OscInitTypeDef osc_init = {0};
        osc_init.OscillatorType = RCC_OSCILLATORTYPE_LSE;
        osc_init.LSEState = RCC_LSE_ON;
        if (HAL_RCC_OscConfig(&osc_init) == HAL_OK && __HAL_RCC_GET_FLAG(RCC_FLAG_LSERDY)) {
            RCC_PeriphCLKInitTypeDef periph_clk = {0};
            periph_clk.PeriphClockSelection = RCC_PERIPHCLK_RTC;
            periph_clk.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
            if (HAL_RCCEx_PeriphCLKConfig(&periph_clk) == HAL_OK) {
                use_lse = true;
            }
        }
    }

    if (!use_lse) {
        /* Fallback: In case Y202 crystal is not ready or unpopulated, use internal LSI */
        LOG("BSP_RTC: LSE crystal not ready -> Falling back to internal LSI\r\n");

        __HAL_RCC_BACKUPRESET_FORCE();
        __HAL_RCC_BACKUPRESET_RELEASE();
        HAL_PWR_EnableBkUpAccess();

        RCC_OscInitTypeDef osc_init = {0};
        osc_init.OscillatorType = RCC_OSCILLATORTYPE_LSI;
        osc_init.LSIState = RCC_LSI_ON;
        (void)HAL_RCC_OscConfig(&osc_init);

        RCC_PeriphCLKInitTypeDef periph_clk = {0};
        periph_clk.PeriphClockSelection = RCC_PERIPHCLK_RTC;
        periph_clk.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
        (void)HAL_RCCEx_PeriphCLKConfig(&periph_clk);
    }

    /* 4. Enable both the RTC calendar clock and its APB register interface. */
    __HAL_RCC_RTCAPB_CLK_ENABLE();
    __HAL_RCC_RTC_ENABLE();

    /* 5. Initialize RTC handle */
    s_hrtc.Instance = RTC;
    s_hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
    if (use_lse) {
        /* LSE nominal 32.768 kHz:
         * AsynchPrediv=127, SynchPrediv=255 -> total division 128 * 256 = 32768 (exact 1 Hz). */
        s_hrtc.Init.AsynchPrediv = BSP_RTC_LSE_ASYNCH_PREDIV;
        s_hrtc.Init.SynchPrediv = BSP_RTC_LSE_SYNCH_PREDIV;
    } else {
        /* LSI nominal 32 kHz calibrated:
         * AsynchPrediv=127, SynchPrediv=246 -> total division 128 * 247 = 31616. */
        s_hrtc.Init.AsynchPrediv = BSP_RTC_CALIB_ASYNCH_PREDIV;
        s_hrtc.Init.SynchPrediv = BSP_RTC_CALIB_SYNCH_PREDIV;
    }
    s_hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
    s_hrtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
    s_hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
    s_hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
    s_hrtc.Init.OutPutPullUp = RTC_OUTPUT_PULLUP_NONE;

    if (HAL_RTC_Init(&s_hrtc) != HAL_OK) {
        return false;
    }

    /* 6. Ensure prescaler in hardware matches desired values even if calendar was already initialized */
    if (use_lse) {
        if (!rtc_update_prescaler_if_needed(BSP_RTC_LSE_ASYNCH_PREDIV, BSP_RTC_LSE_SYNCH_PREDIV)) {
            return false;
        }
    } else {
        if (!rtc_update_prescaler_if_needed(BSP_RTC_CALIB_ASYNCH_PREDIV, BSP_RTC_CALIB_SYNCH_PREDIV)) {
            return false;
        }
        /* 7. Ensure smooth digital calibration is configured for LSI */
        if (!rtc_update_smooth_calib_if_needed()) {
            return false;
        }
    }

    LOG("BSP_RTC: Configured PRER=0x%08lX CALR=0x%08lX ICSR=0x%08lX\r\n",
        (unsigned long)RTC->PRER, (unsigned long)RTC->CALR, (unsigned long)RTC->ICSR);

    /* Check if the calendar was previously synchronized. */
    uint32_t bkp = HAL_RTCEx_BKUPRead(&s_hrtc, RTC_BKP_DR0);
    LOG("BSP_RTC: backup marker=0x%08lX (%s)\r\n",
        (unsigned long)bkp,
        (bkp == BSP_RTC_MAGIC_VALID) ? "valid" : "invalid");
    if (bkp == BSP_RTC_MAGIC_VALID) {
        s_time_valid = true;
    } else {
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

    HAL_StatusTypeDef hal_status = HAL_RTC_SetTime(&s_hrtc, &sTime, RTC_FORMAT_BIN);
    if (hal_status != HAL_OK) {
        LOG("RTC: HAL_RTC_SetTime failed (status=%u state=%u)\r\n",
            (unsigned)hal_status, (unsigned)HAL_RTC_GetState(&s_hrtc));
        return false;
    }

    sDate.WeekDay = (dt->weekday >= 1U && dt->weekday <= 7U) ? dt->weekday : RTC_WEEKDAY_MONDAY;
    sDate.Month = dt->month;
    sDate.Date = dt->day;
    sDate.Year = (uint8_t)((dt->year >= 2000U) ? (dt->year - 2000U) : 0U);

    hal_status = HAL_RTC_SetDate(&s_hrtc, &sDate, RTC_FORMAT_BIN);
    if (hal_status != HAL_OK) {
        LOG("RTC: HAL_RTC_SetDate failed (status=%u state=%u)\r\n",
            (unsigned)hal_status, (unsigned)HAL_RTC_GetState(&s_hrtc));
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
