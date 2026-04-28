@echo off
:: RIME Standalone Installer
:: Right-click -> Run as Administrator
::
:: This wrapper bypasses PowerShell execution policy restrictions
:: so users don't need to change their global settings.

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo.
    echo ERROR: This installer must be run as Administrator.
    echo Right-click Install.bat and select "Run as administrator"
    echo.
    pause
    exit /b 1
)

echo.
echo Starting RIME Standalone Installer...
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1" %*

if %errorlevel% neq 0 (
    echo.
    echo Installation encountered an error.
    pause
)
