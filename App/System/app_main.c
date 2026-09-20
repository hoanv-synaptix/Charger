/**
 * @file    app_main.c
 * @brief   Application layer - main loop logic
 * @note    CAN1 (FDCAN1, PD0/PD1): 125Kbps - charger modules (Maxwell/Lianming/TonHe)
 *          CAN2 (FDCAN2, PB12/PB13): 250Kbps - BMS
 *          RS485 (USART3, PB10/PB11, DE=PB1): DWIN HMI (USART SWAP enabled)
 *          USB CDC: PC app protocol
 */

#include "app_main.h"
#include "bsp_can.h"
#include "bsp_adc.h"
#include "bsp_rs485.h"
#include "bms_can.h"
#include "bms_core.h"
#include "dwin_protocol.h"
#include "charge_cycle_config.h"
#include "charge_cycle_storage.h"
#include "charge_energy_storage.h"
#include "charge_controller.h"
#include "alarm.h"
#include "dwin_alarm_text.h"
#include "chg_lib.h"
#include "chg_lib_can_backend.h"
#include "chg_lib_driver_lianming.h"
#include "chg_lib_driver_maxwell.h"
#include "chg_lib_driver_tonhe.h"
#include "pc_protocol.h"
#include "pc_debug_protocol.h"
#include "app_dwin_debug.h"
#include "app_rtc_sync.h"
#include "debug_log.h"
#include "main.h"
#include "iwdg.h"

#include "bsp_gpio.h"
#include "bsp_sys.h"
#include "bsp_rtc.h"
#include "bsp_spi_flash.h"
#include "bsp_sd_card.h"
#include "sd_storage.h"
#include "bsp_quectel.h"
#include "quectel_at_engine.h"
#include "ota_service.h"

#include "app_version.h"

/* ============== Configuration ============== */

#define APP_PROCESS_INTERVAL_MS      20U     /* Control loop period */
#define APP_LED_INTERVAL_MS          100U    /* LED update period */
#define APP_BTN_DEBOUNCE_MS          50U     /* Button debounce */

#define DWIN_BOOT_DELAY_MS           3000U   /* Wait for DWIN panel to finish boot */
#define DWIN_HEARTBEAT_INTERVAL_MS   5000U   /* Periodic force-full-refresh interval */
#define DWIN_UPDATE_INTERVAL_MS      20U     /* Field scatter cadence (20ms per group -> ~220ms full cycle) */
#define DWIN_RESET_INTERVAL_MS       (12U * 60U * 60U * 1000U)
#define DWIN_REBOOT_WAIT_MS          3000U   /* Wait for panel after SW reset */

#include <string.h>
#include <math.h>
#include <stdio.h>

/* ============== Private state ============== */

static uint32_t last_process_tick   = 0;
static uint32_t last_led_tick       = 0;
static uint32_t last_dwin_tick      = 0;
static uint32_t last_dwin_full_tick = 0;
static uint32_t last_dwin_reset_tick = 0;
static uint32_t last_main_log       = 0;
static uint32_t last_can_diag_log   = 0;
/* Button debounce -- single toggle button (BUTTON_1/PA15), see App_Loop()
 * "(2) Button handling" for the state-decides-direction logic. BUTTON_2
 * (PD2) is no longer read here -- confirmed with user 2026-08-29, hardware
 * only needs 1 button. */
static uint32_t btn_start_last    = 0;
static uint8_t  btn_start_prev    = 0;
static uint8_t  btn_start_db      = 0;

static void log_can_diagnostics(uint32_t now)
{
    if ((uint32_t)(now - last_can_diag_log) < 1000U) return;
    last_can_diag_log = now;

    BSP_CAN_RxStats_t c1;
    BSP_CAN_RxStats_t c2;
    BMS_Diagnostics_t bms_diag;
    BMS_View_t bms_view;
    BMS_GetDiagnostics(&bms_diag);
    BMS_GetView(&bms_view);
    BSP_CAN_GetRxStats(1U, &c1);
    BSP_CAN_GetRxStats(2U, &c2);

    uint32_t bms_valid = 0U;
    for (uint8_t i = 0U; i < BMS_FRAME_MAX; i++) {
        bms_valid += bms_diag.valid_rx_count[i];
    }

    if (c1.queue_overflow_count != 0U || c1.stale_queue_drop_count != 0U ||
        c1.fifo_lost_count != 0U ||
        c1.fifo_full_count != 0U || c1.bus_off_count != 0U ||
        c1.error_warning_count != 0U || c1.error_passive_count != 0U ||
        c1.protocol_error_count != 0U ||
        c2.queue_overflow_count != 0U || c2.stale_queue_drop_count != 0U ||
        c2.fifo_lost_count != 0U ||
        c2.fifo_full_count != 0U || c2.bus_off_count != 0U ||
        c2.error_warning_count != 0U || c2.error_passive_count != 0U ||
        c2.protocol_error_count != 0U || bms_diag.unknown_id_count != 0U ||
        bms_diag.invalid_dlc_count != 0U ||
        bms_diag.argument_reject_count != 0U) {
        LOG("[CAN DIAG] C1 raw=%lu q=%lu ovf=%lu old=%lu lost=%lu full=%lu bo=%lu "
            "ew=%lu ep=%lu pe=%lu | C2 raw=%lu q=%lu ovf=%lu lost=%lu "
            "old=%lu full=%lu bo=%lu ew=%lu ep=%lu pe=%lu "
            "| BMS valid=%lu unknown=%lu dlc=%lu arg=%lu last=%lu on=%u stale=%u\r\n",
            (unsigned long)c1.raw_rx_count,
            (unsigned long)c1.queued_rx_count,
            (unsigned long)c1.queue_overflow_count,
            (unsigned long)c1.stale_queue_drop_count,
            (unsigned long)c1.fifo_lost_count,
            (unsigned long)c1.fifo_full_count,
            (unsigned long)c1.bus_off_count,
            (unsigned long)c1.error_warning_count,
            (unsigned long)c1.error_passive_count,
            (unsigned long)c1.protocol_error_count,
            (unsigned long)c2.raw_rx_count,
            (unsigned long)c2.queued_rx_count,
            (unsigned long)c2.queue_overflow_count,
            (unsigned long)c2.fifo_lost_count,
            (unsigned long)c2.stale_queue_drop_count,
            (unsigned long)c2.fifo_full_count,
            (unsigned long)c2.bus_off_count,
            (unsigned long)c2.error_warning_count,
            (unsigned long)c2.error_passive_count,
            (unsigned long)c2.protocol_error_count,
            (unsigned long)bms_valid,
            (unsigned long)bms_diag.unknown_id_count,
            (unsigned long)bms_diag.invalid_dlc_count,
            (unsigned long)bms_diag.argument_reject_count,
            (unsigned long)bms_view.last_rx_tick,
            bms_view.online ? 1U : 0U,
            BMS_IsDataStale() ? 1U : 0U);
    }
}
/* One-shot: identity strings + initial page pushed to the DWIN panel once
 * it has had time to boot (the panel comes up slower than the MCU). */
static bool     dwin_boot_sent    = false;
/* DWIN login/pre-charge session belongs to the composition root: it is UI
 * state, not charge policy or a DWIN protocol concern. PIN digits are never
 * logged and are cleared whenever the short-lived session ends. */
static char     dwin_pin_digits[7];
static uint8_t  dwin_pin_length = 0U;
static bool     dwin_precharge_session = false;
static bool     dwin_precharge_exit_pending = false;
static bool     dwin_precharge_seen_active = false;
static bool     dwin_precharge_error_hold = false;
static AlarmCode_t dwin_precharge_error_code = ALARM_NONE;

typedef enum {
    DWIN_RECOVERY_RUNNING = 0,
    DWIN_RECOVERY_WAIT_BOOT,
    DWIN_RECOVERY_RESTORE_PENDING,
} DwinRecoveryState_e;

static DwinRecoveryState_e dwin_recovery_state = DWIN_RECOVERY_RUNNING;
static uint32_t dwin_recovery_start_tick = 0U;
static uint32_t dwin_reset_requested_count = 0U;
static uint32_t dwin_reset_completed_count = 0U;
static uint32_t dwin_replay_count = 0U;
static uint32_t dwin_rx_suppressed_bytes = 0U;
static float    s_total_charged_ah;
static float    s_total_energy_kwh;
static uint32_t s_last_energy_tick;

/* Keep the last charge duration visible briefly after a session ends. This
 * gives the operator time to read the result before the footer returns to the
 * live RTC clock, without blocking the main loop or persisting a transient UI
 * value. */
#define CHARGE_DURATION_HOLD_MS 60000U

/* ============== LED control ============== */

static void led_run_on(void)   { BSP_LED_On(BSP_LED_RUN); }
static void led_run_off(void)  { BSP_LED_Off(BSP_LED_RUN); }
static void led_power_on(void) { BSP_LED_On(BSP_LED_POWER); }

static void dwin_set_unavailable(char *text, size_t text_size)
{
    if (text == NULL || text_size == 0U) return;
    memset(text, 0, text_size);
    if (text_size > 3U) {
        memcpy(text, "---", 3U);
    }
}

static void dwin_login_clear(void)
{
    dwin_pin_length = 0U;
    memset(dwin_pin_digits, 0, sizeof(dwin_pin_digits));
    DWIN_SendString(VP_LOGIN_PIN_TEXT, "", DWIN_TEXT_8_BYTES_WORDS);
}

static void dwin_login_show_mask(void)
{
    char mask[7];
    memset(mask, '*', dwin_pin_length);
    mask[dwin_pin_length] = '\0';
    DWIN_SendString(VP_LOGIN_PIN_TEXT, mask, DWIN_TEXT_8_BYTES_WORDS);
}

static uint32_t dwin_login_pin_value(void)
{
    uint32_t value = 0U;
    for (uint8_t i = 0U; i < dwin_pin_length; i++) {
        value = (value * 10U) + (uint32_t)(dwin_pin_digits[i] - '0');
    }
    return value;
}

static void dwin_open_login(void)
{
    dwin_precharge_session = false;
    dwin_precharge_error_hold = false;
    dwin_precharge_error_code = ALARM_NONE;
    dwin_precharge_exit_pending = false;
    dwin_precharge_seen_active = false;
    dwin_login_clear();
    DWIN_SetPage(DWIN_PAGE_LOGIN);
    DWIN_ForceFullRefresh();
}

static void dwin_open_precharge(void)
{
    ChargeController_PreparePrecharge();
    uint16_t status = DWIN_PRECHARGE_STATUS_READY;
    uint16_t button = DWIN_PRECHARGE_BTN_START;
    dwin_precharge_error_hold = false;
    dwin_precharge_error_code = ALARM_NONE;
    dwin_precharge_exit_pending = false;
    dwin_precharge_seen_active = false;
    DWIN_SetPage(DWIN_PAGE_PRECHARGE);
    DWIN_SendWords(VP_PRECHARGE_STATUS_ICON, &status, 1U);
    DWIN_SendWords(VP_PRECHARGE_BTN_ICON, &button, 1U);
    DWIN_ForceFullRefresh();
}

static void dwin_end_precharge_session(void)
{
    ChargeController_EndPrechargeSession(BSP_GetTick());
    dwin_precharge_session = false;
    dwin_precharge_error_hold = false;
    dwin_precharge_error_code = ALARM_NONE;
    dwin_precharge_exit_pending = false;
    dwin_precharge_seen_active = false;
    dwin_login_clear();
}

static DwinPageId_e dwin_page_after_panel_reset(void)
{
    return dwin_precharge_session ? DWIN_PAGE_PRECHARGE : DWIN_PAGE_DASH;
}

static void dwin_send_identity_and_page(uint32_t now)
{
    const char *hw_str = ChargeCycleConfig_GetHwRev();

    if (strncmp(hw_str, "HW ", 3) == 0) {
        hw_str += 3;
    } else if (strncmp(hw_str, "HW", 2) == 0) {
        hw_str += 2;
    }

    char hw_buf[16];
    if (hw_str[0] != 'V' && hw_str[0] != 'v' && hw_str[0] != '\0') {
        (void)snprintf(hw_buf, sizeof(hw_buf), "V%s", hw_str);
        hw_str = hw_buf;
    }

    DWIN_SendSettingStrings(hw_str, FW_VERSION_STRING,
                            ChargeCycleConfig_GetDeviceId());
    DWIN_SetPage(dwin_page_after_panel_reset());
    DWIN_ForceFullRefresh();
    last_dwin_full_tick = now;
}

static void dwin_check_and_send_identity_update(void)
{
    static char s_last_dev_id[16] = "";
    static char s_last_hw_rev[12] = "";
    const char *cur_dev = ChargeCycleConfig_GetDeviceId();
    const char *cur_hw = ChargeCycleConfig_GetHwRev();

    if (s_last_dev_id[0] == '\0') {
        strncpy(s_last_dev_id, cur_dev, sizeof(s_last_dev_id) - 1U);
        strncpy(s_last_hw_rev, cur_hw, sizeof(s_last_hw_rev) - 1U);
        return;
    }

    if (strncmp(s_last_dev_id, cur_dev, sizeof(s_last_dev_id)) != 0 ||
        strncmp(s_last_hw_rev, cur_hw, sizeof(s_last_hw_rev)) != 0) {
        strncpy(s_last_dev_id, cur_dev, sizeof(s_last_dev_id) - 1U);
        strncpy(s_last_hw_rev, cur_hw, sizeof(s_last_hw_rev) - 1U);

        const char *hw_str = cur_hw;
        if (strncmp(hw_str, "HW ", 3) == 0) {
            hw_str += 3;
        } else if (strncmp(hw_str, "HW", 2) == 0) {
            hw_str += 2;
        }
        char hw_buf[16];
        if (hw_str[0] != 'V' && hw_str[0] != 'v' && hw_str[0] != '\0') {
            (void)snprintf(hw_buf, sizeof(hw_buf), "V%s", hw_str);
            hw_str = hw_buf;
        }
        DWIN_SendSettingStrings(hw_str, FW_VERSION_STRING, cur_dev);
    }
}

static void dwin_service_recovery(uint32_t now)
{
    if (dwin_recovery_state == DWIN_RECOVERY_RUNNING) {
        if (dwin_boot_sent &&
            (uint32_t)(now - last_dwin_reset_tick) >= DWIN_RESET_INTERVAL_MS) {
            /* This is a panel-only reset. The MCU-owned controller, relay,
             * CAN state and charger operation are intentionally untouched. */
            DWIN_InvalidateSyncState();
            DWIN_SendSoftwareReset();
            dwin_recovery_start_tick = now;
            dwin_recovery_state = DWIN_RECOVERY_WAIT_BOOT;
            dwin_reset_requested_count++;
            LOG("DWIN: software reset requested count=%lu\r\n",
                (unsigned long)dwin_reset_requested_count);
        }
    } else if (dwin_recovery_state == DWIN_RECOVERY_WAIT_BOOT &&
               (uint32_t)(now - dwin_recovery_start_tick) >= DWIN_REBOOT_WAIT_MS) {
        dwin_recovery_state = DWIN_RECOVERY_RESTORE_PENDING;
    }
}

static void dwin_set_soc_unavailable(char *text, size_t text_size)
{
    if (text == NULL || text_size == 0U) return;
    memset(text, 0, text_size);
    if (text_size > 3U) {
        /* SOC keeps its unit in the text field, including while unavailable. */
        memcpy(text, "--%", 3U);
    }
}

static DwinSocColor_e dwin_soc_color_for_bms(const BMS_View_t *bms)
{
    if (bms == NULL || !bms->online || bms->soc > 100U) {
        return DWIN_SOC_COLOR_UNAVAILABLE;
    }
    if (bms->soc <= 10U) {
        return DWIN_SOC_COLOR_CRITICAL;
    }
    if (bms->soc <= 30U) {
        return DWIN_SOC_COLOR_LOW;
    }
    if (bms->soc <= 60U) {
        return DWIN_SOC_COLOR_MEDIUM;
    }
    return DWIN_SOC_COLOR_NORMAL;
}

/* newlib-nano is intentionally linked without float printf support. Keep all
 * dashboard formatting deterministic and small by converting the already
 * validated physical float to fixed-point integers before snprintf(). */
static bool dwin_format_fixed(char *text, size_t text_size, float value,
                              uint8_t fractional_digits, const char *unit)
{
    float scale;
    float magnitude;
    uint32_t scaled;
    uint32_t whole;
    uint32_t fraction;
    bool negative;
    int written;

    if (text == NULL || text_size == 0U || unit == NULL ||
        !isfinite(value) || fractional_digits > 2U) {
        return false;
    }

    scale = (fractional_digits == 0U) ? 1.0f :
            ((fractional_digits == 1U) ? 10.0f : 100.0f);
    negative = (value < 0.0f);
    magnitude = negative ? -value : value;
    scaled = (uint32_t)(magnitude * scale + 0.5f);
    whole = scaled / (uint32_t)scale;
    fraction = scaled % (uint32_t)scale;

    if (unit[0] == '\0') {
        if (fractional_digits == 0U) {
            written = snprintf(text, text_size, "%s%lu",
                               negative ? "-" : "", (unsigned long)whole);
        } else if (fractional_digits == 1U) {
            written = snprintf(text, text_size, "%s%lu.%01lu",
                               negative ? "-" : "", (unsigned long)whole,
                               (unsigned long)fraction);
        } else {
            written = snprintf(text, text_size, "%s%lu.%02lu",
                               negative ? "-" : "", (unsigned long)whole,
                               (unsigned long)fraction);
        }
    } else if (fractional_digits == 0U) {
        written = snprintf(text, text_size, "%s%lu %s",
                           negative ? "-" : "", (unsigned long)whole, unit);
    } else if (fractional_digits == 1U) {
        written = snprintf(text, text_size, "%s%lu.%01lu %s",
                           negative ? "-" : "", (unsigned long)whole,
                           (unsigned long)fraction, unit);
    } else {
        written = snprintf(text, text_size, "%s%lu.%02lu %s",
                           negative ? "-" : "", (unsigned long)whole,
                           (unsigned long)fraction, unit);
    }
    return written >= 0 && (size_t)written < text_size;
}

/* ============== Button read ============== */

static uint8_t read_btn_start(void) { return BSP_BTN_IsPressed(BSP_BTN_START) ? 1 : 0; }

/* ============== DWIN state <-> screen icon mapping ==============
 *
 * Policy lives here in the composition root, not in Modules/hmi (which does
 * pure framing). The dashboard centre element is one status-box Variable
 * Icon (VP_SYS_STATUS_ICON, 6 states) plus one button-label Variable Icon
 * (VP_SYS_BTN_ICON, 4 modes). Neither is a safety interlock -- the relay /
 * charge-control logic reads fault_flags/state directly. */

static uint16_t dwin_status_from_state(const ChargeCtrlView_t *cc,
                                       const CHG_LIB_SystemSummary_t *sum)
{
    (void)sum;
    /* Fault first: any active fault flag, or the FAULT state -> ERROR. */
    if (cc->state == CHARGE_CTRL_STATE_FAULT ||
        cc->fault_flags != CHARGE_CTRL_FAULT_NONE) {
        return DWIN_STATUS_ERROR;
    }

    switch (cc->state) {
        case CHARGE_CTRL_STATE_RUNNING:
        case CHARGE_CTRL_STATE_PRECHARGE:
            /* relay latched closed == real current is flowing */
            return cc->relay_should_close ? DWIN_STATUS_CHARGING
                                          : DWIN_STATUS_STARTING;
        case CHARGE_CTRL_STATE_READY:
        case CHARGE_CTRL_STATE_DELAY:
            return DWIN_STATUS_STARTING;
        case CHARGE_CTRL_STATE_STOPPING:
            return DWIN_STATUS_CHARGING; /* transient, keep showing activity */
        case CHARGE_CTRL_STATE_IDLE:
        default:
            break;
    }

    /* While in IDLE / Standby: if there is an active alarm requiring STOP/ESTOP, show ERROR + RESET */
    {
        AlarmView_t av;
        Alarm_GetView(&av);
        if (av.highest_action >= ALARM_ACT_STOP) {
            return DWIN_STATUS_ERROR;
        }
    }

    /* IDLE: distinguish "finished a cycle" from standby / ready.
     * Note: DGUS project 28.icl only has 5 status icons (0:READY, 1:STARTING,
     * 2:CHARGING, 3:COMPLETE, 4:ERROR). 25.icl has 3 button icons (0:START,
     * 1:STOP, 2:RESET). Values >=5 or >=3 cause controls to disappear.
     * In standby (with or without BMS/modules), always present READY + START. */
    if (cc->stop_reason == CHARGE_STOP_VOLTAGE_REACHED ||
        cc->stop_reason == CHARGE_STOP_CELL_VOLTAGE_REACHED ||
        cc->stop_reason == CHARGE_STOP_SOC_REACHED) {
        return DWIN_STATUS_COMPLETE;
    }
    return DWIN_STATUS_READY;
}

static uint16_t dwin_btn_mode_from_status(uint16_t status_icon)
{
    switch (status_icon) {
        case DWIN_STATUS_STARTING:
        case DWIN_STATUS_CHARGING:
            return DWIN_BTN_STOP;
        case DWIN_STATUS_COMPLETE:
        case DWIN_STATUS_ERROR:
            return DWIN_BTN_RESET;
        case DWIN_STATUS_READY:
        default:
            return DWIN_BTN_START;
    }
}

/* Current DWIN status (0..4) -- the single source of truth for what the
 * button means, since it distinguishes COMPLETE (IDLE + target reached)
 * from a plain IDLE/READY. */
static uint16_t dwin_current_status(void)
{
    ChargeCtrlView_t v;
    CHG_LIB_SystemSummary_t s;
    ChargeController_GetView(&v);
    CHG_LIB_GetSystemSummary(&s);
    return dwin_status_from_state(&v, &s);
}

/* Shared by the physical BUTTON_1/PA15 handler and the DWIN screen button:
 * one press starts / stops / resets-if-safe, decided by the state the button
 * is currently showing -- NOT by which surface the press came from. */
static void app_action_button(uint16_t dwin_status, uint32_t now)
{
    switch (dwin_status) {
        case DWIN_STATUS_READY:
            LOG("App: button (READY) -> start charge cycle\r\n");
            ChargeController_Start(CHARGE_CTRL_OWNER_DWIN, false, now);
            break;
        case DWIN_STATUS_STARTING:
        case DWIN_STATUS_CHARGING:
            LOG("App: button (STARTING/CHARGING) -> stop charge cycle\r\n");
            ChargeController_Stop(now);
            break;
        case DWIN_STATUS_ERROR:
            LOG("App: button (ERROR) -> reset if safe\r\n");
            Alarm_Acknowledge(now);      /* clear latched alarms whose cause is gone */
            /* The controller, not the HMI, decides whether the root cause and
             * output path are safe enough to clear. A failed reset is a
             * deliberate no-op and leaves ERROR visible. */
            (void)ChargeController_ResetEmergencyStop(now);
            (void)ChargeController_ResetFaultIfSafe(now);
            break;
        case DWIN_STATUS_COMPLETE:
            LOG("App: button (COMPLETE) -> acknowledge, back to READY\r\n");
            ChargeController_AcknowledgeCompletion();
            break;
        default:
            break;
    }
}

/* ============== Init ============== */

void App_Init(void)
{
    LOG_Banner();
    LOG("App_Init: Starting system...\r\n");

    /* Enable Peripheral Power (RS485/CAN/HMI) - assert early with 50ms stabilization delay */
    HAL_GPIO_WritePin(GPIOA, MCU_PA4_POWER_EN_Pin, GPIO_PIN_SET);
    HAL_Delay(50);

    led_run_off();
    led_power_on();

    /* Real-Time Clock (internal LSI / VBAT domain) */
    if (BSP_RTC_Init()) {
        char rtc_str[32];
        BSP_RTC_FormatDateTime(rtc_str, sizeof(rtc_str));
        LOG("App_Init: RTC ready (%s, synced=%d).\r\n", rtc_str, (int)BSP_RTC_IsTimeValid());
    } else {
        LOG("App_Init: WARNING - RTC init failed!\r\n");
    }

    /* RS485 (USART3 + DE pin, che do nhan) + DWIN HMI protocol */
    BSP_RS485_Init();
    DWIN_Init();
    LOG("App_Init: RS485/DWIN HMI ready (USART3 115200-8N1 SWAP, DE=PB1).\r\n");

    /* Initialize BMS driver (CAN2, 250Kbps) */
    BMS_Init();
    LOG("App_Init: BMS driver ready.\r\n");

    /* Register charger drivers (CAN1, 125Kbps) */
    LOG("App_Init: Initializing charger core\r\n");
    CHG_LIB_RegisterDriver(CHG_LIB_DRV_MAXWELL, CHG_LIB_MaxwellDriverOps());
    CHG_LIB_RegisterDriver(CHG_LIB_DRV_LIANMING, CHG_LIB_LianmingDriverOps());
    CHG_LIB_RegisterDriver(CHG_LIB_DRV_TONHE, CHG_LIB_TonheDriverOps());
    CHG_LIB_Init();
    CHG_LIB_CanBackend_Init();

    /* Wire the FDCAN RX ISR to the business modules. BSP only captures raw
     * frames off the wire and must not #include bms_core.h/chg_lib.h itself
     * (AGENTS.md sec 5-6) -- the composition root does the wiring instead.
     * BSP_CAN_ProcessRx() invokes these from main context after the ISR has
     * copied raw frames into its bounded transport queue. */
    BSP_CAN_SetChargerRxHandler(CHG_LIB_FeedCanFrame);
    BSP_CAN_SetBmsRxHandler(BMS_FeedFrame);

    /* Start CAN1 + CAN2 only after consumers are ready. */
    LOG("App_Init: Starting CAN bus...\r\n");
    if (!BSP_CAN_Start()) {
        LOG("App_Init: ERROR - Could not start CAN bus!\r\n");
    } else {
        LOG("App_Init: CAN bus started successfully.\r\n");
    }

    /* Initialize external SPI NOR Flash (W25Q / GD25Q via SPI2) */
    if (BSP_SPIFlash_Init()) {
        LOG("App_Init: External SPI Flash ready (JEDEC=0x%06lX, Cap=%lu KB).\r\n",
            (unsigned long)BSP_SPIFlash_GetJedecId(),
            (unsigned long)(BSP_SPIFlash_GetCapacity() / 1024UL));
    } else {
        LOG("App_Init: WARNING - External SPI Flash not detected, using fallback.\r\n");
    }

    /* SD is optional storage. Keep card absence non-fatal so a charger can
     * boot and charge normally without a card inserted. */
    if (SDStorage_Mount()) {
        LOG("App_Init: SD card ready (type=%u, blocks=%lu).\r\n",
            (unsigned)BSP_SDCard_GetType(),
            (unsigned long)BSP_SDCard_GetBlockCount());
    } else {
        LOG("App_Init: SD card not mounted (FatFs=%u).\r\n",
            (unsigned)SDStorage_GetLastResult());
    }

    ChargeCycleConfig_Init();

    /* Load config from flash — this populates module_type and, via
     * ChargeCycleConfig_Set() (called internally either way, see
     * charge_cycle_storage.c), selects the matching driver and
     * auto-registers its modules. That's the single source of truth for
     * module_type -> driver_id; no separate restoration step needed here
     * (a narrower ad-hoc re-implementation of that mapping used to live in
     * this function and silently missed the RAM-default module_type on a
     * blank-flash first boot -- fixed at the source in
     * ChargeCycleStorage_Init() instead of duplicating the mapping here). */
    ChargeCycleStorage_Init();
    ChargeEnergyStorage_Init();
    ChargeEnergyStorage_Get(&s_total_charged_ah, &s_total_energy_kwh);
    LOG("App_Init: Driver selected: id=%u\r\n", (unsigned)CHG_LIB_GetActiveDriverId());

    /* Initialize charge controller */
    ChargeController_Init();
    LOG("App_Init: Charge controller initialized.\r\n");

    /* Initialize the unified alarm subsystem */
    Alarm_Init();
    LOG("App_Init: Alarm subsystem initialized.\r\n");

    /* Initialize ADC for NTC */
    BSP_ADC_Init();
    LOG("App_Init: NTC ADC initialized.\r\n");

    /* Initialize Quectel LTE hardware driver and AT Engine */
    BSP_Quectel_Init();
    QuectelEngine_Init();
    OTAService_Init();
    /* Confirm a boot-tested image only after all safety-critical subsystems
     * have completed initialization. A crash before this point is therefore
     * recoverable by the bootloader on the next reset. */
    OTAService_ConfirmBoot();
    LOG("App_Init: Quectel LTE driver, AT engine, and OTA service initialized.\r\n");

    LOG("App_Init: Initialization complete.\r\n");
}

/* ============== Main Loop ============== */

void App_Loop(void)
{
    uint32_t now = BSP_GetTick();

    /* Non-blocking Quectel hardware power & cellular AT engine state machine */
    BSP_Quectel_Process(now);
    QuectelEngine_Process(now);
    OTAService_Process(now);

    /* Schedule/recover the DWIN panel independently of the MCU and charging
     * state. This state machine is non-blocking so the MCU watchdog and all
     * safety/control processing continue during the panel reboot window. */
    dwin_service_recovery(now);

    if (now - last_main_log >= 2000) {
        last_main_log = now;
        LOG("[MAIN_LOOP] running tick=%lu\r\n", now);
    }

    /* Dispatch complete USB CDC frames outside the RX ISR. This is where
     * protocol commands may safely touch RTC, charger modules, and app state. */
    PC_Protocol_ProcessRx();
    App_RtcSync_Process();

    /* Drain queued USB CDC TX */
    PC_Protocol_ProcessTx();

    /* Stream module data to debug app (internal rate-limit) */
    DebugProtocol_SendStream();

    /* Watchdog bus-off cho CAN1/CAN2 */
    BSP_CAN_Process();

    /* FDCAN ISR only captures raw frames. Deliver a bounded batch to the
     * protocol consumers from main context before the 20ms control step. */
    BSP_CAN_ProcessRx();
    log_can_diagnostics(now);

    /* (1) Control loop 20ms: charger FSM, BMS, charge controller */
    if ((now - last_process_tick) >= APP_PROCESS_INTERVAL_MS) {
        last_process_tick = now;
        CHG_LIB_Process(now);
        BMS_Process(now);

        /* Re-arm the NTC ADC scan (single-shot ADC + one-shot DMA -- see
         * bsp_adc.c). Internally rate-limited; without this the jack temps
         * below would be frozen at their power-on sample. */
        BSP_ADC_Process(now);

        /* Read jack/connector NTC temperature here (Platform) and hand the
         * max of the 4 channels to charge policy (App/Charge), which must
         * not touch BSP_ADC itself (AGENTS.md sec 5-6). Same max-of-4 +
         * "-50C means disconnected, fall back to 25C" logic that used to
         * live inside apply_jack_temp_derating(). */
        {
            float jack_temp_c = -273.15f;
            for (uint8_t i = 0; i < 4; i++) {
                float temp = BSP_ADC_GetTempC(i);
                if (isfinite(temp) && temp > jack_temp_c) {
                    jack_temp_c = temp;
                }
            }
            if (jack_temp_c < -50.0f) {
                jack_temp_c = 25.0f; /* Fallback if all disconnected */
            }
            ChargeController_SetJackTempC(jack_temp_c);
        }

        ChargeController_Process(now);

        /* Unified alarm evaluation. Runs AFTER the controller (so its view,
         * plus the BMS / module views, are fresh) and BEFORE the relay GPIO
         * mirror below -- an alarm that commands a stop this tick is reflected
         * in the relay write via the ChargeController_GetView() there. */
        Alarm_Process(now);

        /* Cập nhật Rơ-le (Relay) — quyết định đóng/mở được tính trong
         * ChargeController_Process() (relay_should_close: RUNNING + điện áp
         * module ≥ 90% target + BMS an toàn khi ở chế độ BMS-Controlled,
         * không yêu cầu BMS ở chế độ Standalone). App layer chỉ đọc kết quả
         * và ghi GPIO — không tự quyết định điều kiện an toàn ở đây.
         *
         * Cả 3 relay (RELAY_1/PB14, RELAY_2/PB15, RELAY_3/PA8) đóng/mở CÙNG
         * lúc, cùng vai trò -- xác nhận với người dùng 2026-08-29: phần cứng
         * thực tế chỉ cần 1 relay để đóng/cắt mạch sạc, 3 relay không phải
         * 3 chức năng khác nhau (không phải pre-charge/main/aux riêng biệt).
         * RELAY_3 nằm khác port (GPIOA) nên ghi bằng lệnh riêng. */
        {
            ChargeCtrlView_t relay_view;
            ChargeController_GetView(&relay_view);
            GPIO_PinState relay_pin_state = relay_view.relay_should_close ? GPIO_PIN_SET : GPIO_PIN_RESET;
            HAL_GPIO_WritePin(GPIOB, MCU_PB14_RELAY_1_Pin|MCU_PB15_RELAY_2_Pin, relay_pin_state);
            HAL_GPIO_WritePin(GPIOA, MCU_PA8_RELAY_3_Pin, relay_pin_state);
        }
    }

    PC_Protocol_ProcessTx();
    BSP_CAN_ProcessTxTrace();
    
    /* DWIN HMI: drain RX + parse; pump any queued bench-debug frame first
     * (no-op unless CHG_DEBUG_DWIN). */
    if (dwin_recovery_state == DWIN_RECOVERY_RUNNING) {
        AppDwinDebug_Pump();
    }
    uint8_t rs485_buf[64];
    uint16_t rs485_len = BSP_RS485_Read(rs485_buf, sizeof(rs485_buf));
    if (rs485_len > 0) {
        if (dwin_recovery_state != DWIN_RECOVERY_RUNNING) {
            /* Ignore stale/boot bytes so a panel restart cannot replay a
             * touch command as an unintended charger action. */
            dwin_rx_suppressed_bytes += rs485_len;
        } else {
            DWIN_ParseRX(rs485_buf, rs485_len);
            AppDwinDebug_CaptureRx(rs485_buf, rs485_len);
        }
    }

    /* (2) Button handling with debounce -- single toggle button (BUTTON_1/
     * PA15): one press-release cycle acts on the debounced rising edge,
     * decided by the status the button is currently showing (see
     * app_action_button()), not by which physical button was pressed.
     * Shares that logic with the DWIN screen button (DWIN_OnActionButton).
     * Confirmed with user 2026-08-29: hardware only needs 1 button for
     * this (BUTTON_2/PD2 dropped from this flow, BSP_BTN_STOP left intact
     * in bsp_gpio.c/.h in case it's repurposed later). */
    {
        uint8_t start_raw = read_btn_start();

        if (start_raw != btn_start_db) {
            btn_start_db = start_raw;
            btn_start_last = now;
        }
        if ((now - btn_start_last) > APP_BTN_DEBOUNCE_MS) {
            if (btn_start_db != btn_start_prev) {
                btn_start_prev = btn_start_db;
                if (btn_start_prev) {
                    app_action_button(dwin_current_status(), now);
                }
            }
        }
    }

    /* (3) LED update */
    if ((now - last_led_tick) >= APP_LED_INTERVAL_MS) {
        last_led_tick = now;

        CHG_LIB_SystemSummary_t sum;
        CHG_LIB_GetSystemSummary(&sum);

        /* LED_RUN: co module online & dang sac */
        if (sum.modules_online > 0 && PC_Protocol_IsCharging()) {
            led_run_on();
        } else {
            led_run_off();
        }

        /* PC7 is the power indicator and is intentionally independent from
         * alarms, module communication and charger state. */
    }

    /* (4) DWIN HMI refresh -- one field group per 20ms tick (see
     * DWIN_UpdateData scatter). RX is drained above, near the top of the
     * loop. */
    if ((now - last_dwin_tick) >= DWIN_UPDATE_INTERVAL_MS) {
        last_dwin_tick = now;

        if (dwin_recovery_state != DWIN_RECOVERY_WAIT_BOOT) {
            if (dwin_recovery_state == DWIN_RECOVERY_RESTORE_PENDING) {
                dwin_send_identity_and_page(now);
                dwin_recovery_state = DWIN_RECOVERY_RUNNING;
                last_dwin_reset_tick = now;
                dwin_reset_completed_count++;
                dwin_replay_count++;
                LOG("DWIN: software reset restored page=%u complete=%lu replay=%lu suppressed=%lu\r\n",
                    (unsigned)dwin_page_after_panel_reset(),
                    (unsigned long)dwin_reset_completed_count,
                    (unsigned long)dwin_replay_count,
                    (unsigned long)dwin_rx_suppressed_bytes);
            }

        ChargeCtrlView_t cc_view;
        ChargeController_GetView(&cc_view);
        AlarmView_t av;
        Alarm_GetView(&av);

        bool controller_fault = (cc_view.state == CHARGE_CTRL_STATE_FAULT) ||
                                (cc_view.fault_flags != CHARGE_CTRL_FAULT_NONE);
        bool runtime_alarm = dwin_precharge_seen_active &&
                             (av.highest_action >= ALARM_ACT_STOP);

        /* A pre-charge error is a presentation latch only. The controller
         * still owns the safety stop and relay sequencing; this latch keeps
         * the operator on Page 07 long enough to read the existing alarm code. */
        if (dwin_precharge_session &&
            (controller_fault || runtime_alarm || dwin_precharge_error_hold)) {
            if (!dwin_precharge_error_hold) {
                dwin_precharge_error_hold = true;
                dwin_precharge_error_code = av.worst_code;
            } else if (dwin_precharge_error_code == ALARM_NONE &&
                       av.worst_code != ALARM_NONE) {
                dwin_precharge_error_code = av.worst_code;
            }
        }

        /* Leaving PRECHARGE normally closes the privileged panel session.
         * Fault/protection is the exception: remain on Page 07 until the
         * operator chooses Reset or Back. User Stop and normal completion
         * still use the pending route after the controller is safe. */
        if (cc_view.state == CHARGE_CTRL_STATE_PRECHARGE) {
            dwin_precharge_seen_active = true;
        }
        if (dwin_precharge_seen_active && cc_view.state != CHARGE_CTRL_STATE_PRECHARGE) {
            dwin_precharge_seen_active = false;
            if (controller_fault || (av.highest_action >= ALARM_ACT_STOP)) {
                dwin_precharge_error_hold = true;
                if (dwin_precharge_error_code == ALARM_NONE) {
                    dwin_precharge_error_code = av.worst_code;
                }
            } else {
                dwin_end_precharge_session();
                dwin_precharge_exit_pending = true;
            }
        }
        if (dwin_precharge_exit_pending &&
            cc_view.state == CHARGE_CTRL_STATE_IDLE &&
            !dwin_precharge_error_hold) {
            DWIN_SetPage(DWIN_PAGE_DASH);
            DWIN_ForceFullRefresh();
            dwin_precharge_exit_pending = false;
        }

        CHG_LIB_SystemSummary_t sum;
        CHG_LIB_GetSystemSummary(&sum);

        BMS_View_t bms;
        BMS_GetView(&bms);

        /* One-shot once the panel has booted: identity strings + land on the
         * dashboard page (DWIN_SetPage self-suppresses, so this never fights
         * the operator navigating to Setting/Alarm via the footer). */
        if (!dwin_boot_sent && now > DWIN_BOOT_DELAY_MS) {
            dwin_send_identity_and_page(now);
            /* Panel just got its page + strings -- push every data field to
             * it now (the first scatter cycle ran at t~=400ms, before the
             * panel was listening). */
            dwin_boot_sent = true;
            last_dwin_reset_tick = now;
            /* Fires exactly once -- safe outside the 50ms LOG-blocking budget. */
            LOG("DWIN: HMI init sent (HW/FW/ID strings + dashboard page).\r\n");
        }

        /* Heartbeat: re-send every field periodically so a panel that booted
         * late, or brown-out-rebooted, catches up without needing a value to
         * change. Diff-suppressed in between. */
        if (dwin_boot_sent) {
            dwin_check_and_send_identity_update();
            if ((now - last_dwin_full_tick) >= DWIN_HEARTBEAT_INTERVAL_MS) {
                last_dwin_full_tick = now;
                DWIN_ForceFullRefresh();
            }
        }

        DWIN_SystemData_t dd;
        memset(&dd, 0, sizeof(dd));

        /* OUTPUT DC: show measured module values only when at least one
         * module is online. Never use zero-init as a validity indication. */
        {
            if (sum.modules_online > 0U && isfinite(sum.voltage) && sum.voltage >= 0.0f &&
                isfinite(sum.total_current) && sum.total_current >= 0.0f) {
                if (!dwin_format_fixed(dd.dc_voltage_text, sizeof(dd.dc_voltage_text), sum.voltage, 1U, ""))
                    dwin_set_unavailable(dd.dc_voltage_text, sizeof(dd.dc_voltage_text));
                if (!dwin_format_fixed(dd.dc_current_text, sizeof(dd.dc_current_text), sum.total_current, 1U, ""))
                    dwin_set_unavailable(dd.dc_current_text, sizeof(dd.dc_current_text));
                if (!dwin_format_fixed(dd.dc_power_text, sizeof(dd.dc_power_text),
                                       (sum.voltage * sum.total_current) / 1000.0f, 1U, ""))
                    dwin_set_unavailable(dd.dc_power_text, sizeof(dd.dc_power_text));
            } else {
                dwin_set_unavailable(dd.dc_voltage_text, sizeof(dd.dc_voltage_text));
                dwin_set_unavailable(dd.dc_current_text, sizeof(dd.dc_current_text));
                dwin_set_unavailable(dd.dc_power_text, sizeof(dd.dc_power_text));
            }
        }

        /* Page 07 intentionally reuses the same validated system summary as
         * Home. Its Text Displays therefore show exactly the total output
         * voltage/current the operator sees on the dashboard. */
        memcpy(dd.precharge_voltage_text, dd.dc_voltage_text,
               sizeof(dd.precharge_voltage_text));
        memcpy(dd.precharge_current_text, dd.dc_current_text,
               sizeof(dd.precharge_current_text));
        if (dwin_precharge_error_hold || cc_view.state == CHARGE_CTRL_STATE_FAULT ||
            (dwin_precharge_session && av.highest_action >= ALARM_ACT_STOP)) {
            dd.precharge_status_mode = DWIN_PRECHARGE_STATUS_ERROR;
            dd.precharge_btn_mode = DWIN_PRECHARGE_BTN_RESET;
        } else if (cc_view.state == CHARGE_CTRL_STATE_PRECHARGE) {
            dd.precharge_status_mode = DWIN_PRECHARGE_STATUS_ACTIVE;
            dd.precharge_btn_mode = DWIN_PRECHARGE_BTN_STOP;
        } else {
            dd.precharge_status_mode = DWIN_PRECHARGE_STATUS_READY;
            dd.precharge_btn_mode = DWIN_PRECHARGE_BTN_START;
        }

        /* Battery telemetry: online is necessary but not sufficient for every
         * field. An invalid individual value remains unavailable on DWIN. */
        if (bms.online) {
            if (bms.soc <= 100U)
                (void)snprintf(dd.soc_text, sizeof(dd.soc_text), "%u%%", (unsigned)bms.soc);
            else
                dwin_set_soc_unavailable(dd.soc_text, sizeof(dd.soc_text));

            if (bms.batt_voltage < 0.0f ||
                !dwin_format_fixed(dd.bat_pack_volt_text, sizeof(dd.bat_pack_volt_text), bms.batt_voltage, 1U, ""))
                dwin_set_unavailable(dd.bat_pack_volt_text, sizeof(dd.bat_pack_volt_text));

            /* Home displays the max cell voltage in volts with millivolt
             * precision. Keep the conversion integer-based so 3315 mV is
             * transmitted exactly as "3.315" without float printf support. */
            if (bms.max_cell_volt == 0U ||
                snprintf(dd.bat_cell_volt_text, sizeof(dd.bat_cell_volt_text),
                         "%u.%03u", (unsigned)(bms.max_cell_volt / 1000U),
                         (unsigned)(bms.max_cell_volt % 1000U)) < 0)
                dwin_set_unavailable(dd.bat_cell_volt_text, sizeof(dd.bat_cell_volt_text));

            if (!dwin_format_fixed(dd.temp_battery_text, sizeof(dd.temp_battery_text), bms.max_cell_temp, 1U, ""))
                dwin_set_unavailable(dd.temp_battery_text, sizeof(dd.temp_battery_text));

        } else {
            dwin_set_soc_unavailable(dd.soc_text, sizeof(dd.soc_text));
            dwin_set_unavailable(dd.bat_pack_volt_text, sizeof(dd.bat_pack_volt_text));
            dwin_set_unavailable(dd.bat_cell_volt_text, sizeof(dd.bat_cell_volt_text));
            dwin_set_unavailable(dd.temp_battery_text, sizeof(dd.temp_battery_text));
        }

        /* Home battery row 3: active charge mode ("NORMAL" or "FAST") */
        {
            const char *mode_str = (ChargeCycleConfig_GetActiveMode() == CHARGE_MODE_FAST) ? "FAST" : "NORMAL";
            strncpy(dd.bat_cap_text, mode_str, sizeof(dd.bat_cap_text) - 1U);
            dd.bat_cap_text[sizeof(dd.bat_cap_text) - 1U] = '\0';
        }
        DWIN_SetSocColor(dwin_soc_color_for_bms(&bms));

        dwin_set_unavailable(dd.ac_l1_text, sizeof(dd.ac_l1_text));
        dwin_set_unavailable(dd.ac_l2_text, sizeof(dd.ac_l2_text));
        dwin_set_unavailable(dd.ac_l3_text, sizeof(dd.ac_l3_text));
        for (uint8_t i = 0U; i < CHG_LIB_GetModuleCount(); i++) {
            CHG_LIB_ModuleView_t mv;
            if (!CHG_LIB_GetModuleView(i, &mv) || !mv.enabled || !mv.online) continue;
            if (mv.ac_phase_a_voltage < 0.0f ||
                !dwin_format_fixed(dd.ac_l1_text, sizeof(dd.ac_l1_text), mv.ac_phase_a_voltage, 0U, ""))
                dwin_set_unavailable(dd.ac_l1_text, sizeof(dd.ac_l1_text));
            if (mv.ac_phase_b_voltage < 0.0f ||
                !dwin_format_fixed(dd.ac_l2_text, sizeof(dd.ac_l2_text), mv.ac_phase_b_voltage, 0U, ""))
                dwin_set_unavailable(dd.ac_l2_text, sizeof(dd.ac_l2_text));
            if (mv.ac_phase_c_voltage < 0.0f ||
                !dwin_format_fixed(dd.ac_l3_text, sizeof(dd.ac_l3_text), mv.ac_phase_c_voltage, 0U, ""))
                dwin_set_unavailable(dd.ac_l3_text, sizeof(dd.ac_l3_text));
            break;
        }

        /* Dashboard TEMP panel, 0.1 degC (x10) per DWIN DGUS 0.0 format:
         *  BATTERY = BMS max cell temp   (set above when bms.online)
         *  CHARGE  = hottest DC-DC stage across online modules, from CAN
         *  JACK    = hottest of the 4 connector NTCs (PA0..PA3) */
        {
            float max_dcdc = 0.0f;
            bool have_dcdc = false;
            uint8_t nmod = CHG_LIB_GetModuleCount();
            for (uint8_t i = 0; i < nmod; i++) {
                CHG_LIB_ModuleView_t tv;
                if (CHG_LIB_GetModuleView(i, &tv) && tv.online &&
                    isfinite(tv.temp_dcdc) && (!have_dcdc || tv.temp_dcdc > max_dcdc)) {
                    max_dcdc = tv.temp_dcdc;
                    have_dcdc = true;
                }
            }
            if (!have_dcdc || !dwin_format_fixed(dd.temp_charge_text, sizeof(dd.temp_charge_text), max_dcdc, 1U, ""))
                dwin_set_unavailable(dd.temp_charge_text, sizeof(dd.temp_charge_text));
        }
        {
            float jack_c = 0.0f;
            bool jack_valid = false;
            for (uint8_t i = 0; i < 4; i++) {
                float t = BSP_ADC_GetTempC(i);
                if (isfinite(t) && (!jack_valid || t > jack_c)) {
                    jack_c = t;
                    jack_valid = true;
                }
            }
            if (!jack_valid) dwin_set_unavailable(dd.temp_jack_text, sizeof(dd.temp_jack_text));
            else if (!dwin_format_fixed(dd.temp_jack_text, sizeof(dd.temp_jack_text), jack_c, 1U, ""))
                dwin_set_unavailable(dd.temp_jack_text, sizeof(dd.temp_jack_text));
        }

        dd.status_icon = dwin_status_from_state(&cc_view, &sum);
        dd.btn_mode    = dwin_btn_mode_from_status(dd.status_icon);
        dd.uptime_s    = now / 1000U;

        /* Charge duration: tracks elapsed time from charge start to stop.
         * Retains the final duration for 60 seconds after charge ends so the
         * operator can inspect it, then returns the footer to the live RTC. */
        static uint32_t s_charge_start_tick = 0U;
        static uint32_t s_charge_duration_s = 0U;
        static uint32_t s_charge_stop_tick = 0U;
        static bool     s_was_charging = false;
        static bool     s_hold_last_duration = false;
        bool is_charging = (dd.status_icon == DWIN_STATUS_CHARGING ||
                            dd.status_icon == DWIN_STATUS_STARTING);
        if (cc_view.state == CHARGE_CTRL_STATE_DELAY) {
            dd.charge_duration_s = cc_view.delay_remaining_s;
            dd.footer_time_str[0] = '\0';
            s_was_charging = false;
        } else if (is_charging) {
            if (!s_was_charging) {
                s_charge_start_tick = now;
                s_charge_duration_s = 0U;
                s_hold_last_duration = false;
                s_was_charging = true;
            } else {
                s_charge_duration_s = (now - s_charge_start_tick) / 1000U;
            }
            dd.charge_duration_s = s_charge_duration_s;
            dd.footer_time_str[0] = '\0'; /* Format charge duration */
        } else {
            if (s_was_charging) {
                /* The STOPPING state is intentionally still charging in the
                 * HMI mapping; this edge is therefore the first stable
                 * non-charging state after the session has ended. */
                s_charge_stop_tick = now;
                s_hold_last_duration = true;
                s_was_charging = false;
            }

            if (s_hold_last_duration &&
                (uint32_t)(now - s_charge_stop_tick) < CHARGE_DURATION_HOLD_MS) {
                dd.charge_duration_s = s_charge_duration_s;
                dd.footer_time_str[0] = '\0';
            } else {
                s_hold_last_duration = false;
                dd.charge_duration_s = 0U;
                if (BSP_RTC_IsTimeValid()) {
                    /* After the hold window, show the live real-time clock. */
                    BSP_RTC_FormatTime(dd.footer_time_str, sizeof(dd.footer_time_str));
                } else {
                    (void)snprintf(dd.footer_time_str, sizeof(dd.footer_time_str), "00:00:00");
                }
            }
        }

        /* Energy & capacity accumulator */
        if (ChargeEnergyStorage_TakeResetRequest()) {
            s_total_charged_ah = 0.0f;
            s_total_energy_kwh = 0.0f;
            s_last_energy_tick = now;
        }
        if (s_last_energy_tick == 0U) {
            s_last_energy_tick = now;
        }
        uint32_t dt_ms = now - s_last_energy_tick;
        s_last_energy_tick = now;
        if (is_charging && dt_ms > 0U && dt_ms < 500U) {
            float hours = (float)dt_ms * (1.0f / 3600000.0f);
            float cur_f = (isfinite(sum.total_current) && sum.total_current > 0.0f)
                              ? sum.total_current : 0.0f;
            float volt_f = (isfinite(sum.voltage) && sum.voltage > 0.0f)
                              ? sum.voltage : 0.0f;
            s_total_charged_ah += cur_f * hours;
            s_total_energy_kwh += (cur_f * volt_f * 0.001f) * hours;
        }
        dd.total_charged_ah_x10 = (uint32_t)(s_total_charged_ah * 10.0f);
        dd.total_energy_kwh_x10 = (uint32_t)(s_total_energy_kwh * 10.0f);
        ChargeEnergyStorage_Process(now, is_charging, s_total_charged_ah,
                                    s_total_energy_kwh);

        /* Topbar fault code: "0000" if normal, worst code (e.g. "E006") if fault active */
        if (av.active_count == 0U && av.latched_mask == 0U) {
            if (dwin_precharge_error_hold && dwin_precharge_error_code != ALARM_NONE) {
                const char *c_str = DWIN_Alarm_GetCodeString(dwin_precharge_error_code);
                strncpy(dd.topbar_fault_code, c_str, sizeof(dd.topbar_fault_code) - 1U);
            } else if (cc_view.state == CHARGE_CTRL_STATE_FAULT &&
                       ((cc_view.fault_flags & CHARGE_CTRL_FAULT_BMS_ALARM) != 0U ||
                        cc_view.stop_reason == CHARGE_STOP_BMS_ALARM)) {
                const char *c_str = DWIN_Alarm_GetCodeString(ALARM_BMS_TEMP_HIGH_CHG);
                strncpy(dd.topbar_fault_code, c_str, sizeof(dd.topbar_fault_code) - 1U);
            } else {
                strncpy(dd.topbar_fault_code, "0000", sizeof(dd.topbar_fault_code) - 1U);
            }
        } else {
            const char *c_str = DWIN_Alarm_GetCodeString(av.worst_code);
            strncpy(dd.topbar_fault_code, c_str, sizeof(dd.topbar_fault_code) - 1U);
        }

        /* Synchronize alarm event log with DWIN 4-row FIFO ring buffer.
         * The RAM log count saturates at ALARM_LOG_DEPTH, so use its
         * generation counter to detect writes after the ring is full. */
        static uint32_t s_last_log_sequence = 0U;
        AlarmLogEntry_t log_entries[ALARM_LOG_DEPTH];
        uint8_t log_count = Alarm_GetLog(log_entries, ALARM_LOG_DEPTH);
        uint32_t log_sequence = Alarm_GetLogSequence();
        uint32_t new_events = log_sequence - s_last_log_sequence;
        if (new_events > ALARM_LOG_DEPTH) {
            new_events = ALARM_LOG_DEPTH;
        }
        if (new_events > 0U) {
            /* Alarm_GetLog returns newest-first: index 0 is newest.
             * Push oldest-of-new-batch first so newest ends up at row 0. */
            for (int8_t i = (int8_t)new_events - 1; i >= 0; i--) {
                if (log_entries[i].event == 1U) { /* Raised */
                    char time_buf[10];
                    if (BSP_RTC_IsTimeValid()) {
                        BSP_RTC_FormatTime(time_buf, sizeof(time_buf));
                    } else {
                        uint32_t sec = log_entries[i].uptime_ms / 1000U;
                        uint32_t h = (sec / 3600U) % 24U;
                        uint32_t m = (sec % 3600U) / 60U;
                        uint32_t s = sec % 60U;
                        (void)snprintf(time_buf, sizeof(time_buf), "%02u:%02u:%02u",
                                       (unsigned)h, (unsigned)m, (unsigned)s);
                    }

                    AlarmCode_t c = (AlarmCode_t)log_entries[i].code;
                    const char *c_str = DWIN_Alarm_GetCodeString(c);
                    uint8_t d_len = 0;
                    const uint16_t *d_utf16 = DWIN_Alarm_GetDescUtf16(c, &d_len);

                    DWIN_Alarm_Push(time_buf, c_str, d_utf16, d_len);
                }
            }
        }
        (void)log_count; /* Snapshot size is used to bound available entries. */
        s_last_log_sequence = log_sequence;

        DWIN_UpdateData(&dd);
    }
        }

    /* (5) Refresh IWDG — main loop only, never in ISR (~1s timeout) */
    MX_IWDG_Refresh();
}

/* The DGUS button uploads a fixed keycode on VP_SYS_BTN_KEY (0x1043);
 * `keyval` only means "pressed". app_action_button() decides what to do
 * from the status the button is currently showing, same as the physical
 * PA15 button. The button-label icon is a separate MCU-owned VP
 * (VP_SYS_BTN_ICON, 0x1042); write the fresh label straight away so it
 * flips without waiting for the next scatter cycle. */
void DWIN_OnActionButton(uint16_t keyval)
{
    uint32_t now = BSP_GetTick();
    uint16_t status = dwin_current_status();

    LOG("DWIN: button press (keyval=%u status=%u)\r\n",
        (unsigned)keyval, (unsigned)status);
    app_action_button(status, now);

    {
        uint16_t btn = dwin_btn_mode_from_status(dwin_current_status());
        DWIN_SendWords(VP_SYS_BTN_ICON, &btn, 1);
    }
}

void DWIN_OnKeyEvent(uint16_t vp, uint16_t keyval)
{
    uint32_t now = BSP_GetTick();
    static uint16_t s_cfg_hours = 2U;
    static uint16_t s_cfg_minutes = 30U;

    LOG("DWIN: key event (vp=0x%04X keyval=0x%04X)\r\n", (unsigned)vp, (unsigned)keyval);

    if (vp == VP_TIME_MODE_KEY) {
        if (keyval == DWIN_SETTING_KEY_LOGIN) {
            dwin_open_login();
        } else {
            /* Open Time & Mode config page for any keyval (e.g. 0x0001) */
            ChargeCycleConfig_t cfg;
            ChargeCycleConfig_Get(&cfg);
            s_cfg_hours = cfg.delay_hours;
            s_cfg_minutes = cfg.delay_minutes;
            uint16_t target_page = DWIN_PAGE_CONFIG_FAST_OFF;
            if (cfg.charge_mode == 0U) {
                target_page = (cfg.delay_enabled != 0U) ? DWIN_PAGE_CONFIG_FAST_ON : DWIN_PAGE_CONFIG_FAST_OFF;
            } else {
                target_page = (cfg.delay_enabled != 0U) ? DWIN_PAGE_CONFIG_NORM_ON : DWIN_PAGE_CONFIG_NORM_OFF;
            }
            LOG("DWIN: Open Time & Mode page=%u (mode=%u delay=%u %02u:%02u)\r\n",
                (unsigned)target_page, (unsigned)cfg.charge_mode, (unsigned)cfg.delay_enabled,
                (unsigned)s_cfg_hours, (unsigned)s_cfg_minutes);
            DWIN_InvalidateSyncState();
            DWIN_SetPage(target_page);
            uint16_t time_vals[2] = { s_cfg_hours, s_cfg_minutes };
            DWIN_SendWords(VP_CFG_HOURS, time_vals, 2U);
        }
        return;
    }

    if (vp == VP_CFG_HOURS) {
        if (keyval <= 99U) {
            s_cfg_hours = keyval;
            ChargeCycleConfig_t cfg;
            ChargeCycleConfig_Get(&cfg);
            if (cfg.delay_hours != s_cfg_hours) {
                cfg.delay_hours = s_cfg_hours;
                ChargeCycleConfig_Set(&cfg);
            }
        }
        return;
    }

    if (vp == VP_CFG_MINUTES) {
        if (keyval <= 59U) {
            s_cfg_minutes = keyval;
            ChargeCycleConfig_t cfg;
            ChargeCycleConfig_Get(&cfg);
            if (cfg.delay_minutes != s_cfg_minutes) {
                cfg.delay_minutes = s_cfg_minutes;
                ChargeCycleConfig_Set(&cfg);
            }
        }
        return;
    }

    if (vp == VP_CFG_APPLY_KEY) {
        uint8_t target_mode = CHARGE_MODE_FAST;
        uint8_t delay_en = 0U;
        if (keyval == DWIN_CFG_KEY_FAST_ON) {
            target_mode = CHARGE_MODE_FAST;
            delay_en = 1U;
        } else if (keyval == DWIN_CFG_KEY_FAST_OFF) {
            target_mode = CHARGE_MODE_FAST;
            delay_en = 0U;
        } else if (keyval == DWIN_CFG_KEY_NORM_ON) {
            target_mode = CHARGE_MODE_NORMAL;
            delay_en = 1U;
        } else if (keyval == DWIN_CFG_KEY_NORM_OFF) {
            target_mode = CHARGE_MODE_NORMAL;
            delay_en = 0U;
        }

        ChargeCycleConfig_t cfg;
        ChargeCycleConfig_GetProfile(target_mode, &cfg);
        cfg.charge_mode = target_mode;
        cfg.delay_enabled = delay_en;
        cfg.delay_hours = s_cfg_hours;
        cfg.delay_minutes = s_cfg_minutes;

        /* Apply session config in RAM for the upcoming charge cycle */
        ChargeCycleConfig_SetProfile(target_mode, &cfg);
        ChargeCycleConfig_SetActiveMode(target_mode);

        /* Save delay time preferences to flash (Option A) while keeping startup default safe (NO DELAY) */
        ChargeCycleConfig_t flash_cfg = cfg;
        flash_cfg.delay_enabled = 0U;
        (void)ChargeCycleStorage_SaveProfile(target_mode, &flash_cfg);

        LOG("DWIN: Session charge config applied (mode=%u delay=%u %02u:%02u)\r\n",
            (unsigned)cfg.charge_mode, (unsigned)cfg.delay_enabled,
            (unsigned)cfg.delay_hours, (unsigned)cfg.delay_minutes);
        return;
    }

    if (vp == VP_LOGIN_KEY) {
        if (keyval >= DWIN_LOGIN_KEY_DIGIT_0 && keyval <= DWIN_LOGIN_KEY_DIGIT_9) {
            if (dwin_pin_length < 6U) {
                dwin_pin_digits[dwin_pin_length++] =
                    (char)('0' + (keyval - DWIN_LOGIN_KEY_DIGIT_0));
                dwin_login_show_mask();
            }
        } else if (keyval == DWIN_LOGIN_KEY_DELETE) {
            if (dwin_pin_length > 0U) {
                dwin_pin_length--;
                dwin_pin_digits[dwin_pin_length] = '\0';
                dwin_login_show_mask();
            }
        } else if (keyval == DWIN_LOGIN_KEY_BACK) {
            dwin_end_precharge_session();
            DWIN_SetPage(DWIN_PAGE_DASH);
        } else if (keyval == DWIN_LOGIN_KEY_OK) {
            ChargeCycleConfig_t cfg;
            ChargeCycleConfig_Get(&cfg);
            if (dwin_pin_length == 6U && dwin_login_pin_value() == cfg.admin_pin) {
                dwin_login_clear();
                dwin_precharge_session = true;
                dwin_open_precharge();
            } else {
                /* Never log PINs or attempted values. */
                dwin_login_clear();
            }
        }
        return;
    }

    if (vp != VP_PRECHARGE_ACTION_KEY || !dwin_precharge_session) {
        return;
    }

    if (keyval == DWIN_PRECHARGE_KEY_ACTION) {
        ChargeCtrlView_t view;
        AlarmView_t alarm_view;
        ChargeController_GetView(&view);
        Alarm_GetView(&alarm_view);

        if (dwin_precharge_error_hold || view.state == CHARGE_CTRL_STATE_FAULT ||
            alarm_view.highest_action >= ALARM_ACT_STOP) {
            /* RESET is an acknowledge/retry request, never a forced clear.
             * A persistent root cause keeps the page in ERROR. */
            Alarm_Acknowledge(now);
            ChargeController_GetView(&view);

            if (view.state == CHARGE_CTRL_STATE_FAULT) {
                (void)ChargeController_ResetEmergencyStop(now);
                if (ChargeController_ResetFaultIfSafe(now)) {
                    dwin_precharge_error_hold = false;
                    dwin_precharge_error_code = ALARM_NONE;
                }
            } else if (view.state == CHARGE_CTRL_STATE_PRECHARGE) {
                /* A STOP-level alarm may be observed before the controller
                 * processes its stop request. Do not let RESET resume output. */
                ChargeController_StopPrecharge(now);
            } else {
                Alarm_GetView(&alarm_view);
                if (view.state == CHARGE_CTRL_STATE_IDLE &&
                    alarm_view.highest_action < ALARM_ACT_STOP) {
                    dwin_precharge_error_hold = false;
                    dwin_precharge_error_code = ALARM_NONE;
                }
            }
        } else if (view.state == CHARGE_CTRL_STATE_PRECHARGE) {
            ChargeController_StopPrecharge(now);
            dwin_end_precharge_session();
            dwin_precharge_exit_pending = true;
        } else {
            if (ChargeController_StartPrecharge(CHARGE_CTRL_OWNER_DWIN, now)) {
                uint16_t status = DWIN_PRECHARGE_STATUS_ACTIVE;
                uint16_t button = DWIN_PRECHARGE_BTN_STOP;
                dwin_precharge_seen_active = true;
                DWIN_SendWords(VP_PRECHARGE_STATUS_ICON, &status, 1U);
                DWIN_SendWords(VP_PRECHARGE_BTN_ICON, &button, 1U);
            } else {
                /* Start failure is shown on Page 07 by the normal HMI
                 * refresh (controller FAULT + existing alarm code). */
                dwin_precharge_exit_pending = false;
            }
        }
    } else if (keyval == DWIN_PRECHARGE_KEY_BACK) {
        ChargeCtrlView_t view;
        ChargeController_GetView(&view);
        if (view.state == CHARGE_CTRL_STATE_PRECHARGE) {
            ChargeController_StopPrecharge(now);
            dwin_end_precharge_session();
            dwin_precharge_exit_pending = true;
        } else if (view.state == CHARGE_CTRL_STATE_STOPPING ||
                   view.relay_should_close) {
            /* Preserve the page until the existing stop/relay-settle path
             * reports that the output is safe. */
            dwin_end_precharge_session();
            dwin_precharge_exit_pending = true;
        } else {
            dwin_end_precharge_session();
            DWIN_SetPage(DWIN_PAGE_DASH);
            DWIN_ForceFullRefresh();
        }
    }
}
