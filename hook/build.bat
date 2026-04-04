@echo off
REM Build script for True Blue English patch proxy DLL
REM Run from a Visual Studio x86 Developer Command Prompt
REM
REM Usage: build.bat
REM Output: winmm.dll (copy to game folder alongside dictionary.txt)

echo Building winmm.dll proxy...
cl /LD /O2 /EHsc /W3 winmm_proxy.cpp /Fe:winmm.dll /link /DEF:winmm.def /MACHINE:X86 user32.lib

if %ERRORLEVEL% == 0 (
    echo.
    echo Build successful! Output: winmm.dll
    echo.
    echo Installation:
    echo   1. Copy winmm.dll to the game folder
    echo   2. Copy dictionary.txt to the game folder
    echo   3. Launch the game normally
    echo.
    echo To remove the patch, just delete winmm.dll from the game folder.
) else (
    echo.
    echo Build FAILED. Make sure you're running from a VS x86 Developer Command Prompt.
)
