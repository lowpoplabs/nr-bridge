@echo off
setlocal
cd /d "%~dp0"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" ( echo vcvars64.bat not found at "%VCVARS%" & exit /b 1 )
call "%VCVARS%" >nul
if not exist build mkdir build
set CLFLAGS=/nologo /std:c++17 /O2 /MT /W3 /EHsc /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Isrc

echo === nr-bridge.dll
cl %CLFLAGS% /LD src\nr_bridge_layer.cpp /Fo:build\ /Fe:build\nr-bridge.dll /link d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib user32.lib kernel32.lib advapi32.lib /OPT:REF || exit /b 1
del /q build\nr-bridge.exp build\nr-bridge.lib 2>nul

echo === nvngx.dll_nr_bridge.dll (forwarder)
cl %CLFLAGS% /LD src\nr_forwarder.cpp /Fo:build\ /Fe:build\nvngx.dll_nr_bridge.dll /link kernel32.lib || exit /b 1
del /q build\nvngx.dll_nr_bridge.exp build\nvngx.dll_nr_bridge.lib 2>nul

echo === openvr_api.dll (OpenVR proxy front end)
if not exist build\openvr mkdir build\openvr
cl %CLFLAGS% /LD src\nr_openvr_proxy.cpp /Fo:build\ /Fe:build\openvr\openvr_api.dll /link d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib user32.lib kernel32.lib advapi32.lib /OPT:REF || exit /b 1
del /q build\openvr\openvr_api.exp build\openvr\openvr_api.lib 2>nul
copy /y build\nvngx.dll_nr_bridge.dll build\openvr\ >nul

echo === openvr_api.orig.dll (fake runtime for test_openvr)
if not exist build\openvr-test mkdir build\openvr-test
cl %CLFLAGS% /LD test\test_fake_openvr.cpp /Fo:build\ /Fe:build\openvr-test\openvr_api.orig.dll /link kernel32.lib || exit /b 1
del /q build\openvr-test\openvr_api.orig.exp build\openvr-test\openvr_api.orig.lib 2>nul
copy /y build\openvr\openvr_api.dll build\openvr-test\ >nul
copy /y build\nvngx.dll_nr_bridge.dll build\openvr-test\ >nul

echo === test_openvr.exe
cl %CLFLAGS% test\test_openvr.cpp /Fo:build\ /Fe:build\test_openvr.exe /link d3d11.lib kernel32.lib user32.lib || exit /b 1

echo === nr-bridge-setup.exe (installer)
cl %CLFLAGS% setup\nr_bridge_setup.cpp /Fo:build\ /Fe:build\nr-bridge-setup.exe /link user32.lib comctl32.lib shell32.lib advapi32.lib ole32.lib shlwapi.lib gdi32.lib /SUBSYSTEM:WINDOWS || exit /b 1

echo === test_direct.exe
cl %CLFLAGS% test\test_direct.cpp /Fo:build\ /Fe:build\test_direct.exe /link d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib user32.lib kernel32.lib advapi32.lib || exit /b 1

echo === test_core.exe
cl %CLFLAGS% test\test_core.cpp /Fo:build\ /Fe:build\test_core.exe /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib kernel32.lib || exit /b 1

echo === test_layer.exe
cl %CLFLAGS% test\test_layer.cpp /Fo:build\ /Fe:build\test_layer.exe /link kernel32.lib || exit /b 1

copy /y nr-bridge.json build\ >nul
if exist nr-bridge.cfg copy /y nr-bridge.cfg build\ >nul
del /q build\*.obj 2>nul
echo === done: build\nr-bridge.dll
dir /b build
