@echo off
setlocal EnableExtensions
chcp 65001 >nul
set "drivers=%SystemDrive%\SunUEFI\drivers.cmd"
set "driver_error="
if exist "%drivers%" (
    call "%drivers%"
    if errorlevel 1 set "driver_error=1"
)
wpeinit
set "SUNUEFI_MARKER=@TARGET_MARKER@"
set "SUNUEFI_IMAGE=@IMAGE_NAME@"
set "SUNUEFI_INDEX=@IMAGE_INDEX@"
set "esp="
set "dest="
for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
    if exist "%%D:\SunUEFI\Deployment\%SUNUEFI_MARKER%.pending" set "esp=%%D:"
    if exist "%%D:\%SUNUEFI_MARKER%.target" set "dest=%%D:"
)
for %%D in (S T U V W Y Z) do if not defined esp if not exist %%D:\ (
    mountvol %%D: /s >nul 2>&1
    if exist "%%D:\SunUEFI\Deployment\%SUNUEFI_MARKER%.pending" set "esp=%%D:"
    if not defined esp mountvol %%D: /d >nul 2>&1
)
if not defined esp (
    echo 找不到本次安装的启动卷，未更改系统。
    goto stop
)
set "work=%esp%\SunUEFI\Deployment"
set "log=%work%\install.log"
if exist "%SystemDrive%\SunUEFI\drivers.log" copy /y "%SystemDrive%\SunUEFI\drivers.log" "%work%\" >nul
if defined driver_error (
    echo Windows PE 未能添加选定的驱动。请检查同目录的 drivers.log。>"%log%"
    goto stop
)
if not defined dest (
    echo Windows 未识别指定的系统卷。请检查存储控制器驱动。>"%log%"
    goto stop
)
if /i "%dest%"=="%esp%" goto stop
if not exist "%dest%\%SUNUEFI_IMAGE%" goto stop
move /y "%work%\%SUNUEFI_MARKER%.pending" "%work%\%SUNUEFI_MARKER%.running" >nul || goto stop
echo Windows setup %DATE% %TIME%: %dest% ^(system^) %esp% ^(ESP^)>"%log%"
set "scratch=%dest%\%SUNUEFI_MARKER%.scratch"
md "%scratch%" || goto failed
set "split="
if /i "%SUNUEFI_IMAGE:~-4%"==".swm" set "split=/SWMFile:%dest%\SunUEFI-install*.swm"
echo 正在安装 Windows。详细进度记录在 %log%。
dism /Apply-Image /ImageFile:"%dest%\%SUNUEFI_IMAGE%" %split% /Index:%SUNUEFI_INDEX% /ApplyDir:%dest%\ /ScratchDir:"%scratch%" /CheckIntegrity /Verify >>"%log%" 2>&1
if errorlevel 1 goto failed
if exist "%drivers%" (
    echo 正在将选定的驱动加入新系统。
    call "%drivers%" offline %dest% "%scratch%" >>"%log%" 2>&1
    if errorlevel 1 goto failed
)

rem Generate boot files on the new Windows volume. BCDBoot may also write
rem EFI\Boot; only Microsoft's own directory belongs on the shared ESP.
bcdboot "%dest%\Windows" /s %dest% /f UEFI /c >>"%log%" 2>&1
if errorlevel 1 goto failed
bcdedit /store "%dest%\EFI\Microsoft\Boot\BCD" /set {bootmgr} device partition=%esp% >>"%log%" 2>&1
if errorlevel 1 goto failed
xcopy "%dest%\EFI\Microsoft\*" "%esp%\EFI\Microsoft\" /e /h /r /k /y /i >>"%log%" 2>&1
if errorlevel 1 goto failed
bcdedit /store "%esp%\EFI\Microsoft\Boot\BCD" /enum {default} >>"%log%" 2>&1
if errorlevel 1 goto failed
move /y "%work%\%SUNUEFI_MARKER%.running" "%work%\%SUNUEFI_MARKER%.installed" >nul || goto failed
del /f /q "%dest%\%SUNUEFI_MARKER%.target" "%dest%\%SUNUEFI_IMAGE%"
if defined split del /f /q "%dest%\SunUEFI-install*.swm"
rd /s /q "%scratch%"
rd /s /q "%dest%\EFI"
del /f /q "%esp%\sources\boot.wim" "%esp%\boot\boot.sdi"
>>"%log%" echo boot_files_ready=1
echo Windows 已安装，正在重新启动。
wpeutil reboot
exit /b
:failed
echo Installation failed: %DATE% %TIME%.>>"%log%"
echo 安装未完成。请查看 %log%，修正问题后重新部署介质。
:stop
wpeutil reboot
