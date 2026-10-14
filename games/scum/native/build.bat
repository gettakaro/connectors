@echo off
rem Builds main.dll (UE4SS C++ mod, the Takaro transport) and takaro-smoke.exe with MSVC v143, x64, /MD Release.
rem Run from this folder: build.bat   Output: build\main.dll, build\takaro-smoke.exe
setlocal
cd /d "%~dp0"
if not defined VCToolsInstallDir call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build mkdir build
set CFLAGS=/nologo /std:c++20 /O2 /MD /EHsc /W3 /wd4996 /DNDEBUG /DUNICODE /D_UNICODE /utf-8 /Zi
set SRC=src\transport.cpp src\transport_winhttp.cpp src\bridge.cpp src\log.cpp
lib /nologo /def:ue4ss\UE4SS.def /machine:x64 /out:build\UE4SS.lib || exit /b 1
cl %CFLAGS% /LD /Fobuild\ /Fdbuild\main.pdb /Febuild\main.dll src\mod_main.cpp %SRC% ^
  /link /DEBUG /OPT:REF /OPT:ICF build\UE4SS.lib winhttp.lib crypt32.lib || exit /b 1
cl %CFLAGS% /Fobuild\ /Fdbuild\smoke.pdb /Febuild\takaro-smoke.exe smoke\smoke_main.cpp %SRC% ^
  /link winhttp.lib crypt32.lib || exit /b 1
echo BUILD OK
