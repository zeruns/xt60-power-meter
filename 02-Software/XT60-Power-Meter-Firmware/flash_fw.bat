@echo off
rem ============================================================
rem XT60 Power Meter firmware - Windows one-click flash script
rem
rem Usage: flash_fw.bat [PORT] [nopause]
rem   - PORT comes from the 1st argument (%%1), default COM3
rem     e.g. flash_fw.bat COM5
rem   - flash baud rate can be set via env ESPBAUD; if unset, esptool
rem     uses its own default. Project convention is 921600: set
rem     ESPBAUD=921600 before running this script.
rem   - if env IDF_PATH is already set (ESP-IDF CMD/PowerShell opened),
rem     it is used as-is; otherwise fall back to the local default path.
rem
rem Result visibility (v0.6.9+):
rem   - a success/failure summary is always printed at the end;
rem   - the script then PAUSES so a double-clicked window stays open
rem     and you can read the log. Press any key to close it.
rem   - automation can skip the pause in any of these ways:
rem       flash_fw.bat COM3 nopause   (argument, any position)
rem       set XT60_NO_PAUSE=1         (environment variable)
rem       CI=1                        (standard CI variable)
rem   - the real exit code is always preserved for callers.
rem
rem NOTE: keep this file ASCII-only. cmd.exe reads .bat in the OEM
rem codepage (e.g. GBK); UTF-8 multibyte sequences can swallow the
rem line ending and corrupt parsing. Chinese docs live in README.md
rem / CONTRIBUTING.md instead.
rem
rem Flashing writes 5 images (bootloader / partition table / otadata /
rem app / storage). The storage image holds the SPIFFS CJK fonts and is
rem NOT touched by OTA, so re-flash fully after regenerating fonts.
rem See docs/build_and_flash.md for offsets and the ESPConnect guide.
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

rem ---- PORT: first argument, default COM3 ----
set PORT=%1
if "%PORT%"=="" set PORT=COM6
rem allow "flash_fw.bat nopause COM5" style: skip the nopause token
if /i "%PORT%"=="nopause" set PORT=%2
if "%PORT%"=="" set PORT=COM6

idf.py -p %PORT% flash
set RC=%errorlevel%

echo.
if "%RC%"=="0" (
    echo ============================================================
    echo [OK] Flash completed on %PORT%. The device is rebooting.
    echo     Open a serial monitor at 115200 baud to check boot logs:
    echo     - splash screen shows real init progress, then main UI
    echo     - sample rate should be about 4000 Hz, no faults
    echo ============================================================
) else (
    echo ============================================================
    echo [FAIL] Flash failed on %PORT% with exit code %RC%.
    echo     Things to check:
    echo     1. Correct COM port in Device Manager ^(used: %PORT%^)
    echo     2. Close serial monitors / other tools using this port
    echo     3. Try a lower baud rate: set ESPBAUD=115200
    echo     4. If download mode is not entered, hold BOOT and retry
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
