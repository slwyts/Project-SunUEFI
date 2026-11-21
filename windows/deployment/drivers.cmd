@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "packages=%~dp0Drivers"
if /i "%~1"=="offline" goto offline
chcp 65001 >nul
echo 正在将随介质提供的驱动加入 Windows PE。
call :stage >"%~dp0drivers.log" 2>&1
set "stage_error=%errorlevel%"
type "%~dp0drivers.log"
if not "%stage_error%"=="0" exit /b %stage_error%
if /i "%~1"=="setup" "%SystemDrive%\setup.exe" /InstallDrivers "%packages%"
exit /b %errorlevel%

:stage
set "failed="
for /r "%packages%" %%I in (*.inf) do (
    echo "%%~fI"
    pnputil /add-driver "%%~fI" /install
    call :status
)
if defined failed exit /b 1
exit /b 0

:status
set "result=%errorlevel%"
echo PnPUtil=%result%
rem A staged package may have no matching device (259) or need a reboot (3010).
if not "%result%"=="0" if not "%result%"=="259" if not "%result%"=="3010" set "failed=1"
exit /b 0

:offline
dism /English /Image:%~2\ /Add-Driver /Driver:"%packages%" /Recurse /ScratchDir:"%~3"
if "%errorlevel%"=="3010" exit /b 0
exit /b %errorlevel%
