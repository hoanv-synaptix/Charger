@echo off
echo ===================================
echo   Flashing STM32G0 Charger Project
echo ===================================

set PROGRAMMER="C:\ST\STM32CubeCLT_1.16.0\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe"
set FIRMWARE="build\Debug\Charger.elf"

if not exist %FIRMWARE% (
    echo [ERROR] Firmware not found! Please run build.bat first.
    pause
    exit /b 1
)

echo [INFO] Flashing via ST-LINK (SWD)...
%PROGRAMMER% -c port=SWD -d %FIRMWARE% -v -rst

if %ERRORLEVEL% neq 0 (
    echo [ERROR] Flashing failed! Check your ST-LINK connection.
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo [SUCCESS] Flashing completed!
pause
