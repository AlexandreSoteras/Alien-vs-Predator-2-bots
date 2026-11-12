@echo off
rem Removes the avp2bots bots and gives the server back its normal Master Server Patch code.
powershell -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0avp2bots-setup.ps1" uninstall %*
echo.
pause
