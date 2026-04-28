@echo off
:: RIME Standalone Uninstaller
:: Right-click -> Run as Administrator

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo.
    echo ERROR: This uninstaller must be run as Administrator.
    echo Right-click Uninstall.bat and select "Run as administrator"
    echo.
    pause
    exit /b 1
)

echo.
echo Starting RIME Standalone Uninstaller...
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1" -Uninstall

pause
