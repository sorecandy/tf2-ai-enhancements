@echo off
rem Needs: Visual Studio 2022 Build Tools (x64). Optional env: VCVARS (path to vcvars64.bat).
if not defined VCVARS set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
rem Also needs: RESHADE_SDK = ReShade source checkout (uses its include\ folder, add-on API),
rem             NGX_SDK = NVIDIA DLSS SDK checkout (github.com/NVIDIA/DLSS: include\ + lib\Windows_x86_64\x64\nvsdk_ngx_s.lib).
rem The NVIDIA SDK is NOT part of this repository (NVIDIA RTX SDKs License).
if not defined RESHADE_SDK (echo set RESHADE_SDK & exit /b 1)
if not defined NGX_SDK (echo set NGX_SDK & exit /b 1)
if not defined NGX_LIB set NGX_LIB=%NGX_SDK%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib
call "%VCVARS%" >nul || (echo set VCVARS to your vcvars64.bat & exit /b 1)
cd /d %~dp0
set FXC=fxc.exe
%FXC% /nologo /T cs_5_0 /E main /O3 /Fh prep_cs.h /Vn g_prep_cs prep_cs.hlsl || exit /b 1
cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /LD /I . /I "%RESHADE_SDK%\include" /I "%NGX_SDK%\include" tf2_dlss_sr.cpp /Fe:tf2-dlss-sr.addon64 /link "%NGX_LIB%" user32.lib advapi32.lib version.lib
if errorlevel 1 exit /b 1
echo BUILD_OK
