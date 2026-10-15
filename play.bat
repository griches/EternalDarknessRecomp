@echo off
rem Play the Windows build: play.bat [disc image] (default dump\<game id>.iso).
rem Keyboard and controller as in play.sh. Close the window to quit.
cd /d "%~dp0"
set /p id=<generated\game_id.txt
set "iso=%~1"
if "%iso%"=="" set "iso=dump\%id%.iso"
if not exist logs mkdir logs
build-win\EternalDarkness.exe generated\main.dol --iso "%iso%" --max-blocks 1000000000000000 > logs\play.log 2>&1
