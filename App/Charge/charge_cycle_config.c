#include "charge_cycle_config.h"
#include "chg_lib.h"

#include <string.h>
#include <math.h>

static ChargeCycleConfig_t g_charge_cycle_config;

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

    /* strncpy into a memset-0 buffer leaves the field NUL-terminated as long
     * as the literal is shorter than the field, which both are. */
    strncpy(config->device_id, DEFAULT_DEVICE_ID, sizeof(config->device_id) - 1U);
    strncpy(config->hw_rev, DEFAULT_HW_REV, sizeof(config->hw_rev) - 1U);
}

void ChargeCycleConfig_Init(void)
{
    ChargeCycleConfig_GetDefaults(&g_charge_cycle_config);
}

void ChargeCycleConfig_Get(ChargeCycleConfig_t *config)
{
    if (config == NULL) {
        return;
    }

    *config = g_charge_cycle_config;
}

bool ChargeCycleConfig_Set(const ChargeCycleConfig_t *config)
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

    /* An enabled stage must have five distinct thresholds.  Equal thresholds
     * are technically monotonic, but band_from_thresholds() evaluates from
     * the highest threshold down, so an equal pair silently removes one
     * operating band.  Disabled stages keep accepting their unused zeroed
     * defaults. */
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

        /* At equality the recovery boundary lands exactly on the previous
         * threshold; values below it still recover normally. */
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

    g_charge_cycle_config = *config;
    g_charge_cycle_config.version = CHARGE_CYCLE_CONFIG_VERSION;
    /* Defend the display/consumer side against a caller that filled the
     * identity fields to the brim without a terminator. */
    g_charge_cycle_config.device_id[sizeof(g_charge_cycle_config.device_id) - 1U] = '\0';
    g_charge_cycle_config.hw_rev[sizeof(g_charge_cycle_config.hw_rev) - 1U] = '\0';

    if (g_charge_cycle_config.device_id[0] == '\0') {
        strncpy(g_charge_cycle_config.device_id, DEFAULT_DEVICE_ID, sizeof(g_charge_cycle_config.device_id) - 1U);
    }
    if (g_charge_cycle_config.hw_rev[0] == '\0') {
        strncpy(g_charge_cycle_config.hw_rev, DEFAULT_HW_REV, sizeof(g_charge_cycle_config.hw_rev) - 1U);
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
        for (uint8_t i = 0; i < config->source_module_count; i++) {
            int8_t idx = CHG_LIB_AddModule((uint8_t)(i + 1), 0);
            /* Seed the module's rated current from config immediately, so
             * the very first current-limit command (sent as part of the
             * start sequence, before the module has necessarily answered
             * any poll yet) uses the real rating instead of transiently
             * falling back to a hardcoded default. Drivers also read the
             * module's own self-reported rated current over CAN once
             * online (e.g. Maxwell register 0x0012) and prefer that once
             * available -- this call only covers the brief window before
             * that first response arrives. */
            if (idx >= 0 && config->module_i_max_a > 0.0f) {
                CHG_LIB_SetModuleConfig((uint8_t)idx, config->module_i_max_a);
            }
        }
    }

    return true;
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
