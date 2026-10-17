@echo off
rem stage-win-build.bat [TREE] [OUT]: on the Windows build PC, gather what the
rem Windows package needs from a finished build-win: the exe and its DLLs, the
rem x64 Visual C++ runtime DLLs Dawn, SDL and DXC import (Microsoft lets them
rem ship beside the app), and the licence files of the libraries the build
rem fetched. Copy OUT to the Mac and run tools/win/make-win-package.sh on it.
set "T=%~1"
if "%T%"=="" set "T=%~dp0..\.."
set "P=%~2"
if "%P%"=="" set "P=%USERPROFILE%\src\winpkg"
rem The newest x64 CRT redist on this PC (VS Build Tools 18, MSVC 14.50).
set "CRT=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Redist\MSVC\14.50.35710\x64\Microsoft.VC145.CRT"
if exist "%P%" rmdir /s /q "%P%"
mkdir "%P%\bin"
mkdir "%P%\lic"
copy /y "%T%\build-win\EternalDarkness.exe" "%P%\bin\" >nul
copy /y "%T%\build-win\*.dll" "%P%\bin\" >nul
for %%f in (msvcp140.dll msvcp140_atomic_wait.dll vcruntime140.dll vcruntime140_1.dll) do copy /y "%CRT%\%%f" "%P%\bin\" >nul
for /d %%d in ("%T%\build-win\_deps\*-src") do (
  for %%l in (LICENSE LICENSE.txt LICENSE.TXT LICENSE.md COPYING) do if exist "%%d\%%l" (
    mkdir "%P%\lic\%%~nxd" 2>nul
    copy /y "%%d\%%l" "%P%\lic\%%~nxd\" >nul
  )
)
if exist "%T%\build-win\_deps\freetype-src\docs\FTL.TXT" copy /y "%T%\build-win\_deps\freetype-src\docs\FTL.TXT" "%P%\lic\freetype-src\" >nul
xcopy /e /i /q /y "%T%\build-win\_deps\sdl3_prebuilt-src\licenses" "%P%\sdl-licenses" >nul
rem The commit the exe was built from (.build-commit at the tree root).
for /f "tokens=1" %%c in (%T%\.build-commit) do echo %%c> "%P%\build-commit"
dir /b "%P%\bin"
