@echo off
setlocal
if defined SUNUEFI_PYTHON goto custom_python
where py >nul 2>nul
if not errorlevel 1 goto use_py
where python >nul 2>nul
if not errorlevel 1 goto use_python
echo Python 3.10+ is required. Install Python and Android platform-tools, then try again. 1>&2
exit /b 2

:custom_python
"%SUNUEFI_PYTHON%" -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)"
if errorlevel 1 goto python_version_error
"%SUNUEFI_PYTHON%" "%~dp0installer_launcher.py" %*
goto finished

:use_py
py -3 -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)"
if errorlevel 1 goto python_version_error
py -3 "%~dp0installer_launcher.py" %*
goto finished

:use_python
python -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)"
if errorlevel 1 goto python_version_error
python "%~dp0installer_launcher.py" %*
goto finished

:python_version_error
echo Python 3.10+ is required. 1>&2
exit /b 2

:finished
set "installer_result=%errorlevel%"
if "%~1"=="" pause
exit /b %installer_result%
