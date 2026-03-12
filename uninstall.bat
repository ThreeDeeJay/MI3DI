@echo off
:: MI3DI Uninstaller
setlocal EnableDelayedExpansion

echo ============================================================
echo   MI3DI v1.0.0 ^- Uninstaller
echo ============================================================
echo.

net session >nul 2>&1
if %errorLevel% neq 0 (
    echo ERROR: Must be run as Administrator.
    pause & exit /b 1
)

set "REGKEY=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32"
set "REMOVED=0"

for /l %%N in (0,1,9) do (
    for /f "tokens=3" %%V in ('reg query "%REGKEY%" /v "midi%%N" 2^>nul') do (
        if /i "%%V"=="mi3di.dll" (
            reg delete "%REGKEY%" /v "midi%%N" /f >nul
            echo Removed registry key: midi%%N
            set "REMOVED=1"
        )
    )
)

if "!REMOVED!"=="0" echo No MI3DI registry entries found.

:: Remove DLL files
for %%D in ("%SystemRoot%\System32" "%SystemRoot%\SysWOW64") do (
    if exist "%%~D\mi3di.dll" (
        del /f "%%~D\mi3di.dll" >nul 2>&1 && echo Removed %%~D\mi3di.dll
    )
)

echo.
echo MI3DI uninstalled.
pause
