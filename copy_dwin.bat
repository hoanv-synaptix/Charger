@echo off
chcp 65001 >nul
setlocal

set "SOURCE=D:\Projects\Charger_CTRL\Charger\ui_darkmode\DWIN_SET"
set "DRIVE=E"
set "TARGET=%DRIVE%:\DWIN_SET"

echo ==========================================
echo   DWIN_SET AUTO COPY ^& EJECT TOOL
echo ==========================================
echo.
echo Nguon: %SOURCE%
echo Dich:  %TARGET%
echo.

if not exist "%DRIVE%:\" (
    echo [LOI] Khong tim thay o dia %DRIVE%: ! Vui long cam the nho vao.
    pause
    exit /b
)

echo [1/2] Dang xoa thu muc DWIN_SET cu trong the nho %DRIVE%: ...
if exist "%TARGET%" (
    rmdir /s /q "%TARGET%"
)

echo.
echo [2/2] Dang copy DWIN_SET moi vao the nho %DRIVE%: ...
xcopy /s /e /i /h /y "%SOURCE%" "%TARGET%\"

echo.
echo Copy thanh cong! Dang an toan ngat ket noi the nho %DRIVE%: ...

:: Phuong phap WMI
powershell -Command "Get-WmiObject -Class Win32_Volume -Filter 'DriveLetter=\"%DRIVE%:\"' | ForEach-Object { $_ .Dismount($false, $false) | Out-Null }"

echo.
echo HOAN THANH! 
echo Neu khong co thong bao loi mau do, ban da co the rut the nho ra ngay bay gio.
echo ==========================================
pause