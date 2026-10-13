# Handoff: Naming functions in the Billy & Mandy PS2 recomp

## Goal
Give the stripped game's functions (and key globals) meaningful names. Naming happens in **Ghidra**, not in the generated C++.

## How names flow (important)
- `ps2xRuntime/src/runner/` holds ~110,000 **generated** `.cpp` files (`FUN_XXXXXXXX_0x....cpp`, `entry_....cpp`). Every recompile overwrites them, so **never hand-edit them**. **Never list or enumerate that folder.** Use targeted `grep` or open files by address.
- `ps2xRuntime/src/runner/register_functions.cpp` is about 660k lines. Grep it only, never read it whole.
- Pipeline: rename in Ghidra → export with `ps2xRecomp/tools/ghidra/ExportPS2Functions.java` (CSV `Name,Start,End,Size` + TOML) → re-run `ps2xRecomp` → the C++ picks up the names.
- The export script auto-stubs functions whose names match runtime handlers (`PS2_SYSCALL_LIST` / `PS2_STUB_LIST`, see `isLibraryFunctionName` / `resolveRuntimeHandlerName`). Use the exact runtime handler names for SDK functions.
- The generated C++ is a register-level translation (`GPR_U32(ctx, N)`, `WRITE32`). There are no local variables to rename. What's worth naming is functions, global data addresses and structs.

## Environment
- Ghidra 12.1.4: `C:\Users\jason\Desktop\ghidra_12.1.4_PUBLIC`
- Game ELF: `C:\Users\jason\Desktop\Billy and Mandy Recomp\ELF\SLUS_214.51` (open in Ghidra as `SLUS_214.51`, r5900 LE, 16,044 functions)
- **Ghidra MCP** (bethington/ghidra-mcp **v7.0.0-rc.2**, built for 12.1.4):
  - Plugin enabled in Ghidra's project window. HTTP server on `http://127.0.0.1:8089`. Verified with `/check_connection` → `{"status":"ok","program":"SLUS_214.51"}`.
  - Bridge venv: `C:\Users\jason\Tools\ghidra-mcp\venv` (`bridge-mcp-ghidra.exe --transport stdio`). Release files are in `C:\Users\jason\Tools\ghidra-mcp\release-7.0.0-rc.2`.
  - Registered as MCP server `ghidra` in `C:\Users\jason\.claude.json` (backup: `.claude.json.bak-ghidra-mcp`).
  - Tools load lazily (default groups `listing,function,program`). Use `search_tools` / `load_tool_group` for others, such as xref and bulk rename.
  - Ghidra must be open with `SLUS_214.51` loaded for the tools to work.
- System `java` is 1.8. A JDK 21 is installed separately, and Ghidra uses its own JDK setting.

## Done so far
- **SDK/libc identification** → `names/sdk_libc_candidates.csv` (`address,current_name,proposed_name,confidence,evidence`): **352 rows, 274 high / 70 medium / 8 low**. Method and leads are in `names/NOTES.md`.
  - Method: the repo's SCE symbol DB (`ps2xAnalyzer/include/ps2recomp/sce_symbol_database_data.h`, SDK 1.4–3.1) matched by relocation-masked SHA-1 over instruction words. The game uses roughly **SDK 3.0.x** libraries.
  - About 11 existing stub bindings were wrong (e.g. `sceSifLoadModule` → `_sceSifLoadModule`). These are flagged in the CSV.
- **2026-10-12: all 274 `high` rows applied in Ghidra and saved.** That's 259 renames plus 15 functions that were missing in Ghidra and were created at their CSV addresses (their sizes match the DB). The duplicate `setD4_CHCR` at 0x106d28 was named `setD4_CHCR_2`, and `RFU086_WaitEvnetFlag` → `WaitEventFlag`. MCP inline scripts are enabled via the user env var `GHIDRA_MCP_ALLOW_SCRIPTS=1`.
- **2026-10-12: all 70 `medium` rows applied and saved** (69 renames, plus `_set_sreg` created at 0x10a208, 28B). The duplicate `QueryBootMode` copies are suffixed `_2`–`_5`. The CSV's "size gap mismatch" was measured against the recomp's function list. Ghidra's function bodies match the DB sizes, except `sceMcSeek` (224 vs 212) and `sceMpegAddStrCallback` (300 vs 252). The 8 `low` rows were skipped.
- **2026-10-12 data-as-code fixed in Ghidra.** The old `map.csv` had two giant `entry_` spans. The new export is not yet installed: renames would newly stub 12 functions by name (pending a user decision).
- **Full-naming pipeline** (the `/loop` task: name everything). The scripts live in `C:\Users\jason\ghidra_scripts\`:
  - `QualStrNames.java` names functions from `Class::Method` diagnostic strings (79 done).
  - `ExportBatches.java outDir startHex batchSize maxBatches firstNo [ready]` dumps unnamed functions with decompiled C, callers/callees, source-file attribution (from `*.cpp` string anchors, in link order) and vtable slots.
  - Subagents (Haiku/Sonnet) follow `NAMING_RULES.md` and write `0xADDR,Class::Method,h|m|l` CSVs.
  - `ApplyNameCsv.java dry|apply csvPath [force]` applies them, creating class namespaces and touching only unnamed functions.
  - The export must flatten `Class::Method` for C++ identifiers; `getName()` drops the namespace, so it needs a fix before the next recompile.
- **2026-10-12: every function in Ghidra is named (16,107/16,107).** The first pass ran in 12 bottom-up Haiku waves. A Sonnet review pass followed over the 1,942 low-confidence or placeholder names. All applied CSVs are in `names/applied/`, and the rules are in `names/NAMING_RULES.md` and `REVIEW_RULES.md`.
  - The naming scripts in `~/ghidra_scripts/` are `ExportBatches.java` (modes `ready` / `rank` / `list`), `ApplyNameCsv.java [force]`, `NamingStats.java`, `ExportGlobals.java` and `ApplyGlobalCsv.java`.
  - Globals (`DAT_*`, 9,365 referenced from code) are being named with `g_`, `k_` and `vtbl_` prefixes. The recompiled C++ uses raw addresses for data, so this only helps when reading in Ghidra.
  - Globals: all 9,365 code-referenced `DAT_*` labels are named (`names/applied/globals*.csv`; about 60% are low confidence, mostly dispatch-table slots).
  - The ready-to-install export is `names/map_named.csv`: 0 `FUN_`; switch labels are now qualified as `Parent_caseD_N`.
  - Still open: install the new `map.csv` (export flattening `Class::Method` → `Class_Method` is fixed in `ExportPS2Functions.java`). The 12-new-name-stubs question is still undecided. After that, recompile.
- **2026-10-12 boot work.**
  - Build with `..\build_runner.bat` (Ninja, about 20 min to compile plus about 30 min to link).
  - Run it from the game folder: `PS2Recomp\out\build\x64-Debug\ps2xRuntime\ps2EntryRunner.exe SLUS_214.51`. It needs the DLLs in its build folder.
  - Blocker 1, the heap:
    - `App::vInitSoftFloatTuningConstants` sizes the main heap at about 25.1 MB (0x1915B32).
    - The runtime capped `EndOfHeap` at 0x1F00000, so the allocation in `AtApPsx2BasePackage::vInitialize` failed. That assert led to the null-pointer controller call in `GmCoControllerManager::nUpdateControllersAndAdvanceFx`.
    - Fix (runtime): the heap now extends to the main-stack bottom (0x1FE0000), and the stack sits at the top of RAM.
    - RPC pools moved to 0xA0000/0xB0000, and async callback stacks to 0xC0000–0x100000 (`System.cpp`, `ps2_runtime.cpp`, `State.h`).
- Key anchors:
  - `_start` 0x100008 → `_InitSys` 0x111798 → FlushCache 0x1076A0 → **`main` 0x3CD050** → `exit` 0x17DE38
  - libc block around 0x17c000–0x190000 (memcpy 0x17fa28, memset 0x17fbe0, strlen 0x182890, strcmp 0x182630, …)
  - Kernel syscall wrappers at 0x107000–0x111c68 (mostly already named)

## Next steps (in order)
1. **Verify MCP tools** are visible in the session (`check_connection`).
2. **Apply the `high` rows** from `names/sdk_libc_candidates.csv` to Ghidra with a bulk rename. Review `medium` rows before applying them. Skip `low` rows.
   - Ambiguous identical-code pairs (pick one or mark as unsure): `_fs_version`/`_lf_version`, mpeg Stop/Restart DMA, `converttobcd`/`convertfrombcd`, `scePrintf`/`kprintf`.
   - `_printf` 0x109208 (low) is 3.3 KB, vs the DB's 1.5 KB. Don't trust that match.
3. **Fix data decoded as code**: 0x571d10–0x5c1760 and 0x5ca400–0x5f8d80. These produce about 490 fake functions and `syscall` sites. Clear them and mark them as data in Ghidra; this also shrinks the recompiled output.
4. **Use ELF strings** (now available) to confirm library versions (sce* tags) and to name functions that reference debug, assert or file-path strings.
5. **Unmatched libc** (`malloc`, `free`, `strtol`, `atoi`, `printf` …): look around 0x17c000–0x190000 and fuzzy-match against neighbours.
6. **Game code**: start at `main` 0x3CD050 and the most-called functions **0x1873B0** (3,436 callers) and **0x3C9320**. Name the globals they touch.
7. Re-export from Ghidra and recompile. Optionally keep `names/` as the running record of identified names.

## User preferences
- Concise responses.
- Watch token cost. When it gives the same result, delegate bulk work to subagents (e.g. Sonnet at medium effort).
- Ask before downloads or config changes.
