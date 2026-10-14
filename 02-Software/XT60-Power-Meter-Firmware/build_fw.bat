@echo off
set IDF_PATH=D:\Espressif\v6.1\esp-idf
set IDF_TOOLS_PATH=C:\Espressif
set PATH=D:\Espressif\tools\idf-git\2.44.0\cmd;%PATH%
call %IDF_PATH%\export.bat
if errorlevel 1 exit /b 1
idf.py build
