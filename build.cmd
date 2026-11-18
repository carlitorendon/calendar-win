@echo off
setlocal
rem Builds build\Calendar.exe with the MSVC toolchain (static CRT, no runtime dependencies).
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio C++ tools not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
rc /nologo /fo build\calendar.res src\calendar.rc || exit /b 1
cl /nologo /std:c++17 /utf-8 /O2 /MT /W4 /EHsc /DUNICODE /D_UNICODE src\calendar.cpp build\calendar.res /Fo:build\ /Fe:build\Calendar.exe /link /SUBSYSTEM:WINDOWS || exit /b 1
echo Built build\Calendar.exe
