#ifndef CHARGE_CYCLE_CONFIG_H
#define CHARGE_CYCLE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHARGE_CYCLE_CONFIG_VERSION 3U

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
    float cell_volt_delta_v;
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
    float soc_delta_pct;
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

    uint8_t charge_source_mode;
    uint8_t can_battery_id;
    uint8_t source_module_count;
    uint8_t module_type;
    float module_u_min_v;
    float module_u_max_v;
    float module_i_min_a;
    float module_i_max_a;
} ChargeCycleConfig_t;

_Static_assert(sizeof(ChargeCycleConfig_t) == 207, "ChargeCycleConfig_t must stay 207 bytes");

void ChargeCycleConfig_Init(void);
void ChargeCycleConfig_Get(ChargeCycleConfig_t *config);
bool ChargeCycleConfig_Set(const ChargeCycleConfig_t *config);
void ChargeCycleConfig_GetDefaults(ChargeCycleConfig_t *config);

#ifdef __cplusplus
}
#endif

#endif /* CHARGE_CYCLE_CONFIG_H */

