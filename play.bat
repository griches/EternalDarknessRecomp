@echo off
rem Play the Windows build: play.bat [disc image] (default dump\<game id>.iso).
rem Keyboard and controller as in play.sh. Close the window to quit.
cd /d "%~dp0"
set /p id=<generated\game_id.txt
set "iso=%~1"
if "%iso%"=="" set "iso=dump\%id%.iso"
if not exist logs mkdir logs
rem Keep the last session's log: it is the only record of what went wrong.
if exist logs\play.log move /y logs\play.log logs\play-prev.log >nul
build-win\EternalDarkness.exe generated\main.dol --iso "%iso%" --max-blocks 1000000000000000 > logs\play.log 2>&1
