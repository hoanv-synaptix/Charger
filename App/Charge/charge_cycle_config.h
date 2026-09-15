#ifndef CHARGE_CYCLE_CONFIG_H
#define CHARGE_CYCLE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include "app_version.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHARGE_CYCLE_CONFIG_VERSION 7U

/* Fixed-width identity strings shown on the DWIN "Setting" screen
 * (VP_SET_HW_VER / VP_SET_DEVICE_ID). Always NUL-terminated on load. */
#define CHARGE_CYCLE_DEVICE_ID_LEN 16U
#define CHARGE_CYCLE_HW_REV_LEN    12U

/* ===================== Default Configuration Values ===================== */
#define DEFAULT_HW_REV                        HW_VERSION_STRING

#define DEFAULT_BATTERY_CAPACITY_AH           100.0f
#define DEFAULT_IMIN_C                        0.1f
#define DEFAULT_IMAX_C                        1.0f
#define DEFAULT_IPRE_C                        0.2f
#define DEFAULT_ILOW_C                        0.5f

#define DEFAULT_VMIN_V                        32.0f
#define DEFAULT_VMAX_V                        58.4f
#define DEFAULT_VPRE_V                        48.0f
#define DEFAULT_VLOW_V                        52.0f
#define DEFAULT_TEMP_LIMIT_C                  55.0f

#define DEFAULT_MODULE_U_MIN_V                30.0f
#define DEFAULT_MODULE_U_MAX_V                99.0f
#define DEFAULT_MODULE_I_MIN_A                5.0f
#define DEFAULT_MODULE_I_MAX_A                100.0f
#define DEFAULT_JACK_TEMP_POWER_LIMIT_PCT     80.0f
#define DEFAULT_JACK_TEMP_TRIP_C              75.0f
#define DEFAULT_ADMIN_PIN                     123456U
#define DEFAULT_CHARGE_MODE                   0U /* 0: FAST, 1: NORMAL */
#define DEFAULT_DELAY_ENABLED                 0U /* 0: OFF, 1: ON */
#define DEFAULT_DELAY_HOURS                   2U
#define DEFAULT_DELAY_MINUTES                 30U

typedef enum {
    CHARGE_MODULE_TYPE_UNKNOWN = 0,
    CHARGE_MODULE_TYPE_EVR_10KW_100A_100V = 1,
    CHARGE_MODULE_TYPE_MAXWELL = 2,
    CHARGE_MODULE_TYPE_LIANMING = 3,
    CHARGE_MODULE_TYPE_TONHE = 4,
} ChargeModuleType_t;

typedef enum {
    CHARGE_SOURCE_BMS_CONTROLLED = 0,
    CHARGE_SOURCE_STANDALONE_NO_BMS = 1,
} ChargeSourceMode_t;

typedef struct __attribute__((packed)) {
    uint16_t version;

    float battery_capacity_ah;
    float imin_c;
    float imax_c;
    float ipre_c;
    float ilow_c;
    float vmin_v;
    float vmax_v;
    float vpre_v;
    float vlow_v;
    float temp_limit_c;

    uint8_t cell_volt_enabled;
    float cell_volt_delta_t_s;
    float cell_volt_1_v;
    float cell_volt_2_v;
    float cell_volt_3_v;
    float cell_volt_4_v;
    float cell_volt_5_v;
    float cell_curr_1_c;
    float cell_curr_2_c;
    float cell_curr_3_c;
    float cell_curr_4_c;

    uint8_t temp_enabled;
    float temp_delta_c;
    float temp_1_c;
    float temp_2_c;
    float temp_3_c;
    float temp_4_c;
    float temp_5_c;
    float temp_curr_1_c;
    float temp_curr_2_c;
    float temp_curr_3_c;
    float temp_curr_4_c;

    uint8_t soc_enabled;
    float soc_delta_t_s;
    float soc_1_pct;
    float soc_2_pct;
    float soc_3_pct;
    float soc_4_pct;
    float soc_5_pct;
    float soc_curr_1_c;
    float soc_curr_2_c;
    float soc_curr_3_c;
    float soc_curr_4_c;

    uint8_t protect_jack_charge_enabled;
    float protect_jack_charge_delta_v;
    uint16_t protect_jack_charge_delay_s;

    uint8_t protect_jack_temp_enabled;
    uint16_t protect_jack_temp_delay_s;
    float protect_jack_temp_threshold_c;
    float protect_jack_temp_delta_c;
    float protect_jack_temp_power_limit_pct;
    float protect_jack_temp_trip_c;

    uint8_t charge_source_mode;
    uint8_t can_battery_id;
    uint8_t source_module_count;
    uint8_t module_type;
    float module_u_min_v;
    float module_u_max_v;
    float module_i_min_a;
    float module_i_max_a;

    /* v4: device identity strings for the DWIN Setting screen. Appended at
     * the end so the on-flash layout of every prior field is unchanged.
     * Growing the struct changes sizeof(), which the storage layer keys its
     * record-validity check on (charge_cycle_storage.c validate_record):
     * records written by v<=3 firmware are silently rejected on the first
     * boot of v4 and the defaults are loaded instead -- a deliberate,
     * one-time reset of the saved charge parameters, accepted with the user
     * 2026-08-30. Re-save from the PC app after upgrading. */
    char device_id[CHARGE_CYCLE_DEVICE_ID_LEN];
    char hw_rev[CHARGE_CYCLE_HW_REV_LEN];
    uint32_t admin_pin;

    /* v7: DWIN Charge Mode & Delay Start configuration */
    uint8_t  charge_mode;     /* 0: FAST (default), 1: NORMAL */
    uint8_t  delay_enabled;   /* 0: OFF (default), 1: ON */
    uint16_t delay_hours;     /* 0..99 (default: 2) */
    uint16_t delay_minutes;   /* 0..59 (default: 30) */
} ChargeCycleConfig_t;

_Static_assert(sizeof(ChargeCycleConfig_t) == 249, "ChargeCycleConfig_t must stay 249 bytes");

#define CHARGE_MODE_FAST                      0U
#define CHARGE_MODE_NORMAL                    1U

void ChargeCycleConfig_Init(void);
void ChargeCycleConfig_Get(ChargeCycleConfig_t *config);
bool ChargeCycleConfig_Set(const ChargeCycleConfig_t *config);
void ChargeCycleConfig_GetDefaults(ChargeCycleConfig_t *config);

void ChargeCycleConfig_GetProfile(uint8_t mode, ChargeCycleConfig_t *config);
bool ChargeCycleConfig_SetProfile(uint8_t mode, const ChargeCycleConfig_t *config);
uint8_t ChargeCycleConfig_GetActiveMode(void);
bool ChargeCycleConfig_SetActiveMode(uint8_t mode);

/** Identity strings (always NUL-terminated). Never NULL. */
const char *ChargeCycleConfig_GetDeviceId(void);
const char *ChargeCycleConfig_GetHwRev(void);

#ifdef __cplusplus
}
#endif

#endif /* CHARGE_CYCLE_CONFIG_H */

