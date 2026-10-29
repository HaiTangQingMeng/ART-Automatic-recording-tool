@echo off
setlocal

rem Locate VS Build Tools via vswhere
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

set "VS="
for /f "usebackq delims=" %%i in (`""%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`) do set "VS=%%i"

if not defined VS (
    echo [ERROR] Visual Studio Build Tools not found.
    pause
    exit /b 1
)

call "%VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul

cl /EHsc /O2 /utf-8 /W3 main.cpp /Fe:VHFRecorder.exe ole32.lib uuid.lib comctl32.lib shell32.lib user32.lib gdi32.lib /link /SUBSYSTEM:WINDOWS

if %errorlevel%==0 (
    echo.
    echo Build OK: VHFRecorder.exe
) else (
    echo.
    echo Build FAILED
)
pause
