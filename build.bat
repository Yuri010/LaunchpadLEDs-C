@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

:: Check if the GCC compiler is reachable and works
gcc --version > nul
if %errorlevel% NEQ 0 (
    echo Error: GCC is not installed or reachable
    goto :end
)

:main
set "targetFile=lp.exe"
if exist "%targetFile%" (
    echo Warning: Target '%targetFile%' already exists
    choice /M "Overwrite"
    if !errorlevel! NEQ 1 goto :end
)

echo.
gcc -v -std=c11 -Wall -Wextra -Wpedantic lp.c midi_windows.c -o "%targetFile%" -lwinmm
echo.
if %errorlevel% NEQ 0 goto :end

echo Compile finished successfully
choice /M "Run file %targetFile% immediately?"
if %errorlevel% EQU 1 start "" cmd /c "%targetFile% & echo. & pause"

:: Exit either immediately (when called from the terminal)
:: or leave the window open for the user to read before exiting.
:end
set "seperateWindow="
if /I not "%cmdcmdline:"=%" EQU "%comspec% " set seperateWindow = 1
if /I not "%cmdcmdline%" EQU "%comspec%" set seperateWindow = 1
if DEFINED seperateWindow (
    echo.
    echo Press any key to exit...
    pause > nul
)
exit /b