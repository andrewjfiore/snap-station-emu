@echo off
REM ========================================================================
REM build_dev.bat
REM
REM Self-contained build launcher. Sets up the MSVC x64 environment via
REM vcvars64.bat, then runs build_win.bat from the script's own directory.
REM Safe to invoke from a plain cmd, an IDE terminal, or Git Bash (where
REM %ProgramFiles(x86)% and the VS Installer dir may not be on PATH).
REM
REM Uses !VAR! delayed expansion inside if-blocks because the paths
REM contain '(x86)' and the ')' otherwise terminates the if-( ... ).
REM ========================================================================

setlocal EnableDelayedExpansion

set "ProgramFiles(x86)=C:\Program Files (x86)"
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;!PATH!"

set "SCRIPT_DIR=%~dp0"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if not exist "!VCVARS!" (
    echo ERROR: vcvars64.bat not found at !VCVARS!
    exit /b 1
)

call "!VCVARS!" >nul
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed
    exit /b 1
)

cd /d "!SCRIPT_DIR!"
call "!SCRIPT_DIR!build_win.bat" %*
exit /b !ERRORLEVEL!
