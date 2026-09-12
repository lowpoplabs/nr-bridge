@echo off
setlocal EnableExtensions
rem ------------------------------------------------------------------------------------------
rem  nr-bridge installer for the GAMING PC.
rem  RUN THIS AS THE ACCOUNT THAT PLAYS THE GAME, NOT as admin: the layer is registered
rem  per-user (HKCU) so no elevation is needed and the game account is the one that must see it.
rem  Files go to <game>\nr-bridge\. Leaves the existing ReShade dxgi.dll install untouched.
rem ------------------------------------------------------------------------------------------
set "GAMEDIR=E:\SteamLibrary\steamapps\common\BONELAB"
if not "%~1"=="" set "GAMEDIR=%~1"
set "DST=%GAMEDIR%\nr-bridge"
if not exist "%GAMEDIR%\BONELAB_Steam_Windows64.exe" ( echo BONELAB not found in %GAMEDIR% & pause & exit /b 1 )
if not exist "%~dp0nr-bridge.dll" ( echo nr-bridge.dll is not next to this script & pause & exit /b 1 )
if not exist "%DST%" mkdir "%DST%"
copy /y "%~dp0nr-bridge.dll"  "%DST%\" >nul || ( echo copy failed & pause & exit /b 1 )
copy /y "%~dp0nr-bridge.json" "%DST%\" >nul
copy /y "%~dp0nvngx.dll_nr_bridge.dll" "%DST%\" >nul
if not exist "%DST%\nr-bridge.cfg" copy /y "%~dp0nr-bridge.cfg" "%DST%\" >nul
if not exist "%GAMEDIR%\nvngx_dlssnr.dll" echo WARNING: nvngx_dlssnr.dll is not in the game folder; direct mode needs it there.
if exist "%GAMEDIR%\dxgi.dll" echo NOTE: ReShade (dxgi.dll) is still active in the game folder. Direct mode does not need it; run disable-reshade.cmd to stop the mirror-window NR pass.
rem an install from before the rename (bonelab-nr-bridge.*): keep its cfg under the new name, drop the rest
if exist "%DST%\bonelab-nr-bridge.cfg" if not exist "%DST%\nr-bridge.cfg" ren "%DST%\bonelab-nr-bridge.cfg" nr-bridge.cfg
reg delete "HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit" /v "%DST%\bonelab-nr-bridge.json" /f >nul 2>&1
del /q "%DST%\bonelab-nr-bridge.dll" "%DST%\bonelab-nr-bridge.json" "%DST%\nvngx.dll_bonelab_nr.dll" "%DST%\bonelab-nr-bridge.cfg" 2>nul
reg add "HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit" /v "%DST%\nr-bridge.json" /t REG_DWORD /d 0 /f >nul || ( echo registry write failed & pause & exit /b 1 )
echo.
echo Installed to %DST% and registered for user %USERNAME%:
reg query "HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
echo.
echo Now launch BONELAB normally. Log: %DST%\nr-bridge.log   Settings: %DST%\nr-bridge.cfg
echo F10 toggles the bridge on/off in-game for an A/B comparison.
pause
