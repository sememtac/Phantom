@echo off
rem One window to write the game's music. See tools\score_studio.py.
rem
rem     tools\studio.cmd
rem     tools\studio.cmd design\score\motif.score
rem
rem Double click this file, or run it from a shell. Run it from any directory.
rem With no file it opens design\score\motif.score.
setlocal

rem Make the file name full BEFORE the next line changes directory. A name that
rem the caller gave is relative to the caller's directory, not to this one.
set "SCORE="
if not "%~1"=="" set "SCORE=%~f1"

cd /d "%~dp0.."

rem pythonw opens no console window. Fall back to python, which keeps a console
rem and so shows an error that pythonw would hide.
set "PY=pythonw"
where pythonw >nul 2>&1 || set "PY=python"
where %PY% >nul 2>&1 || goto :nopython

if defined SCORE (
    start "" %PY% "tools\score_studio.py" "%SCORE%"
) else (
    start "" %PY% "tools\score_studio.py"
)
exit /b 0

:nopython
echo Cannot find Python. Install Python 3 and put it on the PATH.
pause
exit /b 2
