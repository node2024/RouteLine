@echo off
setlocal
if not exist "%~dp0Install-Addon.ps1" (
    echo Extract the entire RouteLine Workshop ZIP before running this file.
    pause
    exit /b 1
)
echo Enter the Counter-Strike Global Offensive installation folder when prompted.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Addon.ps1"
set "result=%ERRORLEVEL%"
pause
exit /b %result%
