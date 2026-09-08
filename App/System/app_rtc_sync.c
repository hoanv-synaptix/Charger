/**
 * @file app_rtc_sync.c
 * @brief Application-owned RTC synchronization orchestration.
 */

#include "app_rtc_sync.h"
#include "bsp_rtc.h"
#include "pc_debug_protocol.h"

static void send_current_rtc_info(void)
{
    DebugRtcInfo_t info = {0};
    BSP_RTC_DateTime_t dt = {0};

    info.epoch_sec = BSP_RTC_GetEpoch();
    if (BSP_RTC_GetDateTime(&dt)) {
        info.year = dt.year;
        info.month = dt.month;
        info.day = dt.day;
        info.hour = dt.hour;
        info.minute = dt.minute;
        info.second = dt.second;
        info.weekday = dt.weekday;
    }
    info.is_valid = BSP_RTC_IsTimeValid() ? 1U : 0U;
    DebugProtocol_SendRtcInfo(&info);
}

void App_RtcSync_Process(void)
{
    uint32_t epoch = 0U;

    if (DebugProtocol_TakeRtcSetRequest(&epoch)) {
        if (BSP_RTC_SetEpoch(epoch)) {
            send_current_rtc_info();
        } else {
            DebugProtocol_SendRtcError(0x02U); /* EXEC_FAIL */
        }
    }

    if (DebugProtocol_TakeRtcGetRequest()) {
        send_current_rtc_info();
    }
}
