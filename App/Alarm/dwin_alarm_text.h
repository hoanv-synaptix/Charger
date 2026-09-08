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

#ifdef __cplusplus
}
#endif

#endif /* APP_ALARM_DWIN_ALARM_TEXT_H */
