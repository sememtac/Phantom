@echo off
rem One window for the work on the pilot network. See tools\pilot_console.py.
cd /d "%~dp0.."
start "" pythonw "tools\pilot_console.py"
