@echo off
setlocal

net session >nul 2>&1
if errorlevel 1 (
  echo Sleep on LAN uninstall requires Administrator privileges.
  echo Right-click this file and choose "Run as administrator".
  exit /b 1
)

set "APP_DIR=%ProgramFiles%\SleepOnLan"
set "SOL_EXE=%APP_DIR%\sol.exe"
set "FIREWALL_RULE_7=Sleep on LAN UDP 7"
set "FIREWALL_RULE_9=Sleep on LAN UDP 9"

if exist "%SOL_EXE%" (
  "%SOL_EXE%" stop
  "%SOL_EXE%" uninstall
) else (
  sc stop SleepOnLan >nul 2>&1
  sc delete SleepOnLan >nul 2>&1
)

netsh advfirewall firewall delete rule name="%FIREWALL_RULE_7%" protocol=UDP localport=7 >nul 2>&1
netsh advfirewall firewall delete rule name="%FIREWALL_RULE_9%" protocol=UDP localport=9 >nul 2>&1

echo Sleep on LAN service and firewall rules removed.
echo Configuration and logs were left in:
echo   %ProgramData%\SleepOnLan

endlocal
