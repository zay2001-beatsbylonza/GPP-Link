@echo off
rem Double-click to start GPP Link. Leave this window open while you use your MPCs.
rem
rem Change this to your Beats folder if it lives somewhere else, for example:
rem   set "BEATS_FOLDER=%USERPROFILE%\Desktop\BEATS FOLDER"
set "BEATS_FOLDER=%USERPROFILE%\Music\Beats"

title GPP Link
cd /d "%~dp0"
set "APP=gpp-link-hub.exe"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" if exist "gpp-link-hub-arm64.exe" set "APP=gpp-link-hub-arm64.exe"
rem files from the internet are marked "blocked"; clear that for this folder (like the Mac's quarantine)
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-ChildItem -LiteralPath '%~dp0' | Unblock-File" >nul 2>&1
if not exist "%APP%" (
  echo.
  echo Can't find %APP% next to this file, in:
  echo   %~dp0
  echo Unzip the whole download first ^(right-click the zip, Extract All^) and run this file from the
  echo extracted "GPP Link" folder. If the .exe was there before, Windows Security may have removed it:
  echo open Windows Security, Virus ^& threat protection, Protection history, and choose Restore.
  echo.
  pause
  exit /b 1
)
"%APP%" --dir "%BEATS_FOLDER%"
echo.
echo GPP Link stopped. Press any key to close this window.
pause >nul
