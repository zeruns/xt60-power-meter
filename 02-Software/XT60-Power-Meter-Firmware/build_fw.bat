@echo off
rem ============================================================
rem XT60 Power Meter firmware - Windows one-click build script
rem
rem Usage: build_fw.bat [nopause]
rem   - if env IDF_PATH is already set (ESP-IDF CMD/PowerShell opened),
rem     it is used as-is (IDF_PATH / IDF_TOOLS_PATH untouched);
rem   - otherwise fall back to the local default install path below.
rem     Change the hardcoded path when moving to another machine.
rem
rem Result visibility (v0.6.9+):
rem   - a success/failure summary is always printed at the end;
rem   - the script then PAUSES so a double-clicked window stays open
rem     and you can read the log. Press any key to close it.
rem   - automation can skip the pause in any of these ways:
rem       build_fw.bat nopause        (argument, any position)
rem       set XT60_NO_PAUSE=1         (environment variable)
rem       CI=1                        (standard CI variable)
rem   - the real exit code is always preserved for callers.
rem
rem NOTE: keep this file ASCII-only. cmd.exe reads .bat in the OEM
rem codepage (e.g. GBK); UTF-8 multibyte sequences can swallow the
rem line ending and corrupt parsing. Chinese docs live in README.md
rem / CONTRIBUTING.md instead.
rem ============================================================

set RC=0

if defined IDF_PATH goto :have_idf
rem ---- IDF_PATH not set: fall back to the local install path ----
set IDF_PATH=D:\Espressif\v6.1\esp-idf
set IDF_TOOLS_PATH=C:\Espressif
set PATH=D:\Espressif\tools\idf-git\2.44.0\cmd;%PATH%

:have_idf
call "%IDF_PATH%\export.bat"
if errorlevel 1 (
    set RC=1
    echo.
    echo [FAIL] ESP-IDF environment setup failed.
    echo     IDF_PATH=%IDF_PATH%
    echo     Install ESP-IDF v6.1 or fix the path at the top of this script.
    goto :finish
)

idf.py build
set RC=%errorlevel%

echo.
if "%RC%"=="0" (
    echo ============================================================
    echo [OK] Build finished successfully.
    echo     App image : build\XT60-Power-Meter-Firmware.bin
    echo     Flash with: flash_fw.bat COMx   ^(check Device Manager^)
    echo ============================================================
) else (
    echo ============================================================
    echo [FAIL] Build failed with exit code %RC%.
    echo     Read the FIRST error line above ^(not the last ninja line^),
    echo     fix it, then run this script again.
    echo ============================================================
)

:finish
rem ---- pause so the result stays readable; skip in automation ----
echo(%* | find /i "nopause" >nul && exit /b %RC%
if defined CI exit /b %RC%
if defined XT60_NO_PAUSE exit /b %RC%
echo.
pause
exit /b %RC%
