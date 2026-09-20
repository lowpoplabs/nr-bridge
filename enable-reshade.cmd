@echo off
rem MONITOR ON: restores the ReShade proxy DLL parked by disable-reshade.cmd or the installer's "Monitor off" button
rem (<name>.reshade-off -> dxgi.dll / d3d11.dll / d3d12.dll), so DLSS 5 runs on the desktop window again for recording.
set "GAMEDIR=E:\SteamLibrary\steamapps\common\BONELAB"
if not "%~1"=="" set "GAMEDIR=%~1"
set DONE=0
for %%n in (dxgi.dll d3d11.dll d3d12.dll) do (
    if exist "%GAMEDIR%\%%n.reshade-off" (
        if exist "%GAMEDIR%\%%n" ( echo %%n is already present, leaving %%n.reshade-off alone ) else ( ren "%GAMEDIR%\%%n.reshade-off" "%%n" && echo ReShade restored: %%n && set DONE=1 )
    )
)
if "%DONE%"=="0" echo no parked *.reshade-off in %GAMEDIR%, nothing to do.
pause
