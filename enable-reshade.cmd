@echo off
rem Restores the ReShade DXGI proxy parked by disable-reshade.cmd.
set "GAMEDIR=E:\SteamLibrary\steamapps\common\BONELAB"
if not "%~1"=="" set "GAMEDIR=%~1"
if exist "%GAMEDIR%\dxgi.dll.reshade-off" ( ren "%GAMEDIR%\dxgi.dll.reshade-off" "dxgi.dll" && echo ReShade restored. ) else echo dxgi.dll.reshade-off not present, nothing to do.
pause
