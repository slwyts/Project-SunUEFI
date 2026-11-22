@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul
echo 正在初始化设备。检查完成后，Windows PE 会自动重启。
if exist "%SystemDrive%\SunUEFI\drivers.cmd" call "%SystemDrive%\SunUEFI\drivers.cmd"
wpeinit
wpeutil UpdateBootInfo
set "result="
set "ramdiskSource="
for /f "tokens=2,*" %%A in ('reg query HKLM\SYSTEM\CurrentControlSet\Control /v PEBootRamdiskSourceDrive 2^>nul') do if /i "%%A"=="REG_SZ" set "ramdiskSource=%%B"
if defined ramdiskSource if exist "!ramdiskSource!\SunUEFI\WinPE\probe.tag" set "result=!ramdiskSource!\SunUEFI\WinPE"
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
call :storage
pnputil /enum-devices /connected /deviceids >"%result%\devices.txt" 2>&1
pnputil /enum-devices /connected /class SCSIAdapter /deviceids /services /relations /drivers >"%result%\storage-devices.txt" 2>&1
pnputil /enum-devices /connected /class DiskDrive /deviceids /services /relations >>"%result%\storage-devices.txt" 2>&1
if exist "%SYSTEMROOT%\inf\setupapi.dev.log" copy /y "%SYSTEMROOT%\inf\setupapi.dev.log" "%result%\" >nul
:restart
wpeutil reboot
exit /b

:storage
setlocal
set "volume=!result:\SunUEFI\WinPE=!"
if "!volume:~-1!"=="\" set "volume=!volume:~0,-1!"
set "letter="
set "temporary="
if "!volume:~1,1!"==":" set "letter=!volume:~0,2!"
rem DiskPart selects an unlettered volume by mount point, not its enumeration order.
if /i "!volume:~0,11!"=="\\?\Volume{" for %%D in (S T U V W Y Z D E F G H I J K L M N O P Q R) do (
    if not defined letter if not exist %%D:\ (
        mountvol %%D: !volume!\ >nul 2>&1
        if not errorlevel 1 (
            set "letter=%%D:"
            set "temporary=%%D:"
        )
    )
)
>"%result%\storage.txt" echo Result volume: !volume!
if defined ramdiskSource >>"%result%\storage.txt" echo WinPE RAM disk source: !ramdiskSource!
if defined letter (
    rem Selecting a basic volume also selects its disk, including its bus type and disk ID.
    (echo list disk&echo list volume&echo select volume %letter%&echo detail volume&echo detail disk&echo exit) | diskpart >>"%result%\storage.txt" 2>&1
) else (
    >>"%result%\storage.txt" echo Could not select the result volume in DiskPart.
    (echo list disk&echo list volume&echo exit) | diskpart >>"%result%\storage.txt" 2>&1
)
if defined temporary mountvol !temporary! /d >nul 2>&1
endlocal
exit /b
