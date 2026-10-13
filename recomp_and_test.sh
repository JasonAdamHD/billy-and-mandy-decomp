#!/bin/bash
# Recompile with the current map.csv, sync only changed generated files into the runner (keeps Ninja
# incremental), rebuild, and run the game for N seconds (default 60) with logs in run_out/run_err/ps2_log.
# Usage: recomp_and_test.sh [seconds] [mapcsv-to-install]   (seconds=0: recompile and build only, no test run)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SECS="${1:-60}"
cd "$HERE"
if [ -n "$2" ]; then cp "$2" map.csv; fi

RECOMP="$HERE/PS2Recomp/out/build/x64-Debug/ps2xRecomp/ps2_recomp.exe"
if [ ! -x "$RECOMP" ]; then
  powershell -NoProfile -Command "& cmd.exe /c '\"$(cygpath -w "$HERE/build_runner.bat")\" 20 ps2_recomp'" > recomp_build.log 2>&1 || { echo "recompiler build failed"; tail -5 recomp_build.log; exit 1; }
fi
rm -rf output && mkdir output
"$RECOMP" config.toml > recomp.log 2>&1 || { echo "recompile failed"; tail -5 recomp.log; exit 1; }
echo "recompile: $(tail -1 recomp.log)  errors=$(grep -c '\[error\]' recomp.log || true)"

python - "$HERE/output" "$HERE/PS2Recomp/ps2xRuntime/src/runner" "$HERE/PS2Recomp/ps2xRuntime/include" <<'EOF'
import os, sys, filecmp, shutil
out, runner, inc = sys.argv[1:4]
headers = {"ps2_recompiled_functions.h", "ps2_recompiled_stubs.h"}
new = set(os.listdir(out)); old = set(os.listdir(runner))
changed = added = removed = 0
for f in new:
    dst = os.path.join(inc if f in headers else runner, f)
    src = os.path.join(out, f)
    if not os.path.exists(dst): added += 1; shutil.copy2(src, dst)
    elif not filecmp.cmp(src, dst, shallow=False): changed += 1; shutil.copy2(src, dst)
for f in old - new:
    if f not in headers: removed += 1; os.remove(os.path.join(runner, f))
print(f"sync: changed={changed} added={added} removed={removed}")
EOF

powershell -NoProfile -Command "& cmd.exe /c '\"$(cygpath -w "$HERE/build_runner.bat")\" 20'" > build.log 2>&1 || { echo "build failed"; grep -E "FAILED|error" build.log | head -5; exit 1; }
echo "build: $(tail -1 build.log)"

if [ "$SECS" = "0" ]; then exit 0; fi
timeout "$SECS" ./PS2Recomp/out/build/x64-Debug/ps2xRuntime/ps2EntryRunner.exe SLUS_214.51 > run_out.log 2> run_err.log || true
echo "ran ${SECS}s"
