# SDK / libc rename candidates

Output: `sdk_libc_candidates.csv` (address,current_name,proposed_name,confidence,evidence).
`current_name` is the Ghidra-style name (FUN_xxxxxxxx / entry_xxxxxxxx). Names starting with `_` are
SDK-internal; the Ghidra export script also resolves optional leading underscores.

## Method
No game ELF is in the repo (only generated code under ps2xRuntime/src/runner), so everything came from the
recompiled output:
1. **Instruction words**: the `// 0xADDR: 0xWORD` comments in every runner .cpp were harvested with ripgrep
   into an address->word map (about 1.1M unique words).
2. **SCE symbol DB** (`ps2xAnalyzer/include/ps2recomp/sce_symbol_database_data.h`, hashes of Sony libs for
   SDK 1.4 to 3.1): re-implemented the `sce_symbol_scanner.cpp` trie walk + relocation-masked SHA-1 in Python
   and ran it at every code address 0x100000-0x700000. About 150 non-stub functions matched, with no
   contradictions. The game's libs are SDK 3.0.x era (libkernl, libmc, libmpeg, libpad, libc).
3. **Existing stub bindings**: ~190 runner files are one-line `ps2_stubs::X` / `ps2_syscalls::X` bodies
   (sceCd*, sceDma*, sceGs*, sceMc*, sceSif*, scePad*, mem*/str*...). Those names were validated by comparing
   the function size (gap to the next function start) with the DB symbol size. Where the stub name differs
   from the DB (e.g. `sceSifLoadModule` bound to a 552B function), the DB size shows it is the internal
   `_sceSifLoadModule`, so the CSV proposes the underscore name.
4. **Syscall wrappers** (`addiu $v1,$zero,N; syscall`): 63 are already named in the export (AddIntcHandler..
   InitTLB, `ps2__*`, RFU*). Only unnamed ones were added (0x5A x5, 0x80, 0x83) plus the RFU086 typo fix.
5. **Manual**: crt0 `_start` (0x100008) -> `_InitSys` (0x111798), FlushCache (0x1076A0), `main` (0x3CD050),
   exit (0x17DE38). DIntr/EIntr/scePrintf by DB relocation offsets. sprintf/vfprintf/_vfprintf_r/__sinit by shape.

## Leads / library map (address ranges)
- 0x100000 crt0, 0x100200-0x102600 libcdvd then libdma, 0x105000-0x106f00 libgraph/libipu, 0x107000-0x114000
  libkernl (syscall wrappers 0x107000-0x107900, deci2/tty 0x108700, sif cmd/rpc 0x10a200-0x10bd00, fs/io
  0x10bb00-0x10f000, sif heap/load 0x10f600-0x110c00, TLB/alarm/timer 0x110c00-0x113600), libmc 0x113600-0x114f00,
  libmpeg 0x115a00-0x11f400, libmtap 0x11f4xx, libpad 0x11f900-0x120f00, libpkt 0x121000, libscf 0x121700.
- libc/libm: memchr..memset 0x17f880-0x17fbe0, strcat/strchr/strcmp/strcpy/strlen/strncat/strncmp/strncpy
  0x182370-0x182e08, vfprintf family 0x181f78-0x1849f0, __sinit 0x17E1C8. `_impure_ptr` is at 0x55F0C0
  (`lui 0x56; lw -0xF40`).
- Code at 0x571d10-0x5c1760 and 0x5ca400-0x5f8d80 is data decoded as code (hundreds of fake `syscall`s);
  ignore the entry_00571d10 / entry_005ca400 files.
- Game-side hot functions NOT identified (not SDK): 0x1873B0 (3436 callers, switch on log level + sprintf),
  0x3C9320/0x3C92F0/0x3C9410 (stack push/pop helpers on a global at gp-0x7FE4), 0x3C7DF0 (copy up to 8 chars).
- Large unidentified libc candidates: libc DB also has strtol/strtod/atoi/malloc/free/printf (not matched
  because libc was built differently or functions are split); look near 0x17c000-0x190000.
- 0x10a108 scePrintf vs kprintf, 0x106958 sceGsPutIMR vs isceGsPutIMR, 0x11b098/0x11b0c0 (Stop/RestartDMA),
  0x121760/0x1217c8 (to/frombcd): identical code, assignment within a pair is a guess.
- 5 `RFU116_SetSyscall` and 3 `RFU091` wrappers are existing Ghidra-plugin names; only RFU091->GetThreadTLS
  (low) and RFU086 typo are proposed.

## Confidence
- high: DB hash match >= 40B, stub binding with matching DB size, crt0 structure, DIntr/scePrintf.
- medium: small (<40B) DB hash matches, identical-code aliases, shape-based libc/printf family, size-mismatched stubs.
- low: guesses on syscall number naming (0x80, 0x5B), `_printf`, bcd pair ordering.
