@echo off
setlocal EnableDelayedExpansion
rem nr-bridge OpenVR front end: installs the openvr_api.dll proxy into a SteamVR-native (OpenVR) game.
rem Usage: install-openvr.cmd "<game folder>"      e.g. install-openvr.cmd "E:\SteamLibrary\steamapps\common\BONEWORKS"
rem The game's own openvr_api.dll is renamed openvr_api.orig.dll next to the proxy; uninstall-openvr.cmd reverses it.
rem 64-bit games only. Needs nvngx_dlssnr.dll (the DLSS 5 model, 158 MB) in this folder or in the BONELAB folder.
rem The log is written next to the proxy as nr-bridge-(game exe name).log, so each game keeps its own log.
cd /d "%~dp0"
if "%~1"=="" ( echo usage: %~nx0 "<game folder>" & exit /b 1 )
set "GAME=%~1"
if not exist "%GAME%" ( echo game folder not found: "%GAME%" & exit /b 1 )
set "FOUND="
for /f "delims=" %%F in ('dir /s /b "%GAME%\openvr_api.dll" 2^>nul') do (
    if not defined FOUND set "FOUND=%%F"
)
if not defined FOUND ( echo no openvr_api.dll under "%GAME%": this is not an OpenVR game ^(BONELAB is OpenXR: use install-bridge.cmd^) & exit /b 1 )
for %%F in ("%FOUND%") do set "PLUG=%%~dpF"
set "PLUG=%PLUG:~0,-1%"
echo game openvr_api.dll: "%FOUND%"
echo plugin folder:       "%PLUG%"
if exist "%PLUG%\openvr_api.orig.dll" (
    echo openvr_api.orig.dll already there: refreshing the proxy only
) else (
    ren "%FOUND%" openvr_api.orig.dll || ( echo could not rename the game's openvr_api.dll ^(is the game running?^) & exit /b 1 )
    echo renamed the game's DLL to openvr_api.orig.dll
)
copy /y openvr_api.dll "%PLUG%\" >nul || ( echo copy failed & exit /b 1 )
copy /y nvngx.dll_nr_bridge.dll "%PLUG%\" >nul
if not exist "%PLUG%\nr-bridge.cfg" copy /y nr-bridge.cfg "%PLUG%\" >nul
set "MODEL="
if exist "%PLUG%\nvngx_dlssnr.dll" set "MODEL=%PLUG%\nvngx_dlssnr.dll"
if not defined MODEL if exist "nvngx_dlssnr.dll" ( copy /y nvngx_dlssnr.dll "%PLUG%\" >nul && set "MODEL=%PLUG%\nvngx_dlssnr.dll" )
if not defined MODEL (
    for %%D in ("E:\SteamLibrary\steamapps\common\BONELAB" "%GAME%\..\BONELAB") do (
        if not defined MODEL if exist "%%~D\nvngx_dlssnr.dll" ( copy /y "%%~D\nvngx_dlssnr.dll" "%PLUG%\" >nul && set "MODEL=%PLUG%\nvngx_dlssnr.dll" )
    )
)
if defined MODEL ( echo model DLL:           "!MODEL!" ) else ( echo WARNING: nvngx_dlssnr.dll not found - copy it into "%PLUG%" or the game will run without the model )
echo.
echo installed. Log: "%PLUG%\nr-bridge-(game exe name).log"   Settings: "%PLUG%\nr-bridge.cfg". F10 toggles, F9 reloads the cfg.
exit /b 0
