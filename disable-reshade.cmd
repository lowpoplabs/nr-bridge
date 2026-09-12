@echo off
rem Parks the ReShade DXGI proxy so it no longer loads (renames dxgi.dll -> dxgi.dll.reshade-off).
rem Direct mode does not use ReShade; leaving it active costs a Neural Rendering pass on the mirror window.
set "GAMEDIR=E:\SteamLibrary\steamapps\common\BONELAB"
if not "%~1"=="" set "GAMEDIR=%~1"
if exist "%GAMEDIR%\dxgi.dll" ( ren "%GAMEDIR%\dxgi.dll" "dxgi.dll.reshade-off" && echo ReShade parked. ) else echo dxgi.dll not present, nothing to do.
pause
