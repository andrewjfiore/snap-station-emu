@echo off
setlocal EnableDelayedExpansion
set "ProgramFiles(x86)=C:\Program Files (x86)"
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;!PATH!"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /W3 /O2 /MD /nologo /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /Isrc /LD ^
   src\joybus_snapstation.c ^
   src\sticker_sheet.c ^
   src\smart_card.c ^
   src\snap_station_win32.c ^
   src\m64p_input_plugin.c ^
   /Fe:mupen64plus-input-snapstation.dll ^
   /link user32.lib gdi32.lib comdlg32.lib winspool.lib shell32.lib
