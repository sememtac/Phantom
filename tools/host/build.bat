@echo off
rem Build the score renderer. tools/score_audio.py runs this file.
rem
rem This build takes ONE game source file, src\vg\vg_synth.cpp. To build the whole
rem game for the desktop, use host\build.ps1 instead. The renderer needs the
rem synthesiser and nothing else, and it must not pull in the simulation.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :nocompiler
set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :nocompiler
set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto :nocompiler

rem vcvars64.bat writes a harmless complaint about vswhere to stderr. Send the
rem output to nul so that it does not look like a build failure.
call "%VCVARS%" >nul 2>&1
cd /d "%~dp0"
if not exist obj mkdir obj
if not exist objlive mkdir objlive
rem The offline renderer, which writes a WAV file.
cl /nologo /O2 /EHsc /W3 /Fo:obj\ /Fe:score_render.exe score_render.cpp ..\..\src\vg\vg_synth.cpp
if errorlevel 1 exit /b 1

rem The live engine, which plays through the sound card. winmm.lib is that.
cl /nologo /O2 /EHsc /W3 /Fo:objlive\ /Fe:score_live.exe score_live.cpp ..\..\src\vg\vg_synth.cpp winmm.lib
exit /b %ERRORLEVEL%

:nocompiler
echo no MSVC x64 toolchain. Install the C++ build tools of Visual Studio 2022.
exit /b 2
