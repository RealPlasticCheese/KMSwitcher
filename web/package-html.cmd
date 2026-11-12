@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 exit /b 1

where node >nul 2>&1
if errorlevel 1 (
  echo Node.js is required. Install a version supported by web/package.json.
  popd
  exit /b 1
)

rem Install the locked build dependencies, then inline JavaScript and CSS.
call npm.cmd ci --include=dev
if errorlevel 1 goto failed
call npm.cmd run build
if errorlevel 1 goto failed

echo.
echo Open "%~dp0dist\KMSwitcher.html" in desktop Chrome or Edge.
echo This single file can be copied anywhere and needs no server.
popd
exit /b 0

:failed
set "KMS_PACKAGE_EXIT=%errorlevel%"
echo Failed to package the HTML page. See the error above.
popd
exit /b %KMS_PACKAGE_EXIT%
