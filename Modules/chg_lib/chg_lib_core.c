#include "chg_lib.h"
#include "debug_log.h"
#include "bsp_sys.h"
#include <string.h>
#include <math.h>

static const CHG_LIB_DriverOps_t *s_driver_table[CHG_LIB_MAX_DRV];
static const CHG_LIB_DriverOps_t *s_active_driver = 0;
static CHG_LIB_DriverId_t s_active_driver_id = CHG_LIB_DRV_NONE;
static CHG_LIB_TxSource_t s_command_source = CHG_LIB_TX_SOURCE_UNKNOWN;
static volatile uint32_t s_current_zero_reject_count = 0U;

#define CHG_LIB_CURRENT_ZERO_EPSILON_A 0.0001f

static bool set_current_limit_all_ex(float current_a,
                                     CHG_LIB_TxSource_t source,
                                     CHG_LIB_CurrentPath_t path);
static bool set_current_limit_ex(uint8_t idx, float current_a,
                                 CHG_LIB_TxSource_t source,
                                 CHG_LIB_CurrentPath_t path);

static CHG_LIB_CurrentPath_t path_for_source(CHG_LIB_CurrentPath_t default_path,
                                             CHG_LIB_TxSource_t source)
{
    if (source == CHG_LIB_TX_SOURCE_PC_SET_CURRENT) {
        return CHG_LIB_CURRENT_PATH_PC_SET_CURRENT;
    }
    if (source == CHG_LIB_TX_SOURCE_PC_PROFILE) {
        return CHG_LIB_CURRENT_PATH_PC_PROFILE;
    }
    if (source == CHG_LIB_TX_SOURCE_CC_START ||
        source == CHG_LIB_TX_SOURCE_CC_RAMP ||
        source == CHG_LIB_TX_SOURCE_CC_INHIBIT ||
        source == CHG_LIB_TX_SOURCE_CC_COMPLETION) {
        return CHG_LIB_CURRENT_PATH_CONTROLLER;
    }
    return default_path;
}

__attribute__((weak)) void CHG_LIB_RecordRejectedZero(CHG_LIB_TxSource_t source,
                                                      CHG_LIB_CurrentPath_t path)
{
    (void)source;
    (void)path;
}

static const CHG_LIB_DriverOps_t *get_active(void)
{
    return s_active_driver;
}

bool CHG_LIB_RegisterDriver(CHG_LIB_DriverId_t id, const CHG_LIB_DriverOps_t *ops)
{
    if ((uint32_t)id >= CHG_LIB_MAX_DRV || ops == 0) return false;
    s_driver_table[id] = ops;
    return true;
}

bool CHG_LIB_SelectDriver(CHG_LIB_DriverId_t id)
{
    if ((uint32_t)id >= CHG_LIB_MAX_DRV || s_driver_table[id] == 0) return false;

    /* Deinit old driver to stop any ongoing polling/timers */
    const CHG_LIB_DriverOps_t *old_driver = s_active_driver;
    if (old_driver != 0) {
        if (old_driver->deinit != 0) {
            old_driver->deinit();
        } else if (old_driver->init != 0) {
            old_driver->init();
        }
    }

    /* Switch to new driver */
    s_active_driver = s_driver_table[id];
    s_active_driver_id = id;

    /* Init new driver to reset its state */
    if (s_active_driver->init != 0) {
        s_active_driver->init();
    }

    return true;
}

CHG_LIB_DriverId_t CHG_LIB_GetActiveDriverId(void)
{
    return s_active_driver_id;
}

const char *CHG_LIB_GetActiveDriverName(void)
{
    const CHG_LIB_DriverOps_t *driver = get_active();
    return (driver != 0 && driver->name != 0) ? driver->name : "none";
}

void CHG_LIB_Init(void)
{
    const CHG_LIB_DriverOps_t *driver = get_active();
    if (driver != 0 && driver->init != 0) driver->init();
}

int8_t CHG_LIB_AddModule(uint8_t addr, uint8_t group)
{
    const CHG_LIB_DriverOps_t *driver = get_active();
    return (driver != 0 && driver->add_module != 0) ? driver->add_module(addr, group) : -1;
}

bool CHG_LIB_SetModuleConfig(uint8_t idx, float rated_current_a)
{
    const CHG_LIB_DriverOps_t *driver = get_active();
    return (driver != 0 && driver->set_config != 0) ? driver->set_config(idx, rated_current_a) : false;
}

void CHG_LIB_RemoveModule(uint8_t idx)
{
    const CHG_LIB_DriverOps_t *driver = get_active();
    if (driver != 0 && driver->remove_module != 0) driver->remove_module(idx);
}

bool CHG_LIB_SetVoltage(uint8_t idx, float voltage_v)
{
    const CHG_LIB_DriverOps_t *driver = get_active();
    return (driver != 0 && driver->set_voltage != 0) ? driver->set_voltage(idx, voltage_v) : false;
}

bool CHG_LIB_SetCurrentLimit(uint8_t idx, float current_a)
{
    return set_current_limit_ex(idx, current_a, CHG_LIB_TX_SOURCE_UNKNOWN,
                                CHG_LIB_CURRENT_PATH_LEGACY_MODULE);
}

static bool set_current_limit_ex(uint8_t idx, float current_a,
                                 CHG_LIB_TxSource_t source,
                                 CHG_LIB_CurrentPath_t path)
{
    const CHG_LIB_DriverOps_t *driver = get_active();
    if (!isfinite(current_a) || current_a < 0.0f) return false;
    if (current_a <= CHG_LIB_CURRENT_ZERO_EPSILON_A &&
        source != CHG_LIB_TX_SOURCE_CC_START &&
        source != CHG_LIB_TX_SOURCE_CC_INHIBIT &&
        source != CHG_LIB_TX_SOURCE_CC_COMPLETION) {
        s_current_zero_reject_count++;
        CHG_LIB_RecordRejectedZero(source, path);
        return false;
    }
    if (current_a <= CHG_LIB_CURRENT_ZERO_EPSILON_A) current_a = 0.0f;
    if (driver == 0 || driver->set_current_limit == 0) return false;
    s_command_source = source;
    bool result = driver->set_current_limit(idx, current_a);
    s_command_source = CHG_LIB_TX_SOURCE_UNKNOWN;
    return result;
}

bool CHG_LIB_SetCurrentLimitEx(uint8_t idx, float current_a,
                               CHG_LIB_TxSource_t source)
{
    return set_current_limit_ex(idx, current_a, source,
                                path_for_source(CHG_LIB_CURRENT_PATH_MODULE_EX,
                                                source));
}

bool CHG_LIB_Start(uint8_t idx)
{
    const CHG_LIB_DriverOps_t *driver = get_active();
    return (driver != 0 && driver->start != 0) ? driver->start(idx) : false;
}

bool CHG_LIB_Stop(uint8_t idx)
{
    const CHG_LIB_DriverOps_t *driver = get_active();
    return (driver != 0 && driver->stop != 0) ? driver->stop(idx) : false;
}

void CHG_LIB_SetVoltageAll(float voltage_v)
{
    CHG_LIB_SetVoltageAllEx(voltage_v, CHG_LIB_TX_SOURCE_UNKNOWN);
}

void CHG_LIB_SetVoltageAllEx(float voltage_v, CHG_LIB_TxSource_t source)
{
    const CHG_LIB_DriverOps_t *driver = get_active();
    if (!isfinite(voltage_v) || driver == 0 || driver->set_voltage_all == 0) return;
    s_command_source = source;
    driver->set_voltage_all(voltage_v);
    s_command_source = CHG_LIB_TX_SOURCE_UNKNOWN;
}

void CHG_LIB_SetCurrentLimitAll(float current_a)
{
    (void)set_current_limit_all_ex(current_a, CHG_LIB_TX_SOURCE_UNKNOWN,
                                   CHG_LIB_CURRENT_PATH_LEGACY_ALL);
}

static bool set_current_limit_all_ex(float current_a, CHG_LIB_TxSource_t source,
                                     CHG_LIB_CurrentPath_t path)
{
    const CHG_LIB_DriverOps_t *driver = get_active();
    if (!isfinite(current_a) || current_a < 0.0f) return false;
    if (current_a <= CHG_LIB_CURRENT_ZERO_EPSILON_A &&
        source != CHG_LIB_TX_SOURCE_CC_START &&
        source != CHG_LIB_TX_SOURCE_CC_INHIBIT &&
        source != CHG_LIB_TX_SOURCE_CC_COMPLETION) {
        s_current_zero_reject_count++;
        CHG_LIB_RecordRejectedZero(source, path);
        return false;
    }
    if (current_a <= CHG_LIB_CURRENT_ZERO_EPSILON_A) current_a = 0.0f;
    if (driver == 0 || driver->set_current_limit_all == 0) return false;
    s_command_source = source;
    driver->set_current_limit_all(current_a);
    s_command_source = CHG_LIB_TX_SOURCE_UNKNOWN;
    return true;
}

bool CHG_LIB_SetCurrentLimitAllEx(float current_a, CHG_LIB_TxSource_t source)
{
    return set_current_limit_all_ex(current_a, source,
                                    path_for_source(CHG_LIB_CURRENT_PATH_ALL_EX,
                                                    source));
}

CHG_LIB_TxSource_t CHG_LIB_GetCommandSource(void)
{
    return s_command_source;
}

uint32_t CHG_LIB_GetCurrentZeroRejectCount(void)
{
    return s_current_zero_reject_count;
}

void CHG_LIB_StartAll(void)
{
    const CHG_LIB_DriverOps_t *driver = get_active();
    if (driver != 0 && driver->start_all != 0) driver->start_all();
}

void CHG_LIB_StopAll(void)
{
    const CHG_LIB_DriverOps_t *driver = get_active();
    if (driver != 0 && driver->stop_all != 0) driver->stop_all();
}

void CHG_LIB_EmergencyStop(void)
{
    const CHG_LIB_DriverOps_t *driver = get_active();
    if (driver != 0 && driver->emergency_stop != 0) driver->emergency_stop();
}

void CHG_LIB_Process(uint32_t now_tick)
{
    /* Both process() and feed_frame() run in the main loop.  CAN RX only
     * captures raw frames in the ISR, so no long IRQ-off section is needed
     * around the driver call. */
    const CHG_LIB_DriverOps_t *driver = get_active();
    if (driver != 0 && driver->process != 0) driver->process(now_tick);
}

void CHG_LIB_FeedCanFrame(uint32_t ext_id, const uint8_t *data, uint8_t dlc)
{
    /* Main-loop context.  The FDCAN ISR only copies frames to a raw queue. */
    const CHG_LIB_DriverOps_t *driver = get_active();
    if (driver != 0 && driver->feed_frame != 0) driver->feed_frame(ext_id, data, dlc);
}

void CHG_LIB_GetSystemSummary(CHG_LIB_SystemSummary_t *summary)
{
    const CHG_LIB_DriverOps_t *driver = get_active();
    if (summary == 0) return;
    memset(summary, 0, sizeof(*summary));
    if (driver != 0 && driver->get_system_summary != 0) driver->get_system_summary(summary);
}

uint8_t CHG_LIB_GetModuleCount(void)
{
    const CHG_LIB_DriverOps_t *driver = get_active();
    return (driver != 0 && driver->get_module_count != 0) ? driver->get_module_count() : 0;
}

bool CHG_LIB_GetModuleView(uint8_t idx, CHG_LIB_ModuleView_t *view)
{
    const CHG_LIB_DriverOps_t *driver;
    BSP_EnterCritical();
    driver = get_active();
    BSP_ExitCritical();
    return (driver != 0 && driver->get_module_view != 0) ? driver->get_module_view(idx, view) : false;
}
