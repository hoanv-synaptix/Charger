/**
 * @file    bsp_rtc.h
 * @brief   BSP layer for STM32G0 internal RTC (Real-Time Clock).
 */
#ifndef BSP_RTC_H
#define BSP_RTC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_RTC_MAGIC_VALID      0x5A5A3201U
#define BSP_RTC_TIMEZONE_HOURS   (+7)  /* Vietnam Standard Time (ICT, UTC+7) */
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

/**
 * @brief Initialize the RTC peripheral and clock source (LSI).
 *        If backup register indicates valid time, preserves existing clock.
 * @return true if initialization succeeded.
 */
bool BSP_RTC_Init(void);

/**
 * @brief Check if the real-time clock has been synchronized at least once.
 */
bool BSP_RTC_IsTimeValid(void);

/**
 * @brief Set RTC calendar date and time. Marks clock as valid in backup register.
 */
bool BSP_RTC_SetDateTime(const BSP_RTC_DateTime_t *dt);

/**
 * @brief Get current calendar date and time from RTC.
 */
bool BSP_RTC_GetDateTime(BSP_RTC_DateTime_t *dt);

/**
 * @brief Set RTC from Unix epoch timestamp (seconds since 1970-01-01 00:00:00 UTC).
 */
bool BSP_RTC_SetEpoch(uint32_t epoch);

/**
 * @brief Get current RTC time as Unix epoch timestamp.
 */
uint32_t BSP_RTC_GetEpoch(void);

/**
 * @brief Format current time as "HH:MM:SS" into @p buf.
 */
void BSP_RTC_FormatTime(char *buf, size_t buf_size);

/**
 * @brief Format current date & time as "YYYY-MM-DD HH:MM:SS" into @p buf.
 */
void BSP_RTC_FormatDateTime(char *buf, size_t buf_size);

/**
 * @brief Convert broken-down datetime to Unix epoch seconds.
 */
uint32_t BSP_RTC_DateTimeToEpoch(const BSP_RTC_DateTime_t *dt);

/**
 * @brief Convert Unix epoch seconds to broken-down datetime.
 */
void BSP_RTC_EpochToDateTime(uint32_t epoch, BSP_RTC_DateTime_t *dt);

#ifdef __cplusplus
}
#endif

#endif /* BSP_RTC_H */
