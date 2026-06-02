@echo off
REM Build ce_mcp_tcp.dll for Cheat Engine MCP Bridge
REM Requires Visual Studio Build Tools (cl.exe in PATH)
REM
REM Usage:
REM   build.bat          - builds x64 Release
REM   build.bat x86      - builds x86 Release
REM
REM Output: bin\x64\ce_mcp_tcp.dll or bin\x86\ce_mcp_tcp.dll

setlocal

set ARCH=x64
if /i "%1"=="x86" set ARCH=x86

set OUTDIR=bin\%ARCH%
if not exist %OUTDIR% mkdir %OUTDIR%

echo Building ce_mcp_tcp.dll (%ARCH%)...

cl.exe /nologo /O2 /LD /W3 ^
    /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_USRDLL" ^
    ce_mcp_tcp.c ^
    ws2_32.lib kernel32.lib user32.lib ^
    /Fe:%OUTDIR%\ce_mcp_tcp.dll ^
    /Fo:%OUTDIR%\ ^
    /link /DLL /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

REM Clean up intermediate files
del /q %OUTDIR%\*.obj %OUTDIR%\*.exp %OUTDIR%\*.lib 2>nul

echo.
echo SUCCESS: %OUTDIR%\ce_mcp_tcp.dll
echo.
echo Copy this DLL next to ce_mcp_bridge.lua on the target machine.

endlocal
