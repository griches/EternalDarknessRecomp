@echo off
rem Eternal Darkness Recomp (Windows, 64-bit): the ready-made game folder.
rem Put your own image of the European disc (GEDP01, .iso or .gcm) in the dump
rem folder, then run this. The first start reads the game's program from the
rem disc into generated\main.dol; the graphics, sound and music always come
rem from the disc.
cd /d "%~dp0"
set "iso="
rem The disc as the launcher's own folder scan names it ("dump/x.iso", "x.iso"),
rem so it isn't listed twice.
for %%f in (dump\*.iso dump\*.gcm) do if not defined iso set "iso=dump/%%~nxf"
for %%f in (*.iso *.gcm) do if not defined iso set "iso=%%~nxf"
if not defined iso (
  echo Put your Eternal Darkness disc image ^(.iso or .gcm of the European disc^) in
  echo   %~dp0dump
  echo then start the game again.
  pause
  exit /b 1
)
if not exist logs mkdir logs
rem Keep the last session's log: it is the only record of what went wrong.
if exist logs\play.log move /y logs\play.log logs\play-prev.log >nul
rem generated/main.dol with a forward slash: the first start makes the folder
rem from the path's last "/" (b453381) before it writes main.dol there.
EternalDarkness.exe generated/main.dol --iso "%iso%" --max-blocks 1000000000000000 > logs\play.log 2>&1
