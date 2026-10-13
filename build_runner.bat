@echo off
rem Builds ps2EntryRunner (or the recompiler) with Ninja (Debug) in PS2Recomp\out\build\x64-Debug.
rem Faster than the Visual Studio solution: Ninja keeps every core busy, /Z7 avoids the shared-PDB
rem bottleneck, and bigger unity batches mean fewer compiler invocations.
rem Usage: build_runner.bat [jobs] [target]   (jobs default 20, each can use ~1-2 GB RAM;
rem        target default ps2EntryRunner, use ps2_recomp to build the recompiler)

setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%PS2Recomp"
set "BUILD=%ROOT%\out\build\x64-Debug"
set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "JOBS=%~1"
if "%JOBS%"=="" set "JOBS=20"
set "TARGET=%~2"
if "%TARGET%"=="" set "TARGET=ps2EntryRunner"

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

"%CMAKE%" -S "%ROOT%" -B "%BUILD%" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Debug ^
  -DCMAKE_C_FLAGS_DEBUG="/Z7 /Ob0 /Od /RTC1" ^
  -DCMAKE_CXX_FLAGS_DEBUG="/Z7 /Ob0 /Od /RTC1" ^
  -DPS2X_RUNNER_UNITY_BUILD_BATCH_SIZE=64 ^
  -DPS2X_GAME_OVERRIDES_DIR="%HERE%overrides" ^
  -DCMAKE_EXE_LINKER_FLAGS_DEBUG="/debug:fastlink /INCREMENTAL" || exit /b 1

"%CMAKE%" --build "%BUILD%" --target %TARGET% -- -j %JOBS% || exit /b 1

rem The runner needs the ffmpeg/SDL DLLs staged next to it, so run it in place from the game folder:
rem   cd /d "%HERE%" ^&^& "%BUILD%\ps2xRuntime\ps2EntryRunner.exe" SLUS_214.51
if /I "%TARGET%"=="ps2EntryRunner" (echo Built %BUILD%\ps2xRuntime\ps2EntryRunner.exe) else (echo Built %TARGET%)
