@echo off
rem build_lib.bat - static /MT Release libraries: patched FFX DX11 backend + FI 1.1 + OF 1.1.1 + tf2fg wrapper.
rem   lib\build\tf2fg_fsr.lib                   PRODUCTION flavour (the add-on links this one): no test counters,
rem                                             no fault injection, no hooks.
rem   lib\build_testing\tf2fg_fsr_testing.lib   TEST flavour (/DTF2FG_TESTING): counters, fault injection, NULL-binding
rem                                             hook, OF on/off switch; linked only by test\tf2fg_test.exe.
rem Run from anywhere:
rem   fsr\lib\build_lib.bat
rem Optional (experiments only): %1 = extra cl defines (quoted), %2 = output directory; then only the production
rem flavour is built, into %2.
rem
rem /EHs (NOT /EHsc): the FFX API is declared extern "C" but the backend throws on D3D11 failures (TIF). /EHc would
rem let the compiler assume extern "C" calls never throw and drop the try/catch around them in tf2fg_fsr.cpp.
setlocal
set ROOT=%~dp0..
set SDK=%ROOT%\sdk_patched
set EXTRA=%~1

if not defined VCVARS set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

if not "%~2"=="" (
  call :flavour "%~2" tf2fg_fsr "%EXTRA%"
  if errorlevel 1 exit /b 1
  goto :done
)
call :flavour "%~dp0build" tf2fg_fsr "%EXTRA%"
if errorlevel 1 exit /b 1
call :flavour "%~dp0build_testing" tf2fg_fsr_testing "/DTF2FG_TESTING %EXTRA%"
if errorlevel 1 exit /b 1
:done
echo BUILD_LIB_OK
endlocal
exit /b 0

rem ---- :flavour <outdir> <libname> <extra defines> ----
:flavour
set OUT=%~1
set NAME=%~2
set FDEFS=%~3
set OBJ=%OUT%\obj
if not exist "%OBJ%" mkdir "%OBJ%"
del /q "%OBJ%\*.obj" 2>nul

set DEFS=/DNDEBUG /DTF2FG_FFX_STATIC /DDISABLE_FFX_DEBUG_BREAK /DNOMINMAX /D_UNICODE /DUNICODE /D_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
set INCS=/I"%SDK%\include" /I"%SDK%\src" /I"%SDK%\src\shared" /I"%SDK%\src\components" /I"%SDK%\src\backends\shared" /I"%ROOT%\shaders\generated" /I"%~dp0."
set CFLAGS=/nologo /O2 /MT /W3 /GS /Gy /Zi /c %DEFS% %FDEFS% %INCS% /Fo"%OBJ%\\" /Fd"%OUT%\%NAME%.pdb"

cl %CFLAGS% /std:c++17 /EHs ^
  "%SDK%\src\backends\dx11\ffx_dx11.cpp" ^
  "%SDK%\src\components\frameinterpolation\ffx_frameinterpolation.cpp" ^
  "%SDK%\src\components\opticalflow\ffx_opticalflow.cpp" ^
  "%SDK%\src\backends\shared\ffx_shader_blobs.cpp" ^
  "%SDK%\src\backends\shared\blob_accessors\ffx_frameinterpolation_shaderblobs.cpp" ^
  "%SDK%\src\backends\shared\blob_accessors\ffx_opticalflow_shaderblobs.cpp" ^
  "%SDK%\src\shared\ffx_assert.cpp" ^
  "%SDK%\src\shared\ffx_object_management.cpp" ^
  "%~dp0tf2fg_fsr.cpp"
if errorlevel 1 exit /b 1

cl %CFLAGS% /TC "%SDK%\src\backends\dx11\DXBCChecksum.c" "%SDK%\src\backends\dx11\md5.c"
if errorlevel 1 exit /b 1

lib /nologo /OUT:"%OUT%\%NAME%.lib" "%OBJ%\*.obj"
if errorlevel 1 exit /b 1
echo BUILT "%OUT%\%NAME%.lib"
exit /b 0
