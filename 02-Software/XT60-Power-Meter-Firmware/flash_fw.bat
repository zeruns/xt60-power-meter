@echo off
rem ============================================================
rem XT60 Power Meter firmware - Windows one-click flash script
rem
rem Usage: flash_fw.bat [PORT]
rem   - PORT comes from the 1st argument (%%1), default COM3
rem     e.g. flash_fw.bat COM5
rem   - flash baud rate can be set via env ESPBAUD (default 921600)
rem     e.g. set ESPBAUD=921600
rem   - if env IDF_PATH is already set (ESP-IDF CMD/PowerShell opened),
rem     it is used as-is; otherwise fall back to the local default path.
rem
rem NOTE: keep this file ASCII-only. cmd.exe reads .bat in the OEM
rem codepage (e.g. GBK); UTF-8 multibyte sequences can swallow the
rem line ending and corrupt parsing. Chinese docs live in README.md
rem / CONTRIBUTING.md instead.
rem ============================================================

if defined IDF_PATH goto :have_idf
rem ---- IDF_PATH not set: fall back to the local install path ----
set IDF_PATH=D:\Espressif\v6.1\esp-idf
set IDF_TOOLS_PATH=C:\Espressif
set PATH=D:\Espressif\tools\idf-git\2.44.0\cmd;%PATH%

:have_idf
call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1

rem ---- PORT: first argument, default COM3 ----
set PORT=%1
if "%PORT%"=="" set PORT=COM3
idf.py -p %PORT% flash
