@echo off
rem MONITOR OFF: parks the ReShade proxy DLL in the game folder so it no longer loads (dxgi.dll, d3d11.dll or
rem d3d12.dll -> <name>.reshade-off). ReShade + the RenoDX add-on are what run DLSS 5 on the desktop window; with the
rem headset bridge on as well they spend a Neural Rendering pass on the mirror every frame. The installer's
rem "Monitor off" button does the same thing.
set "GAMEDIR=E:\SteamLibrary\steamapps\common\BONELAB"
if not "%~1"=="" set "GAMEDIR=%~1"
set DONE=0
for %%n in (dxgi.dll d3d11.dll d3d12.dll) do (
    if exist "%GAMEDIR%\%%n" (
        if exist "%GAMEDIR%\%%n.reshade-off" del /q "%GAMEDIR%\%%n.reshade-off"
        ren "%GAMEDIR%\%%n" "%%n.reshade-off" && echo ReShade parked: %%n -^> %%n.reshade-off && set DONE=1
    )
)
if "%DONE%"=="0" echo no dxgi.dll / d3d11.dll / d3d12.dll in %GAMEDIR%, nothing to do.
pause
