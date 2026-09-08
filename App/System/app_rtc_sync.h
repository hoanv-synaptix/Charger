/**
 * @file app_rtc_sync.h
 * @brief Application-owned RTC request processing.
 */

#ifndef APP_RTC_SYNC_H
#define APP_RTC_SYNC_H

/** Apply pending PC RTC requests from main-loop context. */
void App_RtcSync_Process(void);

#endif /* APP_RTC_SYNC_H */
