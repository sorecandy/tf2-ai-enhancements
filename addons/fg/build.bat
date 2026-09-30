@echo off
rem Needs: Visual Studio 2022 Build Tools (x64). Optional env: VCVARS (path to vcvars64.bat).
if not defined VCVARS set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
rem Build ..\..\fsr\lib\build_lib.bat first.
if not defined RESHADE_SDK (echo set RESHADE_SDK & exit /b 1)
call "%VCVARS%" >nul || (echo set VCVARS to your vcvars64.bat & exit /b 1)
cd /d %~dp0
set FXC=fxc.exe
%FXC% /nologo /T cs_5_0 /E blend_main /O3 /Fh blend_cs.h /Vn g_blend_cs blend_cs.hlsl || exit /b 1
%FXC% /nologo /T cs_5_0 /E diff_main /O3 /Fh diff_cs.h /Vn g_diff_cs blend_cs.hlsl || exit /b 1
%FXC% /nologo /T cs_5_0 /E select_main /O3 /Fh select_cs.h /Vn g_select_cs blend_cs.hlsl || exit /b 1
cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /LD /DNDEBUG /I "%RESHADE_SDK%\include" /I ..\..\fsr\lib tf2_fg.cpp /Fe:tf2-fg.addon64 /link user32.lib shell32.lib ole32.lib version.lib ..\..\fsr\lib\build\tf2fg_fsr.lib
if errorlevel 1 exit /b 1
echo BUILD_OK
