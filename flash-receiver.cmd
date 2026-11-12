@echo off
if "%~1"=="" (
  echo Usage: flash-receiver.cmd COM4 [-ExpectedMac aa:bb:cc:dd:ee:ff] [-SkipBuild]
  exit /b 2
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\flash-receiver.ps1" %*
exit /b %ERRORLEVEL%
