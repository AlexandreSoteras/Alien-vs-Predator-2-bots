@echo off
rem Installs the avp2bots bots into an Aliens vs. Predator 2 dedicated server (asks for the game folder if needed).
powershell -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0avp2bots-setup.ps1" install %*
echo.
pause
