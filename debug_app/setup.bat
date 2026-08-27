@echo off
chcp 65001 >nul 2>&1
title Build Charger Debug App

echo ========================================
echo   Charger Debug App - Build Script
echo ========================================
echo.

REM Get script directory
set SCRIPT_DIR=%~dp0
cd /d "%SCRIPT_DIR%"

REM Resolve Python interpreter
set "PYTHON_EXE="
if exist "C:\Users\bongd\AppData\Local\Programs\Python\Python313\python.exe" (
    set "PYTHON_EXE=C:\Users\bongd\AppData\Local\Programs\Python\Python313\python.exe"
) else (
    py -3 --version >nul 2>&1
    if %errorlevel% equ 0 (
        set "PYTHON_EXE=py -3"
    ) else (
        python --version >nul 2>&1
        if %errorlevel% equ 0 (
            set "PYTHON_EXE=python"
        )
    )
)

if not defined PYTHON_EXE (
    echo ERROR: Python 3 not found.
    echo Checked: Python313, py -3, python
    pause
    exit /b 1
)

REM Step 1: Install dependencies
echo [1/3] Installing dependencies...
%PYTHON_EXE% -m pip install -q -r requirements.txt
if %errorlevel% neq 0 (
    echo ERROR: Failed to install dependencies
    pause
    exit /b 1
)
echo      OK
echo.

REM Step 2: Clean previous build
echo [2/3] Cleaning previous build...
if exist build rmdir /s /q build
if exist dist rmdir /s /q dist
echo      OK
echo.

REM Step 3: Build executable
echo [3/3] Building executable with PyInstaller...
echo      This may take 1-2 minutes...
%PYTHON_EXE% -m PyInstaller --noconfirm --onefile --windowed --name "ChargerDebugApp" main.py
if %errorlevel% neq 0 (
    echo ERROR: Build failed
    pause
    exit /b 1
)
echo.

echo ========================================
echo   Build complete!
echo ========================================
echo.
echo Executable: dist\ChargerDebugApp.exe
echo.
echo Press any key to launch the app...
pause >nul

start "" "dist\ChargerDebugApp.exe"
