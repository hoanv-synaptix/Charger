#ifndef APP_AUTO_START_H
#define APP_AUTO_START_H

#include <stdint.h>
#include <stdbool.h>
#include "charge_cycle_config.h"
#include "bms_core.h"
#include "charge_controller.h"

#define AUTO_START_CAN_ONLINE_DEBOUNCE_MS   15000U
#define AUTO_START_CAN_OFFLINE_RESET_MS    1000U
#define AUTO_START_MODULE_READY_TIMEOUT_MS 30000U  /* 30 seconds grace period for charger module AC power */

typedef struct {
    bool     user_stopped;
    bool     charge_completed;
    bool     fault_latched;
    uint32_t can_online_since;
    uint32_t can_offline_since;
    uint32_t bms_online_no_mod_since; /**< Tick when BMS is online but 0 charger modules are active */
    bool     plug_latched;
    bool     mod_timeout_warning;     /**< True when bms_online_no_mod_since exceeds 30s timeout */
} AutoStartState_t;

void AutoStart_Init(void);
void AutoStart_ResetSession(void);
bool AutoStart_IsBatteryFull(const ChargeCycleConfig_t *cfg, const BMS_View_t *bms);
void AutoStart_Process(uint32_t now);
void AutoStart_OnUserStop(void);
void AutoStart_OnUserStart(void);
void AutoStart_OnUserReset(void);
bool AutoStart_IsChargeCompleted(void);
bool AutoStart_IsModuleTimeout(void);
const AutoStartState_t* AutoStart_GetState(void);

#endif /* APP_AUTO_START_H */
