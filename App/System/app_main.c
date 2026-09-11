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

#include "app_version.h"

/* ============== Configuration ============== */

#define APP_PROCESS_INTERVAL_MS      20U     /* Control loop period */
#define APP_LED_INTERVAL_MS          100U    /* LED update period */
#define APP_BTN_DEBOUNCE_MS          50U     /* Button debounce */

#define DWIN_BOOT_DELAY_MS           3000U   /* Wait for DWIN panel to finish boot */
#define DWIN_HEARTBEAT_INTERVAL_MS   5000U   /* Periodic force-full-refresh interval */
#define DWIN_UPDATE_INTERVAL_MS      50U     /* Field scatter cadence (50ms per group) */

#include <string.h>
#include <math.h>
#include <stdio.h>

/* ============== Private state ============== */

static uint32_t last_process_tick   = 0;
static uint32_t last_led_tick       = 0;
static uint32_t last_dwin_tick      = 0;
static uint32_t last_dwin_full_tick = 0;
static uint32_t last_main_log       = 0;
/* Button debounce -- single toggle button (BUTTON_1/PA15), see App_Loop()
 * "(2) Button handling" for the state-decides-direction logic. BUTTON_2
 * (PD2) is no longer read here -- confirmed with user 2026-08-29, hardware
 * only needs 1 button. */
static uint32_t btn_start_last    = 0;
static uint8_t  btn_start_prev    = 0;
static uint8_t  btn_start_db      = 0;
/* One-shot: identity strings + initial page pushed to the DWIN panel once
 * it has had time to boot (the panel comes up slower than the MCU). */
static bool     dwin_boot_sent    = false;
static double   s_total_charged_ah;
static double   s_total_energy_kwh;
static uint32_t s_last_energy_tick;

/* Keep the last charge duration visible briefly after a session ends. This
 * gives the operator time to read the result before the footer returns to the
 * live RTC clock, without blocking the main loop or persisting a transient UI
 * value. */
#define CHARGE_DURATION_HOLD_MS 60000U

/* ============== LED control ============== */

static void led_run_on(void)   { BSP_LED_On(BSP_LED_RUN); }
static void led_run_off(void)  { BSP_LED_Off(BSP_LED_RUN); }
static void led_fault_on(void) { BSP_LED_On(BSP_LED_FAULT); }
static void led_fault_off(void){ BSP_LED_Off(BSP_LED_FAULT); }

static void dwin_set_unavailable(char *text, size_t text_size)
{
    if (text == NULL || text_size == 0U) return;
    memset(text, 0, text_size);
    if (text_size > 3U) {
        memcpy(text, "---", 3U);
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
    /* Fault first: any active fault flag, the FAULT state, or an alarm at
     * STOP/ESTOP level (covers the debounce/ack window before the controller
     * itself transitions to FAULT) -> ERROR. */
    if (cc->state == CHARGE_CTRL_STATE_FAULT ||
        cc->fault_flags != CHARGE_CTRL_FAULT_NONE) {
        return DWIN_STATUS_ERROR;
    }
    {
        AlarmView_t av;
        Alarm_GetView(&av);
        if (av.highest_action >= ALARM_ACT_STOP) {
            return DWIN_STATUS_ERROR;
        }
    }

    switch (cc->state) {
        case CHARGE_CTRL_STATE_RUNNING:
            /* relay latched closed == real current is flowing */
            return cc->relay_should_close ? DWIN_STATUS_CHARGING
                                          : DWIN_STATUS_STARTING;
        case CHARGE_CTRL_STATE_READY:
            return DWIN_STATUS_STARTING;
        case CHARGE_CTRL_STATE_STOPPING:
            return DWIN_STATUS_CHARGING; /* transient, keep showing activity */
        case CHARGE_CTRL_STATE_IDLE:
        default:
            break;
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
 * one press starts / stops / acknowledges, decided by the state the button
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
            LOG("App: button (ERROR) -> clear fault\r\n");
            Alarm_Acknowledge(now);      /* clear latched alarms whose cause is gone */
            ChargeController_Stop(now);   /* from FAULT: clears fault -> IDLE */
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
    led_fault_off();

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

    /* Start CAN1 + CAN2 (filter + interrupt) */
    LOG("App_Init: Starting CAN bus...\r\n");
    if (!BSP_CAN_Start()) {
        LOG("App_Init: ERROR - Could not start CAN bus!\r\n");
        led_fault_on();
    } else {
        LOG("App_Init: CAN bus started successfully.\r\n");
    }

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
     * Same ISR context and call timing as before; only the include graph
     * changed. */
    BSP_CAN_SetChargerRxHandler(CHG_LIB_FeedCanFrame);
    BSP_CAN_SetBmsRxHandler(BMS_FeedFrame);

    /* Initialize debug protocol */
    DebugProtocol_Init();

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

    LOG("App_Init: Initialization complete.\r\n");
}

/* ============== Main Loop ============== */

void App_Loop(void)
{
    uint32_t now = BSP_GetTick();

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
    
    /* DWIN HMI: drain RX + parse; pump any queued bench-debug frame first
     * (no-op unless CHG_DEBUG_DWIN). */
    AppDwinDebug_Pump();
    uint8_t rs485_buf[64];
    uint16_t rs485_len = BSP_RS485_Read(rs485_buf, sizeof(rs485_buf));
    if (rs485_len > 0) {
        DWIN_ParseRX(rs485_buf, rs485_len);
        AppDwinDebug_CaptureRx(rs485_buf, rs485_len);
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

        /* LED_FAULT: co loi hoac mat ket noi module, hoac alarm muc STOP/ESTOP
         * (bao gom ca loi controller/BMS/derived ma summary khong thay) */
        AlarmView_t av_led;
        Alarm_GetView(&av_led);
        if (sum.any_critical || sum.modules_fault > 0 ||
            av_led.highest_action >= ALARM_ACT_STOP) {
            led_fault_on();
        } else {
            led_fault_off();
        }
    }

    /* (4) DWIN HMI refresh -- one field group per 50ms tick (see
     * DWIN_UpdateData scatter). RX is drained above, near the top of the
     * loop. */
    if ((now - last_dwin_tick) >= DWIN_UPDATE_INTERVAL_MS) {
        last_dwin_tick = now;

        ChargeCtrlView_t cc_view;
        ChargeController_GetView(&cc_view);

        CHG_LIB_SystemSummary_t sum;
        CHG_LIB_GetSystemSummary(&sum);

        BMS_View_t bms;
        BMS_GetView(&bms);

        /* One-shot once the panel has booted: identity strings + land on the
         * dashboard page (DWIN_SetPage self-suppresses, so this never fights
         * the operator navigating to Setting/Alarm via the footer). */
        if (!dwin_boot_sent && now > DWIN_BOOT_DELAY_MS) {
            const char *hw_str = ChargeCycleConfig_GetHwRev();
            if (strncmp(hw_str, "HW ", 3) == 0) {
                hw_str += 3;
            } else if (strncmp(hw_str, "HW", 2) == 0) {
                hw_str += 2;
            }
            DWIN_SendSettingStrings(hw_str, FW_VERSION_STRING,
                                    ChargeCycleConfig_GetDeviceId());
            DWIN_SetPage(DWIN_PAGE_DASH);
            /* Panel just got its page + strings -- push every data field to
             * it now (the first scatter cycle ran at t~=400ms, before the
             * panel was listening). */
            DWIN_ForceFullRefresh();
            last_dwin_full_tick = now;
            dwin_boot_sent = true;
            /* Fires exactly once -- safe outside the 50ms LOG-blocking budget. */
            LOG("DWIN: HMI init sent (HW/FW/ID strings + dashboard page).\r\n");
        }

        /* Heartbeat: re-send every field periodically so a panel that booted
         * late, or brown-out-rebooted, catches up without needing a value to
         * change. Diff-suppressed in between. */
        if (dwin_boot_sent && (now - last_dwin_full_tick) >= DWIN_HEARTBEAT_INTERVAL_MS) {
            last_dwin_full_tick = now;
            DWIN_ForceFullRefresh();
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

            /* DWIN owns this field as text. Keep the BMS millivolt value
             * intact instead of converting to volts and rounding to two
             * decimal places (3315 mV must remain visible as "3315"). */
            if (bms.max_cell_volt == 0U ||
                snprintf(dd.bat_cell_volt_text, sizeof(dd.bat_cell_volt_text),
                         "%04u", (unsigned)bms.max_cell_volt) < 0)
                dwin_set_unavailable(dd.bat_cell_volt_text, sizeof(dd.bat_cell_volt_text));

            if (!dwin_format_fixed(dd.bat_cap_text, sizeof(dd.bat_cap_text),
                                   (float)bms.cap_remain * 0.1f, 1U, ""))
                dwin_set_unavailable(dd.bat_cap_text, sizeof(dd.bat_cap_text));

            if (!dwin_format_fixed(dd.temp_battery_text, sizeof(dd.temp_battery_text), bms.max_cell_temp, 1U, ""))
                dwin_set_unavailable(dd.temp_battery_text, sizeof(dd.temp_battery_text));

        } else {
            dwin_set_soc_unavailable(dd.soc_text, sizeof(dd.soc_text));
            dwin_set_unavailable(dd.bat_pack_volt_text, sizeof(dd.bat_pack_volt_text));
            dwin_set_unavailable(dd.bat_cell_volt_text, sizeof(dd.bat_cell_volt_text));
            dwin_set_unavailable(dd.bat_cap_text, sizeof(dd.bat_cap_text));
            dwin_set_unavailable(dd.temp_battery_text, sizeof(dd.temp_battery_text));
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
        if (is_charging) {
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
            s_total_charged_ah = 0.0;
            s_total_energy_kwh = 0.0;
            s_last_energy_tick = now;
        }
        if (s_last_energy_tick == 0U) {
            s_last_energy_tick = now;
        }
        uint32_t dt_ms = now - s_last_energy_tick;
        s_last_energy_tick = now;
        if (is_charging && dt_ms > 0U && dt_ms < 500U) {
            double hours = (double)dt_ms / 3600000.0;
            float cur_f = (isfinite(sum.total_current) && sum.total_current > 0.0f)
                              ? sum.total_current : 0.0f;
            float volt_f = (isfinite(sum.voltage) && sum.voltage > 0.0f)
                              ? sum.voltage : 0.0f;
            s_total_charged_ah += (double)cur_f * hours;
            s_total_energy_kwh += ((double)cur_f * (double)volt_f / 1000.0) * hours;
        }
        dd.total_charged_ah_x10 = (uint32_t)(s_total_charged_ah * 10.0);
        dd.total_energy_kwh_x10 = (uint32_t)(s_total_energy_kwh * 10.0);
        ChargeEnergyStorage_Process(now, is_charging, s_total_charged_ah,
                                    s_total_energy_kwh);

        /* Topbar fault code: "0000" if normal, worst code (e.g. "E006") if fault active */
        AlarmView_t av;
        Alarm_GetView(&av);
        if (av.active_count == 0U && av.latched_mask == 0U) {
            strncpy(dd.topbar_fault_code, "0000", sizeof(dd.topbar_fault_code) - 1U);
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
