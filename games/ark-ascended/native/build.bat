@echo off
rem Builds TakaroAsa.dll with MSVC 14.44, x64, /MD Release, into native\build.
rem ASA_DEPS is the folder scripts/fetch-deps.sh filled with the pinned AsaApi headers, AsaApi.lib and fmt.
rem TAKARO_VERSION, when set, is the version stamped into the DLL; otherwise src\version.h decides.
setlocal
cd /d "%~dp0"
if not defined ASA_DEPS (echo ASA_DEPS is not set & exit /b 1)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo no Visual Studio with the C++ tools found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.44 >nul || exit /b 1
if not exist build mkdir build
set "API=%ASA_DEPS%\AsaApi\AsaApi\Core\Public"
set VERSION_DEF=
if defined TAKARO_VERSION set VERSION_DEF=/DTAKARO_PLUGIN_VERSION=\"%TAKARO_VERSION%\"
cl /nologo /std:c++20 /O2 /MD /EHsc /W1 /utf-8 /Zc:__cplusplus /Zi /DNDEBUG /D_WINDOWS /D_USRDLL /DUNICODE /D_UNICODE /DFMT_HEADER_ONLY /DPROJECT_NAME=\"TakaroAsa\" %VERSION_DEF% /I"%API%" /I"%API%\API\UE" /I"%ASA_DEPS%\fmt\include" /LD /Fobuild\ /Fdbuild\TakaroAsa.pdb /Febuild\TakaroAsa.dll src\plugin.cpp src\takaro_client.cpp src\logic.cpp src\transport.cpp src\transport_winhttp.cpp src\log.cpp /link /DEBUG /OPT:REF /OPT:ICF "%ASA_DEPS%\Lib\AsaApi.lib" winhttp.lib crypt32.lib bcrypt.lib || exit /b 1
echo BUILD OK
