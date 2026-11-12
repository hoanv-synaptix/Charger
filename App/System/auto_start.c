#include "auto_start.h"
#include "alarm.h"
#include "debug_log.h"
#include <stddef.h>

static AutoStartState_t s_w4 = {0};

void AutoStart_Init(void)
{
    AutoStart_ResetSession();
}

void AutoStart_ResetSession(void)
{
    s_w4.user_stopped = false;
    s_w4.charge_completed = false;
    s_w4.fault_latched = false;
    s_w4.can_online_since = 0U;
    s_w4.can_offline_since = 0U;
    s_w4.bms_online_no_mod_since = 0U;
    s_w4.plug_latched = false;
    s_w4.mod_timeout_warning = false;
    ChargeController_SetAutoStartModuleTimeout(false);
}

bool AutoStart_IsBatteryFull(const ChargeCycleConfig_t *cfg, const BMS_View_t *bms)
{
    if (cfg == NULL || bms == NULL) {
        return false;
    }
    /* When cell_volt_enabled is active, Cell Voltage is the master completion trigger:
     * do not mark full on SOC if cell voltage has not reached level 5. */
    if (cfg->cell_volt_enabled) {
        float cell_v = (float)bms->max_cell_volt / 1000.0f;
        if (cell_v >= cfg->cell_volt_5_v && cfg->cell_volt_5_v > 0.0f) {
            return true;
        }
    } else if (cfg->soc_enabled) {
        if ((float)bms->soc >= cfg->soc_5_pct && cfg->soc_5_pct > 0.0f) {
            return true;
        }
    }
    if (bms->batt_voltage > 0.0f && cfg->vmax_v > 0.0f) {
        if (bms->batt_voltage >= cfg->vmax_v) {
            return true;
        }
    }
    return false;
}

void AutoStart_OnUserStop(void)
{
    s_w4.user_stopped = true;
}

void AutoStart_OnUserStart(void)
{
    s_w4.user_stopped = false;
    s_w4.charge_completed = false;
    s_w4.fault_latched = false;
}

void AutoStart_OnUserReset(void)
{
    s_w4.charge_completed = false;
    s_w4.fault_latched = false;
    s_w4.user_stopped = true;
}

bool AutoStart_IsChargeCompleted(void)
{
    return s_w4.charge_completed;
}

bool AutoStart_IsModuleTimeout(void)
{
    return s_w4.mod_timeout_warning;
}

const AutoStartState_t* AutoStart_GetState(void)
{
    return &s_w4;
}

void AutoStart_Process(uint32_t now)
{
    BMS_View_t bms;
    BMS_GetView(&bms);

    /* 1. Track CAN connection / disconnection */
    if (bms.online) {
        s_w4.can_offline_since = 0U;
        if (s_w4.can_online_since == 0U) {
            s_w4.can_online_since = now;
        } else if (!s_w4.plug_latched &&
                   (uint32_t)(now - s_w4.can_online_since) >= AUTO_START_CAN_ONLINE_DEBOUNCE_MS) {
            s_w4.plug_latched = true;
            LOG("AutoStart: Plug latched (CAN stable 15s)\r\n");
        }
    } else {
        s_w4.can_online_since = 0U;
        s_w4.plug_latched = false;
        s_w4.bms_online_no_mod_since = 0U;
        s_w4.mod_timeout_warning = false;
        ChargeController_SetAutoStartModuleTimeout(false);
        if (s_w4.can_offline_since == 0U) {
            s_w4.can_offline_since = now;
        } else if ((uint32_t)(now - s_w4.can_offline_since) >= AUTO_START_CAN_OFFLINE_RESET_MS) {
            if (s_w4.user_stopped || s_w4.charge_completed || s_w4.fault_latched) {
                LOG("AutoStart: Session reset (CAN offline confirmed)\r\n");
            }
            s_w4.user_stopped = false;
            s_w4.charge_completed = false;
            s_w4.fault_latched = false;
        }
        /* When cable is unplugged/BMS offline, do not evaluate runtime controller faults */
        return;
    }

    /* Track charger module power readiness (Two-Way Handshake) */
    uint8_t active_mods = ChargeController_GetActiveModuleCount();
    if (active_mods == 0U) {
        if (s_w4.bms_online_no_mod_since == 0U) {
            s_w4.bms_online_no_mod_since = now;
        } else if (!s_w4.mod_timeout_warning &&
                   (uint32_t)(now - s_w4.bms_online_no_mod_since) >= AUTO_START_MODULE_READY_TIMEOUT_MS) {
            s_w4.mod_timeout_warning = true;
            ChargeController_SetAutoStartModuleTimeout(true);
            LOG("AutoStart: Module ready timeout (30s) -> Warning E027 active\r\n");
        }
    } else {
        s_w4.bms_online_no_mod_since = 0U;
        if (s_w4.mod_timeout_warning) {
            s_w4.mod_timeout_warning = false;
            ChargeController_SetAutoStartModuleTimeout(false);
            LOG("AutoStart: Module power recovered -> Warning E027 cleared\r\n");
        }
    }

    /* 2. Track completion & fault lockout from ChargeController */
    ChargeCtrlView_t cc;
    ChargeController_GetView(&cc);
    if (cc.stop_reason == CHARGE_STOP_VOLTAGE_REACHED ||
        cc.stop_reason == CHARGE_STOP_CELL_VOLTAGE_REACHED ||
        cc.stop_reason == CHARGE_STOP_SOC_REACHED) {
        s_w4.charge_completed = true;
    }

    /* Lockout AutoStart on any runtime fault/protection stop to prevent oscillation/chattering.
     * Note: cc.stop_reason == CHARGE_STOP_BMS_OFFLINE indicates cable disconnect, which must NOT
     * lock out the next vehicle plug-in session. */
    if (cc.stop_reason == CHARGE_STOP_PROTECTION ||
        cc.stop_reason == CHARGE_STOP_BMS_ALARM ||
        cc.stop_reason == CHARGE_STOP_MODULE_FAULT ||
        cc.stop_reason == CHARGE_STOP_MODULE_TIMEOUT ||
        cc.stop_reason == CHARGE_STOP_EMERGENCY ||
        (cc.state == CHARGE_CTRL_STATE_FAULT &&
         cc.stop_reason != CHARGE_STOP_BMS_OFFLINE &&
         cc.stop_reason != CHARGE_STOP_NONE)) {
        if (!s_w4.fault_latched) {
            s_w4.fault_latched = true;
            LOG("AutoStart: Fault detected (stop_reason=%u, state=%d) -> Lockout engaged\r\n",
                (unsigned)cc.stop_reason, (int)cc.state);
        }
    }

    /* 3. If auto start is disabled or plug not latched, nothing to do */
    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);
    if (cfg.auto_start_enabled == 0U || !s_w4.plug_latched) {
        return;
    }

    /* 3b. Two-Way Handshake: Do not attempt start if charger modules have no AC power.
     * Keep controller in safe IDLE standby waiting for module power. */
    if (active_mods == 0U) {
        return;
    }

    /* 4. Check if session was stopped, completed, or faulted */
    if (s_w4.user_stopped || s_w4.charge_completed || s_w4.fault_latched) {
        return;
    }

    /* 5. Check if battery is already full (e.g. cold boot / AC recovery with full battery) */
    if (AutoStart_IsBatteryFull(&cfg, &bms)) {
        s_w4.charge_completed = true;
        LOG("AutoStart: Battery already full -> latched COMPLETE\r\n");
        return;
    }

    /* 6. Check system alarms & preconditions */
    AlarmView_t av;
    Alarm_GetView(&av);
    if (av.highest_action >= ALARM_ACT_STOP) {
        return;
    }

    const BMS_AlarmFlag_t hard_bms_alarms =
        (BMS_ALARM_OVER_CHG_CURR | BMS_ALARM_HIGH_CELL_VOLT |
         BMS_ALARM_HIGH_PACK_VOLT | BMS_ALARM_TEMP_LOW_CHG);
    if ((bms.alarm_flags & hard_bms_alarms) != 0U) {
        return;
    }

    /* 7. If controller was left in FAULT due to cable disconnect (BMS offline), clear it now that plug is latched */
    if (cc.state == CHARGE_CTRL_STATE_FAULT && s_w4.plug_latched && !s_w4.fault_latched) {
        if (ChargeController_ResetFaultIfSafe(now)) {
            Alarm_Acknowledge(now);
            ChargeController_GetView(&cc);
        } else {
            return;
        }
    }

    /* 8. If in IDLE and ready, start charge */
    if (cc.state == CHARGE_CTRL_STATE_IDLE) {
        LOG("AutoStart: Triggering ChargeController_Start()\r\n");
        (void)ChargeController_Start(CHARGE_CTRL_OWNER_DWIN, false, now);
    }
}
