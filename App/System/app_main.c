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
#include "bsp_rs485.h"
#include "bms_can.h"
#include "bms_core.h"
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

/* ============== Private state ============== */

static uint32_t last_process_tick = 0;
static uint32_t last_led_tick     = 0;
/* Button debounce */
static uint32_t btn_start_last    = 0;
static uint32_t btn_stop_last     = 0;
static uint8_t  btn_start_prev    = 0;
static uint8_t  btn_stop_prev     = 0;

/* ============== LED control ============== */

static void led_run_on(void)   { BSP_LED_On(BSP_LED_RUN); }
static void led_run_off(void)  { BSP_LED_Off(BSP_LED_RUN); }
static void led_fault_on(void) { BSP_LED_On(BSP_LED_FAULT); }
static void led_fault_off(void){ BSP_LED_Off(BSP_LED_FAULT); }

/* ============== Button read ============== */

static uint8_t read_btn_start(void) { return BSP_BTN_IsPressed(BSP_BTN_START) ? 1 : 0; }
static uint8_t read_btn_stop(void)  { return BSP_BTN_IsPressed(BSP_BTN_STOP) ? 1 : 0; }

/* ============== Init ============== */

void App_Init(void)
{
    LOG_Banner();
    LOG("App_Init: Khoi dong he thong...\r\n");

    /* Enable Peripheral Power (RS485/CAN/HMI) - assert early */
    HAL_GPIO_WritePin(GPIOA, MCU_PA4_POWER_EN_Pin, GPIO_PIN_SET);

    led_run_off();
    led_fault_off();

    /* RS485 (USART3 + DE pin, che do nhan) */
    BSP_RS485_Init();

    /* Start CAN1 + CAN2 (filter + interrupt) */
    LOG("App_Init: Khoi dong CAN bus...\r\n");
    if (!BSP_CAN_Start()) {
        LOG("App_Init: LOI - Khong the khoi dong CAN bus!\r\n");
        led_fault_on();
    } else {
        LOG("App_Init: CAN bus khoi dong thanh cong.\r\n");
    }

    /* Initialize BMS driver (CAN2, 250Kbps) */
    BMS_Init();
    LOG("App_Init: BMS driver khoi dong xong.\r\n");

    /* Register charger drivers (CAN1, 125Kbps) - PC app se chon driver */
    LOG("App_Init: Khoi tao charger core\r\n");
    CHG_LIB_RegisterDriver(CHG_LIB_DRV_MAXWELL, CHG_LIB_MaxwellDriverOps());
    CHG_LIB_RegisterDriver(CHG_LIB_DRV_LIANMING, CHG_LIB_LianmingDriverOps());
    CHG_LIB_RegisterDriver(CHG_LIB_DRV_TONHE, CHG_LIB_TonheDriverOps());
    CHG_LIB_Init();
    CHG_LIB_CanBackend_Init();

    /* Initialize debug protocol */
    DebugProtocol_Init();

    ChargeCycleConfig_Init();

    /* Load config from flash */
    ChargeCycleStorage_Init();

    /* Initialize charge controller */
    ChargeController_Init();
    LOG("App_Init: Charge controller initialized.\r\n");

    LOG("App_Init: Hoan tat khoi tao.\r\n");
}

/* ============== Main Loop ============== */

void App_Loop(void)
{
    uint32_t now = BSP_GetTick();

    /* Drain queued USB CDC TX */
    PC_Protocol_ProcessTx();

    /* Watchdog bus-off cho CAN1/CAN2 */
    BSP_CAN_Process();

    /* (1) Control loop 20ms: charger FSM, BMS, charge controller */
    if ((now - last_process_tick) >= APP_PROCESS_INTERVAL_MS) {
        last_process_tick = now;
        CHG_LIB_Process(now);
        BMS_Process(now);
        ChargeController_Process(now);
    }

    PC_Protocol_ProcessTx();

    /* (2) Button handling with debounce */
    {
        uint8_t start_raw = read_btn_start();
        uint8_t stop_raw  = read_btn_stop();

        if (start_raw && !btn_start_prev && (now - btn_start_last) > APP_BTN_DEBOUNCE_MS) {
            btn_start_last = now;
            LOG("App_Loop: Nhan nut START -> Khoi dong chu trinh sac\r\n");
            ChargeController_Start(CHARGE_CTRL_OWNER_DWIN, false);
        }
        btn_start_prev = start_raw;

        if (stop_raw && !btn_stop_prev && (now - btn_stop_last) > APP_BTN_DEBOUNCE_MS) {
            btn_stop_last = now;
            LOG("App_Loop: Nhan nut STOP -> Dung chu trinh sac\r\n");
            ChargeController_Stop();
        }
        btn_stop_prev = stop_raw;
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

    /* (4) Refresh IWDG — main loop only, never in ISR (~1s timeout) */
    MX_IWDG_Refresh();
}

CHG_LIB_DriverId_t App_GetCurrentDriver(void) { return CHG_LIB_GetActiveDriverId(); }
