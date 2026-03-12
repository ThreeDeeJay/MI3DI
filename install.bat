@echo off
:: MI3DI Installer
:: Registers mi3di.dll as a Windows WinMM MIDI output driver.
:: Must be run as Administrator.
setlocal EnableDelayedExpansion

echo ============================================================
echo   MI3DI v1.0.0 ^- Installer
echo ============================================================
echo.

net session >nul 2>&1
if %errorLevel% neq 0 (
    echo ERROR: This installer must be run as Administrator.
    pause & exit /b 1
)

set "SRCDIR=%~dp0"
set "DLLSRC=%SRCDIR%mi3di.dll"
if not exist "%DLLSRC%" (
    echo ERROR: mi3di.dll not found in %SRCDIR%
    pause & exit /b 1
)

:: Detect DLL bitness via PE machine field
for /f "tokens=*" %%A in ('powershell -NoProfile -Command "$b=[System.IO.File]::ReadAllBytes('%DLLSRC%');$off=[System.BitConverter]::ToInt32($b,60);$m=[System.BitConverter]::ToUInt16($b,$off+4);if($m-eq0x8664){'x64'}else{'x86'}"') do set "DLLARCH=%%A"

if "!DLLARCH!"=="x64" (
    set "SYSDIR=%SystemRoot%\System32"
) else (
    if exist "%SystemRoot%\SysWOW64\kernel32.dll" (
        set "SYSDIR=%SystemRoot%\SysWOW64"
    ) else (
        set "SYSDIR=%SystemRoot%\System32"
    )
)

echo DLL arch : !DLLARCH!
echo Target   : !SYSDIR!
echo.

copy /Y "%DLLSRC%" "!SYSDIR!\mi3di.dll" >nul || (echo ERROR: copy failed & pause & exit /b 1)
echo Copied mi3di.dll

if exist "%SRCDIR%OpenAL32.dll" (
    copy /Y "%SRCDIR%OpenAL32.dll" "!SYSDIR!\OpenAL32.dll" >nul
    echo Copied OpenAL32.dll
)

set "REGKEY=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32"
set "SLOT="
for /l %%N in (0,1,9) do (
    if not defined SLOT (
        reg query "%REGKEY%" /v "midi%%N" >nul 2>&1
        if !errorLevel! neq 0 set "SLOT=midi%%N"
    )
)
if not defined SLOT set "SLOT=midi9"

reg add "%REGKEY%" /v "!SLOT!" /t REG_SZ /d "mi3di.dll" /f >nul || (echo ERROR: registry write failed & pause & exit /b 1)
echo Registered as !SLOT!

echo.
echo ============================================================
echo   MI3DI installed!  Place default.sf2 / default.sfz beside
echo   mi3di.dll, or set:  MI3DI_SOUNDFONT=C:\path\to\your.sf2
echo   For logging set:    MI3DI_LOG=C:\path\to\mi3di.log
echo ============================================================
pause
