@echo off
cd /d "%~dp0.."
echo ==================================================
echo   CHARGER LOCAL CI PIPELINE (WINDOWS)
echo ==================================================

echo [1/9] Running Static Analysis (check_ioc.py)...
set "PYTHON_BIN=python"
if exist "C:\cygwin64\bin\python3.9.exe" (
    set "PYTHON_BIN=C:\cygwin64\bin\python3.9.exe"
)
"%PYTHON_BIN%" check_ioc.py
if %errorlevel% neq 0 (
    echo [FAIL] Static Analysis Failed!
    exit /b %errorlevel%
)

echo [2/9] Running Native Unit Tests (test_logic.c)...
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

echo [3/9] Running Config Storage Migration Test: v5 to v6...
gcc -Wall -DCHARGE_CYCLE_STORAGE_HOST_TEST -I"test/mock_hal" -I"Modules/chg_lib" -I"BSP" -I"Utils/Log" -I"App/Charge" ^
    test/host_charge_sim/test_config_storage.c ^
    App/Charge/charge_cycle_storage.c ^
    App/Charge/charge_cycle_config.c ^
    Modules/chg_lib/chg_lib_core.c ^
    -o test_config_storage.exe
if %errorlevel% neq 0 (
    echo [FAIL] Compilation of test_config_storage.c Failed!
    exit /b %errorlevel%
)
.\test_config_storage.exe
if %errorlevel% neq 0 (
    echo [FAIL] Config Storage Migration Test Failed!
    exit /b %errorlevel%
)

echo [4/9] Running Charger E2E Simulation: BMS and module CAN to MCU app view...
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

echo [5/9] Running PC Protocol E2E Simulation: byte stream to PC protocol parser...
gcc -Wall -I"test/mock_hal" -I"test/host_charge_sim" -I"Modules/chg_lib" -I"Modules/bms" -I"BSP" -I"Utils/Log" -I"App/Charge" -I"App/Alarm" -I"App/Protocol" -I"App/System" ^
    test/host_protocol_sim/test_pc_protocol_e2e.c ^
    test/host_charge_sim/sim_can_modules.c ^
    test/host_charge_sim/sim_bms.c ^
    test/mock_hal/mock_stubs.c ^
    App/Protocol/pc_protocol.c ^
    App/Protocol/pc_debug_protocol.c ^
    App/System/app_rtc_sync.c ^
    App/Alarm/alarm.c ^
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

echo [6/9] Running DWIN Protocol E2E Simulation: byte stream to parser...
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

echo [7/9] Running Alarm Subsystem E2E Simulation: BMS and module CAN to controller...
gcc -Wall -I"test/mock_hal" -I"test/host_charge_sim" -I"Modules/chg_lib" -I"Modules/bms" -I"BSP" -I"Utils/Log" -I"App/Charge" -I"App/Alarm" ^
    test/host_alarm_sim/test_alarm_e2e.c ^
    test/host_charge_sim/sim_can_modules.c ^
    test/host_charge_sim/sim_bms.c ^
    test/mock_hal/mock_stubs.c ^
    App/Alarm/alarm.c ^
    App/Alarm/dwin_alarm_text.c ^
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
    -lm -o test_alarm_e2e.exe
if %errorlevel% neq 0 (
    echo [FAIL] Compilation of test_alarm_e2e.c Failed!
    exit /b %errorlevel%
)
.\test_alarm_e2e.exe
if %errorlevel% neq 0 (
    echo [FAIL] Alarm Subsystem E2E Simulation Failed!
    exit /b %errorlevel%
)

echo [8/9] Building Firmware (Release preset)...
call build.bat
if %errorlevel% neq 0 (
    echo [FAIL] Firmware Build Failed!
    exit /b %errorlevel%
)

echo [9/9] Firmware built successfully.
echo Note: Hardware-In-The-Loop tests (integration_sync_test.py) must be run manually when hardware is connected.
echo ==================================================
echo   LOCAL CI PASSED
echo ==================================================
pause
