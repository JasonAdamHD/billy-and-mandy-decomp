@echo off
rem Launches the recompiled game. Runs from this folder so it finds SLUS_214.51 and DATAPSX2;
rem the exe stays in its build folder next to the DLLs it needs. F1 shows the debugger.
rem Prefers the optimised build (build_release.bat), falling back to the debug build.
cd /d "%~dp0"
set "PS2X_HIDE_DEBUG=1"
set "EXE=%~dp0PS2Recomp\out\build\x64-Fast\ps2xRuntime\ps2EntryRunner.exe"
if not exist "%EXE%" set "EXE=%~dp0PS2Recomp\out\build\x64-Release\ps2xRuntime\ps2EntryRunner.exe"
if not exist "%EXE%" set "EXE=%~dp0PS2Recomp\out\build\x64-Debug\ps2xRuntime\ps2EntryRunner.exe"
start "" "%EXE%" SLUS_214.51
