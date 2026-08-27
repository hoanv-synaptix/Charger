@echo off
chcp 65001 >nul 2>&1
title Charger Debug App

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

REM Check dependencies
%PYTHON_EXE% -c "import serial" >nul 2>&1
if %errorlevel% neq 0 (
    echo Installing dependencies...
    %PYTHON_EXE% -m pip install -q -r requirements.txt
    if %errorlevel% neq 0 (
        echo ERROR: Failed to install dependencies
        pause
        exit /b 1
    )
)

REM Run
echo Starting Charger Debug App...
%PYTHON_EXE% main.py
if %errorlevel% neq 0 (
    echo.
    echo App exited with error code %errorlevel%
    pause
)
