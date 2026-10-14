@echo off
rem Optimised build for the native-renderer branch, in its own folder (PS2Recomp\out\build\x64-Native) so it
rem never touches the x64-Fast build. Same settings as build_release.bat.
rem Dependencies are expected in out\build\x64-Native\_deps\<name>-src (copy them from another build folder to
rem skip the network); FETCHCONTENT_FULLY_DISCONNECTED then uses them as they are.
rem Usage: build_native.bat [jobs]   (default 16)

setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%PS2Recomp"
set "BUILD=%ROOT%\out\build\x64-Native"
set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "JOBS=%~1"
if "%JOBS%"=="" set "JOBS=16"
set "DISCONNECTED=OFF"
if exist "%BUILD%\_deps\parallel_gs-src\CMakeLists.txt" set "DISCONNECTED=ON"

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

"%CMAKE%" -S "%ROOT%" -B "%BUILD%" -G Ninja ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_C_FLAGS_RELWITHDEBINFO="/O2 /Ob3 /GS- /DNDEBUG" ^
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="/O2 /Ob3 /GS- /DNDEBUG" ^
  -DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO="/INCREMENTAL:NO /OPT:REF /OPT:NOICF /MAP" ^
  -DFETCHCONTENT_FULLY_DISCONNECTED=%DISCONNECTED% ^
  -DPS2X_ENABLE_RUNTIME_LOGS=OFF ^
  -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF ^
  -DPS2X_GS_BACKEND=PARALLEL_GS ^
  -DPS2X_RUNNER_UNITY_BUILD_BATCH_SIZE=64 ^
  -DPS2X_GAME_OVERRIDES_DIR="%HERE%overrides" || exit /b 1

"%CMAKE%" --build "%BUILD%" --target ps2EntryRunner -- -j %JOBS% || exit /b 1
echo Built %BUILD%\ps2xRuntime\ps2EntryRunner.exe
