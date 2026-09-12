@echo off
setlocal
pushd "%~dp0" || exit /b 1
rem Build, assemble dist\, and zip it to releases\nr-bridge-<version>.zip.
call "%~dp0build.cmd" || exit /b 1
pushd src
for /f "tokens=3 delims= " %%v in ('findstr /c:"#define BRIDGE_VERSION" nr_bridge_layer.cpp') do set VER=%%~v
popd
if "%VER%"=="" ( echo could not read BRIDGE_VERSION & exit /b 1 )
if exist dist rmdir /s /q dist
mkdir dist\openvr
copy /y build\nr-bridge.dll dist\ >nul || exit /b 1
copy /y build\nvngx.dll_nr_bridge.dll dist\ >nul || exit /b 1
copy /y build\nr-bridge-setup.exe dist\ >nul || exit /b 1
copy /y nr-bridge.json dist\ >nul || exit /b 1
copy /y nr-bridge.cfg dist\ >nul || exit /b 1
copy /y install-bridge.cmd dist\ >nul || exit /b 1
copy /y uninstall-bridge.cmd dist\ >nul || exit /b 1
copy /y enable-reshade.cmd dist\ >nul || exit /b 1
copy /y disable-reshade.cmd dist\ >nul || exit /b 1
copy /y README.md dist\ >nul || exit /b 1
copy /y CHANGELOG.md dist\ >nul || exit /b 1
copy /y LICENSE dist\ >nul || exit /b 1
copy /y build\openvr\openvr_api.dll dist\openvr\ >nul || exit /b 1
copy /y build\openvr\nvngx.dll_nr_bridge.dll dist\openvr\ >nul || exit /b 1
copy /y nr-bridge.cfg dist\openvr\ >nul || exit /b 1
copy /y install-openvr.cmd dist\openvr\ >nul || exit /b 1
copy /y uninstall-openvr.cmd dist\openvr\ >nul || exit /b 1
if not exist releases mkdir releases
powershell -NoProfile -Command "Compress-Archive -Path 'dist\*' -DestinationPath 'releases\nr-bridge-%VER%.zip' -Force" || exit /b 1
echo === packaged releases\nr-bridge-%VER%.zip
