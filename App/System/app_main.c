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
#include "charge_controller.h"
#include "chg_lib.h"
#include "chg_lib_can_backend.h"
#include "chg_lib_driver_lianming.h"
#include "chg_lib_driver_maxwell.h"
#include "chg_lib_driver_tonhe.h"
#include "pc_protocol.h"
#include "pc_debug_protocol.h"
#include "app_dwin_debug.h"
#include "debug_log.h"
#include "main.h"
#include "iwdg.h"

#include "bsp_gpio.h"
#include "bsp_sys.h"

/* ============== Configuration ============== */

#define APP_PROCESS_INTERVAL_MS 20      /* Control loop period */
#define APP_LED_INTERVAL_MS     100     /* LED update period */
#define APP_BTN_DEBOUNCE_MS     50      /* Button debounce */

#include <string.h>
#include <math.h>
#include <stdio.h>

/* ============== Private state ============== */

static uint32_t last_process_tick = 0;
static uint32_t last_led_tick     = 0;
static uint32_t last_dwin_tick    = 0;
static uint32_t last_main_log     = 0;
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

/* ============== LED control ============== */

static void led_run_on(void)   { BSP_LED_On(BSP_LED_RUN); }
static void led_run_off(void)  { BSP_LED_Off(BSP_LED_RUN); }
static void led_fault_on(void) { BSP_LED_On(BSP_LED_FAULT); }
static void led_fault_off(void){ BSP_LED_Off(BSP_LED_FAULT); }

/* ============== Button read ============== */

static uint8_t read_btn_start(void) { return BSP_BTN_IsPressed(BSP_BTN_START) ? 1 : 0; }

/* ============== DWIN state <-> screen icon mapping ==============
 *
 * Policy lives here in the composition root, not in Modules/hmi (which does
 * pure framing). The dashboard centre element is one status-box Variable
 * Icon (VP_SYS_STATUS_ICON, 6 states) plus one button-label Variable Icon
 * (VP_SYS_BTN_MODE, 4 modes). Neither is a safety interlock -- the relay /
 * charge-control logic reads fault_flags/state directly. */

static uint16_t dwin_status_from_state(const ChargeCtrlView_t *cc,
                                      const CHG_LIB_SystemSummary_t *sum)
{
    /* Fault first: any active fault flag or the FAULT state -> ERROR. */
    if (cc->state == CHARGE_CTRL_STATE_FAULT ||
        cc->fault_flags != CHARGE_CTRL_FAULT_NONE) {
        return DWIN_STATUS_ERROR;
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

    /* IDLE: distinguish "finished a cycle" from "nothing to do" / "no HW". */
    if (cc->stop_reason == CHARGE_STOP_VOLTAGE_REACHED ||
        cc->stop_reason == CHARGE_STOP_CELL_VOLTAGE_REACHED ||
        cc->stop_reason == CHARGE_STOP_SOC_REACHED) {
        return DWIN_STATUS_COMPLETE;
    }
    if (sum->modules_online == 0U) {
        return DWIN_STATUS_OFFLINE;
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
        case DWIN_STATUS_OFFLINE:
            return DWIN_BTN_DISABLED;
        case DWIN_STATUS_READY:
        default:
            return DWIN_BTN_START;
    }
}

/* Shared by the physical BUTTON_1/PA15 handler and the DWIN action button:
 * one press either starts, stops or acknowledges, decided by controller
 * state -- NOT by which surface the press came from. See the PA15 comment
 * block in App_Loop for the per-state rationale. */
static void app_action_button(ChargeCtrlState_t state, uint32_t now)
{
    switch (state) {
        case CHARGE_CTRL_STATE_IDLE:
        case CHARGE_CTRL_STATE_READY:
            LOG("App: action button (IDLE/READY) -> start charge cycle\r\n");
            ChargeController_Start(CHARGE_CTRL_OWNER_DWIN, false, now);
            break;
        case CHARGE_CTRL_STATE_RUNNING:
        case CHARGE_CTRL_STATE_FAULT:
            LOG("App: action button (RUNNING/FAULT) -> stop charge cycle\r\n");
            ChargeController_Stop(now);
            break;
        case CHARGE_CTRL_STATE_STOPPING:
        default:
            /* Transient -- ignore, let it settle to IDLE first. */
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
    LOG("App_Init: Driver selected: id=%u\r\n", (unsigned)CHG_LIB_GetActiveDriverId());

    /* Initialize charge controller */
    ChargeController_Init();
    LOG("App_Init: Charge controller initialized.\r\n");

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
     * PA15): one press-release cycle either starts or stops the charge
     * cycle, decided by the controller's own state at the moment of the
     * debounced rising edge, not by which physical button was pressed.
     * Confirmed with user 2026-08-29: hardware only needs 1 button for
     * this (BUTTON_2/PD2 dropped from this flow, BSP_BTN_STOP left intact
     * in bsp_gpio.c/.h in case it's repurposed later).
     *   IDLE / READY -> Start.
     *   RUNNING      -> Stop.
     *   FAULT        -> Stop (ChargeController_Stop() already clears
     *                   fault back to IDLE when called from FAULT --
     *                   charge_controller.c -- so a press here reads as
     *                   "acknowledge and reset").
     *   STOPPING     -> ignored; transient state, no single safe action,
     *                   let it settle to IDLE first. */
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
                    ChargeCtrlView_t btn_view;
                    ChargeController_GetView(&btn_view);
                    app_action_button(btn_view.state, now);
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

        /* LED_FAULT: co loi hoac mat ket noi module */
        if (sum.any_critical || sum.modules_fault > 0 || sum.modules_online == 0) {
            led_fault_on();
        } else {
            led_fault_off();
        }
    }

    /* (4) DWIN HMI refresh -- one field group per 50ms tick (see
     * DWIN_UpdateData scatter). RX is drained above, near the top of the
     * loop. */
    if ((now - last_dwin_tick) >= 50) {
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
        if (!dwin_boot_sent && now > 3000U) {
            char fw_str[16];
            (void)snprintf(fw_str, sizeof(fw_str), "FW V%u.%u.%u",
                           (unsigned)FW_VERSION_MAJOR, (unsigned)FW_VERSION_MINOR,
                           (unsigned)FW_VERSION_PATCH);
            DWIN_SendSettingStrings(ChargeCycleConfig_GetHwRev(), fw_str,
                                    ChargeCycleConfig_GetDeviceId());
            DWIN_SetPage(DWIN_PAGE_DASH);
            dwin_boot_sent = true;
            /* Fires exactly once -- safe outside the 50ms LOG-blocking budget. */
            LOG("DWIN: HMI init sent (HW/FW/ID strings + dashboard page).\r\n");
        }

        DWIN_SystemData_t dd;
        memset(&dd, 0, sizeof(dd));

        float amps = cc_view.applied_current_per_module_a *
                     (float)cc_view.actual_module_count;
        dd.dc_voltage_x10 = (uint16_t)(cc_view.applied_voltage_v * 10.0f);
        dd.dc_current_x10 = (uint16_t)(amps * 10.0f);
        dd.dc_power_w     = (uint16_t)(cc_view.applied_voltage_v * amps);

        if (bms.online) {
            dd.soc_pct            = bms.soc;
            dd.bat_pack_volt_x10  = (uint16_t)(bms.batt_voltage * 10.0f);
            dd.bat_cell_volt_x100 = (uint16_t)(bms.max_cell_volt / 10U); /* mV -> 0.01V */
            dd.temp_battery_c     = (int16_t)bms.max_cell_temp;
        }
        /* TODO(Phase 2): session charged-Ah has no accumulator yet -- send 0
         * rather than a misleading proxy. Goes with the alarm/event-log work. */
        dd.charged_ah_x10 = 0U;

        /* AC phase voltages: per-module view (the summary carries none). */
        CHG_LIB_ModuleView_t mv;
        if (CHG_LIB_GetModuleView(0, &mv)) {
            dd.ac_l1_v = (uint16_t)mv.ac_phase_a_voltage;
            dd.ac_l2_v = (uint16_t)mv.ac_phase_b_voltage;
            dd.ac_l3_v = (uint16_t)mv.ac_phase_c_voltage;
        }

        /* NTC channels: ch0 = charger, ch1..3 = jack/connector (max, with the
         * same "-50C means disconnected" fallback used for jack derating). */
        dd.temp_charge_c = (int16_t)BSP_ADC_GetTempC(0);
        {
            float jack_c = -273.15f;
            for (uint8_t i = 1; i < 4; i++) {
                float t = BSP_ADC_GetTempC(i);
                if (isfinite(t) && t > jack_c) {
                    jack_c = t;
                }
            }
            dd.temp_jack_c = (int16_t)((jack_c < -50.0f) ? 0.0f : jack_c);
        }

        dd.status_icon = dwin_status_from_state(&cc_view, &sum);
        dd.btn_mode    = dwin_btn_mode_from_status(dd.status_icon);
        dd.uptime_s    = now / 1000U;

        DWIN_UpdateData(&dd);
    }

    /* (5) Refresh IWDG — main loop only, never in ISR (~1s timeout) */
    MX_IWDG_Refresh();
}

/* The DGUS action button is a single fixed-value Return-Key-Code control, so
 * `keyval` only tells us the button was pressed -- app_action_button() then
 * decides start/stop/reset from ChargeController state, exactly like the
 * physical PA15 button. */
void DWIN_OnActionButton(uint16_t keyval)
{
    ChargeCtrlView_t v;
    ChargeController_GetView(&v);
    LOG("DWIN: action button keyval=%u (state=%d)\r\n",
        (unsigned)keyval, (int)v.state);
    app_action_button(v.state, BSP_GetTick());
}
