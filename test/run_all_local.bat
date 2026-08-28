@echo off
cd /d "%~dp0.."
echo ==================================================
echo   CHARGER LOCAL CI PIPELINE (WINDOWS)
echo ==================================================

echo [1/7] Running Static Analysis (check_ioc.py)...
python check_ioc.py
if %errorlevel% neq 0 (
    echo [FAIL] Static Analysis Failed!
    exit /b %errorlevel%
)

echo [2/7] Running Native Unit Tests (test_logic.c)...
gcc -I"Modules/bms" -I"Modules/chg_lib" -I"test/mock_hal" test/test_logic.c Modules/bms/bms_protocol.c Modules/chg_lib/chg_lib_fsm.c -o test_logic.exe
if %errorlevel% neq 0 (
    echo [FAIL] Compilation of test_logic.c Failed!
    exit /b %errorlevel%
)
.\test_logic.exe
if %errorlevel% neq 0 (
    echo [FAIL] Unit Tests Failed!
    exit /b %errorlevel%
)

echo [3/7] Running Charger E2E Simulation (BMS + module CAN -> MCU -> app view)...
gcc -Wall -I"test/mock_hal" -I"test/host_charge_sim" -I"Modules/chg_lib" -I"Modules/bms" -I"BSP" -I"Utils/Log" -I"App/Charge" -I"App/Protocol" ^
    test/host_charge_sim/test_charge_e2e.c ^
    test/host_charge_sim/sim_can_modules.c ^
    test/host_charge_sim/sim_bms.c ^
    test/mock_hal/mock_stubs.c ^
    App/Charge/charge_controller.c ^
    App/Charge/charge_cycle_config.c ^
    Modules/chg_lib/chg_lib_core.c ^
    Modules/chg_lib/chg_lib_can_backend.c ^
    Modules/chg_lib/chg_lib_maxwell.c ^
    Modules/chg_lib/chg_lib_lianming.c ^
    Modules/chg_lib/chg_lib_tonhe.c ^
    Modules/chg_lib/chg_lib_fsm.c ^
    Modules/bms/bms_core.c ^
    Modules/bms/bms_can.c ^
    Modules/bms/bms_protocol.c ^
    -lm -o test_charge_e2e.exe
if %errorlevel% neq 0 (
    echo [FAIL] Compilation of test_charge_e2e.c Failed!
    exit /b %errorlevel%
)
.\test_charge_e2e.exe
if %errorlevel% neq 0 (
    echo [FAIL] Charger E2E Simulation Failed!
    exit /b %errorlevel%
)

echo [4/7] Running PC Protocol E2E Simulation (byte stream -> pc_protocol.c/pc_debug_protocol.c)...
gcc -Wall -I"test/mock_hal" -I"test/host_charge_sim" -I"Modules/chg_lib" -I"Modules/bms" -I"BSP" -I"Utils/Log" -I"App/Charge" -I"App/Protocol" ^
    test/host_protocol_sim/test_pc_protocol_e2e.c ^
    test/host_charge_sim/sim_can_modules.c ^
    test/host_charge_sim/sim_bms.c ^
    test/mock_hal/mock_stubs.c ^
    App/Protocol/pc_protocol.c ^
    App/Protocol/pc_debug_protocol.c ^
    App/Charge/charge_controller.c ^
    App/Charge/charge_cycle_config.c ^
    Modules/chg_lib/chg_lib_core.c ^
    Modules/chg_lib/chg_lib_can_backend.c ^
    Modules/chg_lib/chg_lib_maxwell.c ^
    Modules/chg_lib/chg_lib_lianming.c ^
    Modules/chg_lib/chg_lib_tonhe.c ^
    Modules/chg_lib/chg_lib_fsm.c ^
    Modules/bms/bms_core.c ^
    Modules/bms/bms_can.c ^
    Modules/bms/bms_protocol.c ^
    -lm -o test_pc_protocol_e2e.exe
if %errorlevel% neq 0 (
    echo [FAIL] Compilation of test_pc_protocol_e2e.c Failed!
    exit /b %errorlevel%
)
.\test_pc_protocol_e2e.exe
if %errorlevel% neq 0 (
    echo [FAIL] PC Protocol E2E Simulation Failed!
    exit /b %errorlevel%
)

echo [5/7] Running DWIN Protocol E2E Simulation (byte stream -> dwin_protocol.c)...
gcc -Wall -I"Modules/hmi" ^
    test/host_protocol_sim/test_dwin_protocol_e2e.c ^
    Modules/hmi/dwin_protocol.c ^
    -o test_dwin_protocol_e2e.exe
if %errorlevel% neq 0 (
    echo [FAIL] Compilation of test_dwin_protocol_e2e.c Failed!
    exit /b %errorlevel%
)
.\test_dwin_protocol_e2e.exe
if %errorlevel% neq 0 (
    echo [FAIL] DWIN Protocol E2E Simulation Failed!
    exit /b %errorlevel%
)

echo [6/7] Building Firmware (Release preset)...
call build.bat
if %errorlevel% neq 0 (
    echo [FAIL] Firmware Build Failed!
    exit /b %errorlevel%
)

echo [7/7] Firmware built successfully.
echo Note: Hardware-In-The-Loop tests (integration_sync_test.py) must be run manually when hardware is connected.
echo ==================================================
echo   LOCAL CI PASSED
echo ==================================================
pause
