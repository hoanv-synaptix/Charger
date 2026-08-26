@echo off
cd /d "%~dp0.."
echo ==================================================
echo   CHARGER LOCAL CI PIPELINE (WINDOWS)
echo ==================================================

echo [1/4] Running Static Analysis (check_ioc.py)...
python check_ioc.py
if %errorlevel% neq 0 (
    echo [FAIL] Static Analysis Failed!
    exit /b %errorlevel%
)

echo [2/4] Running Native Unit Tests (test_logic.c)...
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

echo [3/4] Building Firmware (Release preset)...
call build.bat
if %errorlevel% neq 0 (
    echo [FAIL] Firmware Build Failed!
    exit /b %errorlevel%
)

echo [4/4] Firmware built successfully.
echo Note: Hardware-In-The-Loop tests (integration_sync_test.py) must be run manually when hardware is connected.
echo ==================================================
echo   LOCAL CI PASSED
echo ==================================================
pause
