@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-firmware.ps1" -Target receiver
set "KMS_BUILD_EXIT=%errorlevel%"
echo.
pause
exit /b %KMS_BUILD_EXIT%
