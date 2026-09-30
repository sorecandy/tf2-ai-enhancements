@echo off
rem Needs: Visual Studio 2022 Build Tools (x64). Optional env: VCVARS (path to vcvars64.bat).
if not defined VCVARS set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
call "%VCVARS%" >nul || (echo set VCVARS to your vcvars64.bat & exit /b 1)
cd /d %~dp0
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /LD /DUNICODE /D_UNICODE tf2_ai_loader.cpp /Fe:TF2AIEnhancements.dll /link user32.lib
if errorlevel 1 exit /b 1
echo BUILD_OK
