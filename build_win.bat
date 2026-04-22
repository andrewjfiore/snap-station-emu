@echo off
REM ========================================================================
REM build_win.bat
REM
REM Builds the Snap Station emulation components on Windows.
REM Run from an "x64 Native Tools Command Prompt for VS 2022".
REM
REM Produces:
REM   test_sticker_sheet.exe                Standalone: composes a demo sheet
REM   test_joybus.exe                       Standalone: drives the protocol
REM   test_smart_card.exe                   Standalone: exercises credit reader
REM   mupen64plus-input-snapstation.dll     Mupen64Plus input plugin
REM ========================================================================

setlocal EnableDelayedExpansion

where cl >nul 2>&1
if errorlevel 1 (
    echo ERROR: cl.exe not on PATH. Open an "x64 Native Tools Command Prompt".
    exit /b 1
)

set CFLAGS=/W3 /O2 /MD /nologo /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /Isrc
set WINLIBS=user32.lib gdi32.lib comdlg32.lib winspool.lib shell32.lib

echo.
echo [1/4] test_sticker_sheet.exe ...
cl %CFLAGS% src\sticker_sheet.c test\test_sticker_sheet.c /Fe:test_sticker_sheet.exe 1>nul
if errorlevel 1 goto fail

echo [2/4] test_joybus.exe ...
cl %CFLAGS% src\joybus_snapstation.c test\test_joybus.c /Fe:test_joybus.exe 1>nul
if errorlevel 1 goto fail

echo [3/4] test_smart_card.exe ...
cl %CFLAGS% src\smart_card.c test\test_smart_card.c /Fe:test_smart_card.exe 1>nul
if errorlevel 1 goto fail

echo [4/4] mupen64plus-input-snapstation.dll ...
cl %CFLAGS% /LD ^
   src\joybus_snapstation.c ^
   src\sticker_sheet.c ^
   src\smart_card.c ^
   src\snap_station_win32.c ^
   src\m64p_input_plugin.c ^
   /Fe:mupen64plus-input-snapstation.dll ^
   /link %WINLIBS% 1>nul
if errorlevel 1 goto fail

del /q *.obj *.exp *.lib 2>nul

echo.
echo ==========================================================
echo  Built successfully:
echo    test_sticker_sheet.exe
echo    test_joybus.exe
echo    test_smart_card.exe
echo    mupen64plus-input-snapstation.dll
echo.
echo  Quick smoke test:
echo    test_sticker_sheet.exe ^&^& test_joybus.exe ^&^& test_smart_card.exe
echo.
echo  To install the plugin, copy the DLL alongside your Mupen64Plus
echo  binaries and set in mupen64plus.cfg:
echo    [UI-Console]
echo    InputPlugin = "mupen64plus-input-snapstation.dll"
echo ==========================================================
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1
