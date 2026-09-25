#include "charge_cycle_config.h"
#include "chg_lib.h"

#include <string.h>
#include <math.h>

static ChargeCycleConfig_t g_charge_cycle_config;
static ChargeCycleConfig_t s_fast_config;
static ChargeCycleConfig_t s_normal_config;
static uint8_t s_active_mode = CHARGE_MODE_NORMAL;

static bool value_is_invalid(float value)
{
    return !isfinite(value);
}

static bool validate_non_negative(float value)
{
    return !value_is_invalid(value) && value >= 0.0f;
}

static bool validate_range(float value, float min_value, float max_value)
{
    return !value_is_invalid(value) && value >= min_value && value <= max_value;
}

void ChargeCycleConfig_GetDefaults(ChargeCycleConfig_t *config)
{
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->version = CHARGE_CYCLE_CONFIG_VERSION;

    config->charge_source_mode = CHARGE_SOURCE_BMS_CONTROLLED;
    config->module_type = CHARGE_MODULE_TYPE_EVR_10KW_100A_100V;
    config->can_battery_id = 1U;
    config->source_module_count = 1U;
    config->module_address = DEFAULT_MODULE_ADDRESS;

    config->battery_capacity_ah = DEFAULT_BATTERY_CAPACITY_AH;
    config->imin_c = DEFAULT_IMIN_C;
    config->imax_c = DEFAULT_IMAX_C;
    config->ipre_c = DEFAULT_IPRE_C;
    config->ilow_c = DEFAULT_ILOW_C;
    config->vmin_v = DEFAULT_VMIN_V;
    config->vmax_v = DEFAULT_VMAX_V;
    config->vpre_v = DEFAULT_VPRE_V;
    config->vlow_v = DEFAULT_VLOW_V;
    config->temp_limit_c = DEFAULT_TEMP_LIMIT_C;

    config->module_u_min_v = DEFAULT_MODULE_U_MIN_V;
    config->module_u_max_v = DEFAULT_MODULE_U_MAX_V;
    config->module_i_min_a = DEFAULT_MODULE_I_MIN_A;
    config->module_i_max_a = DEFAULT_MODULE_I_MAX_A;
    config->protect_jack_temp_power_limit_pct = DEFAULT_JACK_TEMP_POWER_LIMIT_PCT;
    config->protect_jack_temp_trip_c = DEFAULT_JACK_TEMP_TRIP_C;
    config->admin_pin = DEFAULT_ADMIN_PIN;
    config->charge_mode = DEFAULT_CHARGE_MODE;
    config->delay_enabled = DEFAULT_DELAY_ENABLED;
    config->delay_hours = DEFAULT_DELAY_HOURS;
    config->delay_minutes = DEFAULT_DELAY_MINUTES;
    config->imax_a = DEFAULT_IMAX_A;

    /* strncpy into a memset-0 buffer leaves the field NUL-terminated as long
     * as the literal is shorter than the field, which both are. */
    strncpy(config->device_id, DEFAULT_DEVICE_ID, sizeof(config->device_id) - 1U);
    strncpy(config->hw_rev, DEFAULT_HW_REV, sizeof(config->hw_rev) - 1U);
}

static bool validate_config_struct(const ChargeCycleConfig_t *config)
{
    if (config == NULL) {
        return false;
    }

    if (config->version != CHARGE_CYCLE_CONFIG_VERSION &&
        config->version != 1U && config->version != 2U && config->version != 3U &&
        config->version != 4U && config->version != 5U && config->version != 6U) {
        return false;
    }

    if (!validate_non_negative(config->battery_capacity_ah) ||
        !validate_non_negative(config->imin_c) ||
        !validate_non_negative(config->imax_c) ||
        !validate_non_negative(config->ipre_c) ||
        !validate_non_negative(config->ilow_c) ||
        !validate_non_negative(config->vmin_v) ||
        !validate_non_negative(config->vmax_v) ||
        !validate_non_negative(config->vpre_v) ||
        !validate_non_negative(config->vlow_v) ||
        !validate_range(config->temp_limit_c, -50.0f, 200.0f) ||
        !validate_range(config->cell_volt_delta_t_s, 0.0f, 600.0f) ||
        !validate_non_negative(config->cell_volt_1_v) ||
        !validate_non_negative(config->cell_volt_2_v) ||
        !validate_non_negative(config->cell_volt_3_v) ||
        !validate_non_negative(config->cell_volt_4_v) ||
        !validate_non_negative(config->cell_volt_5_v) ||
        !validate_non_negative(config->cell_curr_1_c) ||
        !validate_non_negative(config->cell_curr_2_c) ||
        !validate_non_negative(config->cell_curr_3_c) ||
        !validate_non_negative(config->cell_curr_4_c) ||
        !validate_non_negative(config->temp_delta_c) ||
        !validate_range(config->temp_1_c, -50.0f, 200.0f) ||
        !validate_range(config->temp_2_c, -50.0f, 200.0f) ||
        !validate_range(config->temp_3_c, -50.0f, 200.0f) ||
        !validate_range(config->temp_4_c, -50.0f, 200.0f) ||
        !validate_range(config->temp_5_c, -50.0f, 200.0f) ||
        !validate_non_negative(config->temp_curr_1_c) ||
        !validate_non_negative(config->temp_curr_2_c) ||
        !validate_non_negative(config->temp_curr_3_c) ||
        !validate_non_negative(config->temp_curr_4_c) ||
        !validate_range(config->soc_delta_t_s, 0.0f, 600.0f) ||
        !validate_range(config->soc_1_pct, 0.0f, 100.0f) ||
        !validate_range(config->soc_2_pct, 0.0f, 100.0f) ||
        !validate_range(config->soc_3_pct, 0.0f, 100.0f) ||
        !validate_range(config->soc_4_pct, 0.0f, 100.0f) ||
        !validate_range(config->soc_5_pct, 0.0f, 100.0f) ||
        !validate_non_negative(config->soc_curr_1_c) ||
        !validate_non_negative(config->soc_curr_2_c) ||
        !validate_non_negative(config->soc_curr_3_c) ||
        !validate_non_negative(config->soc_curr_4_c) ||
        !validate_non_negative(config->protect_jack_charge_delta_v) ||
        !validate_range(config->protect_jack_temp_threshold_c, -50.0f, 200.0f) ||
        !validate_non_negative(config->protect_jack_temp_delta_c) ||
        !validate_range(config->protect_jack_temp_power_limit_pct, 0.0f, 100.0f) ||
        !validate_range(config->protect_jack_temp_trip_c, -50.0f, 200.0f) ||
        !validate_non_negative(config->module_u_min_v) ||
        !validate_non_negative(config->module_u_max_v) ||
        !validate_non_negative(config->module_i_min_a) ||
        !validate_non_negative(config->module_i_max_a) ||
        !validate_non_negative(config->imax_a) ||
        config->admin_pin < 100000U || config->admin_pin > 999999U ||
        config->charge_mode > 1U || config->delay_enabled > 1U ||
        config->delay_hours > 99U || config->delay_minutes > 59U) {
        return false;
    }

    if (config->protect_jack_temp_trip_c < config->protect_jack_temp_threshold_c) {
        return false;
    }

    if (config->imax_c < config->imin_c ||
        config->module_u_max_v < config->module_u_min_v ||
        config->module_i_max_a < config->module_i_min_a ||
        config->soc_2_pct < config->soc_1_pct ||
        config->soc_3_pct < config->soc_2_pct ||
        config->soc_4_pct < config->soc_3_pct ||
        config->soc_5_pct < config->soc_4_pct ||
        config->cell_volt_2_v < config->cell_volt_1_v ||
        config->cell_volt_3_v < config->cell_volt_2_v ||
        config->cell_volt_4_v < config->cell_volt_3_v ||
        config->cell_volt_5_v < config->cell_volt_4_v ||
        config->temp_2_c < config->temp_1_c ||
        config->temp_3_c < config->temp_2_c ||
        config->temp_4_c < config->temp_3_c ||
        config->temp_5_c < config->temp_4_c) {
        return false;
    }

    if (config->cell_volt_enabled &&
        !(config->cell_volt_1_v < config->cell_volt_2_v &&
          config->cell_volt_2_v < config->cell_volt_3_v &&
          config->cell_volt_3_v < config->cell_volt_4_v &&
          config->cell_volt_4_v < config->cell_volt_5_v)) {
        return false;
    }

    if (config->soc_enabled &&
        !(config->soc_1_pct < config->soc_2_pct &&
          config->soc_2_pct < config->soc_3_pct &&
          config->soc_3_pct < config->soc_4_pct &&
          config->soc_4_pct < config->soc_5_pct)) {
        return false;
    }

    if (config->temp_enabled) {
        float gap_12 = config->temp_2_c - config->temp_1_c;
        float gap_23 = config->temp_3_c - config->temp_2_c;
        float gap_34 = config->temp_4_c - config->temp_3_c;
        float gap_45 = config->temp_5_c - config->temp_4_c;
        float min_gap = gap_12;

        if (!(config->temp_1_c < config->temp_2_c &&
              config->temp_2_c < config->temp_3_c &&
              config->temp_3_c < config->temp_4_c &&
              config->temp_4_c < config->temp_5_c)) {
            return false;
        }
        if (gap_23 < min_gap) min_gap = gap_23;
        if (gap_34 < min_gap) min_gap = gap_34;
        if (gap_45 < min_gap) min_gap = gap_45;

        if (config->temp_delta_c > min_gap) {
            return false;
        }
    }

    if (config->source_module_count == 0U || config->source_module_count > 8U) {
        return false;
    }

    if (config->charge_source_mode > CHARGE_SOURCE_STANDALONE_NO_BMS) {
        return false;
    }

    if (config->module_type > CHARGE_MODULE_TYPE_TONHE) {
        return false;
    }

    return true;
}

static void apply_hardware_config(const ChargeCycleConfig_t *config)
{
    if (config == NULL) {
        return;
    }

    CHG_LIB_DriverId_t drv_id = CHG_LIB_DRV_NONE;
    switch (config->module_type) {
        case CHARGE_MODULE_TYPE_MAXWELL:
            drv_id = CHG_LIB_DRV_MAXWELL;
            break;
        case CHARGE_MODULE_TYPE_LIANMING:
            drv_id = CHG_LIB_DRV_LIANMING;
            break;
        case CHARGE_MODULE_TYPE_TONHE:
        case CHARGE_MODULE_TYPE_EVR_10KW_100A_100V:
            drv_id = CHG_LIB_DRV_TONHE;
            break;
        default:
            drv_id = CHG_LIB_DRV_NONE;
            break;
    }

    if (drv_id != CHG_LIB_DRV_NONE) {
        CHG_LIB_SelectDriver(drv_id);
        CHG_LIB_Init();
        uint8_t base_addr = (config->module_address <= 240U) ? config->module_address : DEFAULT_MODULE_ADDRESS;
        for (uint8_t i = 0; i < config->source_module_count; i++) {
            int8_t idx = CHG_LIB_AddModule((uint8_t)(base_addr + i), 0);
            if (idx >= 0 && config->module_i_max_a > 0.0f) {
                CHG_LIB_SetModuleConfig((uint8_t)idx, config->module_i_max_a);
            }
        }
    }
}

void ChargeCycleConfig_Init(void)
{
    ChargeCycleConfig_GetDefaults(&s_fast_config);
    s_fast_config.charge_mode = CHARGE_MODE_FAST;
    s_fast_config.delay_enabled = 0U;

    ChargeCycleConfig_GetDefaults(&s_normal_config);
    s_normal_config.charge_mode = CHARGE_MODE_NORMAL;
    s_normal_config.delay_enabled = 0U;
    s_normal_config.imax_c = 0.5f; /* Sensible default distinction for normal mode */
    s_normal_config.imax_a = 50.0f;

    s_active_mode = CHARGE_MODE_NORMAL;
    g_charge_cycle_config = s_normal_config;
}

void ChargeCycleConfig_Get(ChargeCycleConfig_t *config)
{
    if (config == NULL) {
        return;
    }
    *config = (s_active_mode == CHARGE_MODE_NORMAL) ? s_normal_config : s_fast_config;
}

void ChargeCycleConfig_GetProfile(uint8_t mode, ChargeCycleConfig_t *config)
{
    if (config == NULL) {
        return;
    }
    *config = (mode == CHARGE_MODE_NORMAL) ? s_normal_config : s_fast_config;
}

bool ChargeCycleConfig_SetProfile(uint8_t mode, const ChargeCycleConfig_t *config)
{
    if (config == NULL || !validate_config_struct(config)) {
        return false;
    }

    ChargeCycleConfig_t *dest = (mode == CHARGE_MODE_NORMAL) ? &s_normal_config : &s_fast_config;
    *dest = *config;
    dest->version = CHARGE_CYCLE_CONFIG_VERSION;
    dest->charge_mode = (mode == CHARGE_MODE_NORMAL) ? CHARGE_MODE_NORMAL : CHARGE_MODE_FAST;
    dest->device_id[sizeof(dest->device_id) - 1U] = '\0';
    dest->hw_rev[sizeof(dest->hw_rev) - 1U] = '\0';

    if (dest->device_id[0] == '\0') {
        strncpy(dest->device_id, DEFAULT_DEVICE_ID, sizeof(dest->device_id) - 1U);
    }
    if (dest->hw_rev[0] == '\0') {
        strncpy(dest->hw_rev, DEFAULT_HW_REV, sizeof(dest->hw_rev) - 1U);
    }

    if (mode == s_active_mode) {
        g_charge_cycle_config = *dest;
        apply_hardware_config(dest);
    }
    return true;
}

bool ChargeCycleConfig_Set(const ChargeCycleConfig_t *config)
{
    if (config == NULL) {
        return false;
    }
    uint8_t mode = (config->charge_mode <= 1U) ? config->charge_mode : s_active_mode;
    bool ok = ChargeCycleConfig_SetProfile(mode, config);
    if (ok) {
        if (mode != s_active_mode) {
            s_active_mode = mode;
            g_charge_cycle_config = (s_active_mode == CHARGE_MODE_NORMAL) ? s_normal_config : s_fast_config;
            apply_hardware_config(&g_charge_cycle_config);
        }
    }
    return ok;
}

uint8_t ChargeCycleConfig_GetActiveMode(void)
{
    return s_active_mode;
}

bool ChargeCycleConfig_SetActiveMode(uint8_t mode)
{
    if (mode > 1U) {
        return false;
    }
    s_active_mode = mode;
    g_charge_cycle_config = (s_active_mode == CHARGE_MODE_NORMAL) ? s_normal_config : s_fast_config;
    for (uint8_t i = 0; i < g_charge_cycle_config.source_module_count; i++) {
        if (g_charge_cycle_config.module_i_max_a > 0.0f) {
            CHG_LIB_SetModuleConfig(i, g_charge_cycle_config.module_i_max_a);
        }
    }
    return true;
}

void ChargeCycleConfig_ResetSessionDefaults(void)
{
    /* Always revert active mode to NORMAL and clear delay_enabled for a new session,
     * while preserving delay_hours and delay_minutes (Option A). */
    s_normal_config.delay_enabled = 0U;
    s_fast_config.delay_enabled = 0U;
    s_active_mode = CHARGE_MODE_NORMAL;
    g_charge_cycle_config = s_normal_config;
    for (uint8_t i = 0; i < g_charge_cycle_config.source_module_count; i++) {
        if (g_charge_cycle_config.module_i_max_a > 0.0f) {
            CHG_LIB_SetModuleConfig(i, g_charge_cycle_config.module_i_max_a);
        }
    }
}

const char *ChargeCycleConfig_GetDeviceId(void)
{
    if (g_charge_cycle_config.device_id[0] == '\0') {
        return DEFAULT_DEVICE_ID;
    }
    return g_charge_cycle_config.device_id;
}

const char *ChargeCycleConfig_GetHwRev(void)
{
    if (g_charge_cycle_config.hw_rev[0] == '\0') {
        return DEFAULT_HW_REV;
    }
    return g_charge_cycle_config.hw_rev;
}
