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

/* ============== Private state ============== */

static uint32_t last_process_tick = 0;
static uint32_t last_led_tick     = 0;
static uint32_t last_dwin_tick    = 0;
static uint32_t last_main_log     = 0;
/* Button debounce */
static uint32_t btn_start_last    = 0;
static uint32_t btn_stop_last     = 0;
static uint8_t  btn_start_prev    = 0;
static uint8_t  btn_stop_prev     = 0;
static uint8_t  btn_start_db      = 0;
static uint8_t  btn_stop_db       = 0;

/* ============== LED control ============== */

static void led_run_on(void)   { BSP_LED_On(BSP_LED_RUN); }
static void led_run_off(void)  { BSP_LED_Off(BSP_LED_RUN); }
static void led_fault_on(void) { BSP_LED_On(BSP_LED_FAULT); }
static void led_fault_off(void){ BSP_LED_Off(BSP_LED_FAULT); }

/* ============== Button read ============== */

static uint8_t read_btn_start(void) { return BSP_BTN_IsPressed(BSP_BTN_START) ? 1 : 0; }
static uint8_t read_btn_stop(void)  { return BSP_BTN_IsPressed(BSP_BTN_STOP) ? 1 : 0; }

/* ============== DWIN fault code translation ============== */

/**
 * @brief Translate the internal fault_flags bitmask into DWIN_FaultCode_e
 * @note  BUGFIX: this used to be `dwin_data.fault_code = cc_view.fault_flags`
 *        directly -- but fault_flags (charge_controller.h) is a uint32_t
 *        bitmask that can OR multiple CHARGE_CTRL_FAULT_* bits together
 *        (e.g. BMS_OFFLINE=(1<<3)=8, EMERGENCY_STOP=(1<<11)=2048), while
 *        DWIN_FaultCode_e (dwin_protocol.h) is a small sequential code
 *        0-7 matching one icon slot on the touchscreen ("Tương ứng với Bit
 *        Variable Icon ID trên DWIN 0.ICO"). Sending the raw bitmask meant
 *        the operator's fault icon showed a meaningless/wrong value during
 *        an actual fault.
 *        This mapping is a best-effort UX priority order (most
 *        safety-critical first), not a safety interlock itself -- the
 *        relay/charge-control logic reads fault_flags directly and is
 *        unaffected by this table. Revisit the exact bit->code grouping
 *        with whoever owns the DWIN icon set (ui/DWIN_SET/) if it doesn't
 *        match the intended on-screen meaning. */
static uint16_t dwin_fault_code_from_flags(uint32_t flags)
{
    if (flags == 0U) {
        return FAULT_NONE;
    }
    if (flags & CHARGE_CTRL_FAULT_EMERGENCY_STOP) {
        return FAULT_HARDWARE;
    }
    if (flags & CHARGE_CTRL_FAULT_BMS_OFFLINE) {
        return FAULT_BMS_OFFLINE;
    }
    if (flags & (CHARGE_CTRL_FAULT_BMS_ALARM | CHARGE_CTRL_FAULT_PROTECT_JACK_V)) {
        return FAULT_OVER_VOLT;
    }
    if (flags & CHARGE_CTRL_FAULT_PROTECT_JACK_TEMP) {
        return FAULT_OVER_TEMP;
    }
    if (flags & (CHARGE_CTRL_FAULT_NO_MODULE | CHARGE_CTRL_FAULT_MODULE_COUNT_MISMATCH |
                 CHARGE_CTRL_FAULT_NO_DRIVER)) {
        return FAULT_CHARGER_OFFLINE;
    }
    /* Catch-all for anything without a dedicated DWIN code yet
     * (INVALID_CONFIG, BMS_STALE, reserved bits, future fault types):
     * still surface *something* is wrong rather than silently showing
     * FAULT_NONE. */
    return FAULT_HARDWARE;
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

    /* RS485 (USART3 + DE pin, che do nhan) */
    BSP_RS485_Init();

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
         * và ghi GPIO — không tự quyết định điều kiện an toàn ở đây. */
        {
            ChargeCtrlView_t relay_view;
            ChargeController_GetView(&relay_view);
            if (relay_view.relay_should_close) {
                HAL_GPIO_WritePin(GPIOB, MCU_PB14_RELAY_1_Pin|MCU_PB15_RELAY_2_Pin, GPIO_PIN_SET);
            } else {
                HAL_GPIO_WritePin(GPIOB, MCU_PB14_RELAY_1_Pin|MCU_PB15_RELAY_2_Pin, GPIO_PIN_RESET);
            }
        }
    }

    PC_Protocol_ProcessTx();
    
    /* DWIN HMI: Read RX and Parse */
    uint8_t rs485_buf[64];
    uint16_t rs485_len = BSP_RS485_Read(rs485_buf, sizeof(rs485_buf));
    if (rs485_len > 0) {
        DWIN_ParseRX(rs485_buf, rs485_len);
    }

    /* (2) Button handling with debounce */
    {
        uint8_t start_raw = read_btn_start();
        uint8_t stop_raw  = read_btn_stop();

        if (start_raw != btn_start_db) {
            btn_start_db = start_raw;
            btn_start_last = now;
        }
        if ((now - btn_start_last) > APP_BTN_DEBOUNCE_MS) {
            if (btn_start_db != btn_start_prev) {
                btn_start_prev = btn_start_db;
                if (btn_start_prev) {
                    LOG("App_Loop: START button pressed -> starting charge cycle\r\n");
                    ChargeController_Start(CHARGE_CTRL_OWNER_DWIN, false, now);
                }
            }
        }

        if (stop_raw != btn_stop_db) {
            btn_stop_db = stop_raw;
            btn_stop_last = now;
        }
        if ((now - btn_stop_last) > APP_BTN_DEBOUNCE_MS) {
            if (btn_stop_db != btn_stop_prev) {
                btn_stop_prev = btn_stop_db;
                if (btn_stop_prev) {
                    LOG("App_Loop: STOP button pressed -> stopping charge cycle\r\n");
                    ChargeController_Stop(now);
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

    /* (4) DWIN Update */
    if ((now - last_dwin_tick) >= 50) { // Call every 50ms (so all 12 frames take 600ms)
        last_dwin_tick = now;
        DWIN_SystemData_t dwin_data = {0};
        
        ChargeCtrlView_t cc_view;
        ChargeController_GetView(&cc_view);
        
        dwin_data.sys_status = cc_view.state;
        dwin_data.dc_volt_x10 = (uint16_t)(cc_view.applied_voltage_v * 10);
        dwin_data.dc_curr_x10 = (uint16_t)(cc_view.applied_current_per_module_a * cc_view.actual_module_count * 10);
        
        BMS_View_t bms;
        BMS_GetView(&bms);
        if (bms.online) {
            dwin_data.bat_soc = bms.soc;
            dwin_data.bat_pack_v_x10 = (uint16_t)(bms.batt_voltage * 10);
            dwin_data.bat_cell_v_x100 = (uint16_t)(bms.max_cell_volt / 10); // mV to x100
            dwin_data.temp_bat = bms.max_cell_temp;
        }
        
        dwin_data.temp_charger = (int16_t)BSP_ADC_GetTempC(0);
        dwin_data.fault_code = dwin_fault_code_from_flags(cc_view.fault_flags);
        
        DWIN_UpdateData(&dwin_data);
    }

    /* (5) Refresh IWDG — main loop only, never in ISR (~1s timeout) */
    MX_IWDG_Refresh();
}

void DWIN_OnCommandReceived(uint16_t command) {
    if (command == 1) {
        LOG("DWIN: Received START command\r\n");
        ChargeController_Start(CHARGE_CTRL_OWNER_DWIN, false, BSP_GetTick());
    } else if (command == 2) {
        LOG("DWIN: Received STOP command\r\n");
        ChargeController_Stop(BSP_GetTick());
    }
}
