@echo off
rem Run from the Visual Studio x64 Developer Command Prompt.
setlocal
cd /d "%~dp0"
if not exist build mkdir build
rc /nologo /I res /fo build\app.res res\app.rc || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 ^
    /Fobuild\ /Febuild\WirelessBattery.exe src\*.cpp src\drivers\*.cpp build\app.res ^
    /link /subsystem:windows /manifest:no user32.lib shell32.lib advapi32.lib hid.lib setupapi.lib cfgmgr32.lib ^
    ole32.lib comctl32.lib gdi32.lib runtimeobject.lib || exit /b 1
echo Built build\WirelessBattery.exe
