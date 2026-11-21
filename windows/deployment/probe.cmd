@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul
echo 正在初始化设备。检查完成后，Windows PE 会自动重启。
if exist "%SystemDrive%\SunUEFI\drivers.cmd" call "%SystemDrive%\SunUEFI\drivers.cmd"
wpeinit
wpeutil UpdateBootInfo
set "result="
for /f "tokens=2,*" %%A in ('reg query HKLM\SYSTEM\CurrentControlSet\Control /v PEBootRamdiskSourceDrive 2^>nul') do if /i "%%A"=="REG_SZ" if exist "%%B\SunUEFI\WinPE\probe.tag" set "result=%%B\SunUEFI\WinPE"
if defined result goto collect
for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if exist "%%D:\SunUEFI\WinPE\probe.tag" set "result=%%D:\SunUEFI\WinPE"
if defined result goto collect
for /f "tokens=1" %%V in ('mountvol') do if not defined result (
    set "volume=%%V"
    if /i "!volume:~0,11!"=="\\?\Volume{" if exist "!volume!SunUEFI\WinPE\probe.tag" set "result=!volume!SunUEFI\WinPE"
)
if defined result goto collect
for %%D in (S T U V W Y Z) do if not exist %%D:\ if not defined result (
    mountvol %%D: /s >nul 2>&1
    if exist "%%D:\SunUEFI\WinPE\probe.tag" set "result=%%D:\SunUEFI\WinPE"
    if not defined result mountvol %%D: /d >nul 2>&1
)
:collect
if not defined result (
    echo Windows PE 未识别带标记的测试卷，无法保存检查结果。请检查存储控制器驱动。
    goto restart
)
echo 正在记录磁盘和设备信息。
set "stamp=WinPE started: %DATE% %TIME%"
>"%result%\boot.log" echo !stamp!
set "readback="
set /p "readback="<"%result%\boot.log"
if "!readback!"=="!stamp!" echo readback=ok>>"%result%\boot.log"
ver >>"%result%\boot.log"
reg query HKLM\SYSTEM\CurrentControlSet\Control /v PEFirmwareType >>"%result%\boot.log" 2>&1
if exist "%SystemDrive%\SunUEFI\drivers.log" copy /y "%SystemDrive%\SunUEFI\drivers.log" "%result%\" >nul
reg query "HKLM\HARDWARE\RESOURCEMAP\System Resources\Physical Memory" /v ".Translated" >"%result%\memory.txt" 2>&1
(echo list disk&echo list volume&echo exit) | diskpart >"%result%\storage.txt" 2>&1
pnputil /enum-devices /connected /deviceids >"%result%\devices.txt" 2>&1
if exist "%SYSTEMROOT%\inf\setupapi.dev.log" copy /y "%SYSTEMROOT%\inf\setupapi.dev.log" "%result%\" >nul
:restart
wpeutil reboot
