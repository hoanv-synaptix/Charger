@echo off
echo ===================================
echo   Building STM32G0 Charger Project
echo ===================================

echo [1/2] Configuring CMake...
cmake --preset Release
if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake configuration failed!
    exit /b %ERRORLEVEL%
)

echo.
echo [2/2] Compiling...
cmake --build build/Release -j 8
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Build failed!
    exit /b %ERRORLEVEL%
)

echo.
echo [SUCCESS] Build completed successfully!
