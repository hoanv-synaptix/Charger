"""
Sprint 1.5 — IWDG + safe-state
"""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]
def read(p): return (ROOT / p).read_text(encoding="utf-8", errors="ignore")

def test_hal_iwdg_enabled():
    txt = read("Core/Inc/stm32g0xx_hal_conf.h")
    assert "#define HAL_IWDG_MODULE_ENABLED" in txt
    assert "/* #define HAL_IWDG_MODULE_ENABLED" not in txt

def test_ioc_iwdg():
    txt = read("Charger.ioc")
    assert "Mcu.IP16=IWDG" in txt or "IWDG" in txt
    assert "IWDG.Prescaler=IWDG_PRESCALER_32" in txt
    assert "IWDG.Reload=1000" in txt
    assert "IWDG.Window=4095" in txt

def test_iwdg_files_exist():
    assert (ROOT / "Core/Inc/iwdg.h").exists()
    assert (ROOT / "Core/Src/iwdg.c").exists()
    c = read("Core/Src/iwdg.c")
    assert "MX_IWDG_Init" in c
    assert "IWDG->PR" in c or "hiwdg" in c
    assert "FREEZE_IWDG" in c or "DBGMCU" in c
    h = read("Core/Inc/iwdg.h")
    assert "MX_IWDG_Init" in h

def test_main_calls_iwdg():
    main = read("Core/Src/main.c")
    assert "MX_IWDG_Init" in main
    # IWDG must start AFTER App_Init to avoid reset during boot (init takes >1s)
    assert main.find("MX_IWDG_Init") > main.find("App_Init")
    # Error_Handler should call Safety_Shutdown
    assert "Safety_Shutdown" in main
    eh_start = main.find("void Error_Handler")
    assert eh_start >= 0
    eh = main[eh_start:]
    assert eh.find("Safety_Shutdown") < eh.find("__disable_irq")

def test_hardfault_calls_safety():
    it = read("Core/Src/stm32g0xx_it.c")
    assert "Safety_Shutdown" in it
    hf = it[it.find("HardFault_Handler"):it.find("HardFault_Handler")+800]
    assert "Safety_Shutdown" in hf
    assert "__disable_irq" in hf

def test_app_loop_refresh():
    app = read("App/System/app_main.c")
    assert "MX_IWDG_Refresh" in app or "HAL_IWDG_Refresh" in app
    # Should be at end of App_Loop, not in ISR
    assert "IWDG" in app
    # Ensure not in ISR (no IWDG refresh in BSP_CAN callback)
    bsp = read("BSP/bsp_can.c")
    assert "IWDG" not in bsp

def test_failsafe_shutdown():
    assert (ROOT / "BSP/bsp_failsafe.h").exists()
    assert (ROOT / "BSP/bsp_failsafe.c").exists()
    c = read("BSP/bsp_failsafe.c")
    assert "Safety_Shutdown" in c
    assert "GPIO_PIN_14" in c and "GPIO_PIN_15" in c and "GPIO_PIN_8" in c
    assert "GPIO_PIN_4" in c  # POWER_EN
    assert "HAL_FDCAN_Stop" in c
    h = read("BSP/bsp_failsafe.h")
    assert "Safety_Shutdown" in h
    cmake = read("BSP/CMakeLists.txt")
    assert "bsp_failsafe.c" in cmake
    cubemx = read("cmake/stm32cubemx/CMakeLists.txt")
    assert "iwdg.c" in cubemx

def test_bsp_failsafe_disables_relays():
    c = read("BSP/bsp_failsafe.c")
    # Must turn off relays — either GPIO_PIN_RESET or direct BSRR << 16u
    assert ("GPIO_PIN_RESET" in c) or ("<< 16u" in c)
    # Safety shutdown owns the safe outputs. PC7 is now a power indicator and
    # must not be driven from this safety-shutdown path.
    assert "GPIO_PIN_7" not in c
