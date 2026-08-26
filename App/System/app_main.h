/**
 * @file    app_charger.h
 * @brief   Application layer - điều khiển sạc chính
 * @note    Gọi App_Init() sau MX init, gọi App_Loop() trong while(1)
 */

#ifndef APP_MAIN_H
#define APP_MAIN_H

#include <stdint.h>
#include "chg_lib.h"

/** Khởi tạo application (CAN filter + start, Maxwell init, add module) */
void App_Init(void);

/** Main loop processing — gọi liên tục trong while(1) */
void App_Loop(void);

/** CAN RX callback — gọi từ HAL_CAN_RxFifo0MsgPendingCallback */
void App_CAN_RxCallback(void);
void App_CAN2_RxCallback(void);

/** Get current driver ID */
CHG_LIB_DriverId_t App_GetCurrentDriver(void);

#endif /* APP_CHARGER_H */



