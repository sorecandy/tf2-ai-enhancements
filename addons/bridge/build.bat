@echo off
rem Needs: Visual Studio 2022 Build Tools (x64). Optional env: VCVARS (path to vcvars64.bat).
if not defined VCVARS set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not defined RESHADE_SDK (echo set RESHADE_SDK & exit /b 1)
call "%VCVARS%" >nul || (echo set VCVARS to your vcvars64.bat & exit /b 1)
cd /d %~dp0
cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /LD /I "%RESHADE_SDK%\include" tf2_nr_bridge.cpp /Fe:tf2-nr-bridge.addon64 /link user32.lib
if errorlevel 1 exit /b 1
echo BUILD_OK
