@echo off
rem Double-click this file to build the PC version and play it. It runs play.ps1.
rem
rem Windows does not run a .ps1 file on a double-click: it opens the file in
rem Notepad. "Run with PowerShell" in the menu fails on a new machine, because
rem Windows blocks scripts there, and the window closes before you can read why.
rem This file starts play.ps1 with -ExecutionPolicy Bypass, which changes no
rem setting.
rem
rem From a command prompt, give this file the same arguments as play.ps1:
rem
rem     play.cmd
rem     play.cmd check
rem     play.cmd device -Port COM6
rem
rem If play.ps1 stops with an error, the window stays open so you can read it.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0play.ps1" %*
if errorlevel 1 (
    echo.
    echo play.ps1 stopped with an error. The message is above.
    pause
    exit /b 1
)
exit /b 0
