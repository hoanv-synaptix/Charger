@echo off
echo ===================================
echo   Flashing STM32G0 Charger Project
echo ===================================

set PROGRAMMER="C:\ST\STM32CubeCLT_1.16.0\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe"
set BOOTLOADER="build\Release\Boot\Charger_Bootloader.hex"
set FIRMWARE="build\Release\Charger.hex"

if not exist %FIRMWARE% (
    echo [ERROR] Firmware not found! Please run build.bat first.
    pause
    exit /b 1
)

if exist %BOOTLOADER% (
    echo [INFO] Flashing Bootloader + Application via ST-LINK [SWD]...
    %PROGRAMMER% -c port=SWD freq=500 ap=0 -d %BOOTLOADER% -v -d %FIRMWARE% -v -rst
) else (
    echo [INFO] Flashing Application via ST-LINK [SWD]...
    %PROGRAMMER% -c port=SWD freq=500 ap=0 -d %FIRMWARE% -v -rst
)

if %ERRORLEVEL% neq 0 (
    echo [ERROR] Flashing failed! Check your ST-LINK connection.
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo [SUCCESS] Flashing completed!
pause
