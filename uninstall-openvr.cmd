@echo off
setlocal
rem Removes the nr-bridge OpenVR proxy from a game and restores its own openvr_api.dll.
rem Usage: uninstall-openvr.cmd "<game folder>"
if "%~1"=="" ( echo usage: %~nx0 "<game folder>" & exit /b 1 )
set "GAME=%~1"
set "FOUND="
for /f "delims=" %%F in ('dir /s /b "%GAME%\openvr_api.orig.dll" 2^>nul') do ( if not defined FOUND set "FOUND=%%F" )
if not defined FOUND ( echo nothing to uninstall under "%GAME%" ^(no openvr_api.orig.dll^) & exit /b 1 )
for %%F in ("%FOUND%") do set "PLUG=%%~dpF"
set "PLUG=%PLUG:~0,-1%"
del /q "%PLUG%\openvr_api.dll" 2>nul
ren "%FOUND%" openvr_api.dll || ( echo could not restore openvr_api.dll ^(is the game running?^) & exit /b 1 )
del /q "%PLUG%\nvngx.dll_nr_bridge.dll" "%PLUG%\nvngx.dll_bonelab_nr.dll" 2>nul
echo restored the game's openvr_api.dll in "%PLUG%". Left in place: nr-bridge.cfg, nr-bridge-*.log, nvngx_dlssnr.dll.
exit /b 0
