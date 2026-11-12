@echo off
if "%~1"=="" (
  echo Usage: flash-controller.cmd COM8
  exit /b 2
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\idf.ps1" -Project controller -Action flash -Port %*
exit /b %ERRORLEVEL%
