/**
 * @file    dwin_alarm_text.h
 * @brief   Vietnamese Unicode (UTF-16BE) descriptions and code strings for DWIN HMI.
 */
#ifndef APP_ALARM_DWIN_ALARM_TEXT_H
#define APP_ALARM_DWIN_ALARM_TEXT_H

#include <stdint.h>
#include "alarm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Get the short code string (e.g. "E006", "W002", "0000") for an alarm.
 * @param  code   AlarmCode_t
 * @return Null-terminated ASCII string (max 8 chars).
 */
const char* DWIN_Alarm_GetCodeString(AlarmCode_t code);

/**
 * @brief  Get the Vietnamese Unicode (UTF-16) description string for an alarm.
 * @param  code     AlarmCode_t
 * @param  out_len  Optional output for number of characters (not including NULL).
 * @return Pointer to 16-bit Unicode character array, NULL-terminated.
 */
const uint16_t* DWIN_Alarm_GetDescUtf16(AlarmCode_t code, uint8_t *out_len);

/**
 * @brief  Format Vietnamese Unicode (UTF-16) description with optional [M1], [M2] suffix.
 * @param  raw_code   Raw 16-bit code from AlarmLogEntry_t (encoded with source_id)
 * @param  out_buf    Output buffer (at least 34 uint16_t elements)
 * @param  out_len    Output number of UTF-16 code units (excluding NUL)
 * @return Pointer to out_buf (NULL-terminated UTF-16 string)
 */
const uint16_t* DWIN_Alarm_FormatDescUtf16(uint16_t raw_code, uint16_t *out_buf, uint8_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* APP_ALARM_DWIN_ALARM_TEXT_H */
