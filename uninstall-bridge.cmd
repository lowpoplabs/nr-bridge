@echo off
setlocal EnableExtensions
rem Removes the nr-bridge layer registration for this user and deletes its folder.
set "GAMEDIR=E:\SteamLibrary\steamapps\common\BONELAB"
if not "%~1"=="" set "GAMEDIR=%~1"
set "DST=%GAMEDIR%\nr-bridge"
reg delete "HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit" /v "%DST%\nr-bridge.json" /f >nul 2>&1
reg delete "HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit" /v "%DST%\bonelab-nr-bridge.json" /f >nul 2>&1
if exist "%DST%" rd /s /q "%DST%"
echo Removed. Remaining per-user implicit layers:
reg query "HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit" 2>nul
pause
