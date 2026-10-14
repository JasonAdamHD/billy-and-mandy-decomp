@echo off
rem Optimised build of ps2EntryRunner without the per-function trace log (ps2_log.txt), for playing.
rem Lives in its own folder (PS2Recomp\out\build\x64-Fast) so the debug build stays untouched.
rem Uses the RelWithDebInfo configuration so PS2Recomp's Release-only whole-program optimisation (/GL + /LTCG)
rem stays off: on ~110k generated functions its link-time code generation runs for hours on one core.
rem Identical-code folding (/OPT:ICF) is off for the same reason.
rem Run recomp_and_test.sh first so the generated code is in place.
rem Graphics use the parallel-gs Vulkan renderer (fetched from GitHub on first configure); set PS2X_GS_CPU=1 at run time for the CPU renderer.
rem Usage: build_release.bat [jobs]   (default 16; optimised compiles use more RAM per job)

setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%PS2Recomp"
set "BUILD=%ROOT%\out\build\x64-Fast"
set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "JOBS=%~1"
if "%JOBS%"=="" set "JOBS=16"

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

"%CMAKE%" -S "%ROOT%" -B "%BUILD%" -G Ninja ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_C_FLAGS_RELWITHDEBINFO="/O2 /Ob3 /GS- /DNDEBUG" ^
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="/O2 /Ob3 /GS- /DNDEBUG" ^
  -DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO="/INCREMENTAL:NO /OPT:REF /OPT:NOICF" ^
  -DPS2X_ENABLE_RUNTIME_LOGS=OFF ^
  -DPS2X_GS_BACKEND=PARALLEL_GS ^
  -DPS2X_RUNNER_UNITY_BUILD_BATCH_SIZE=64 ^
  -DPS2X_GAME_OVERRIDES_DIR="%HERE%overrides" || exit /b 1

"%CMAKE%" --build "%BUILD%" --target ps2EntryRunner -- -j %JOBS% || exit /b 1
echo Built %BUILD%\ps2xRuntime\ps2EntryRunner.exe
