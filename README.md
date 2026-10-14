# The Grim Adventures of Billy & Mandy — PS2 Static Recompilation

A static recompilation of the North American PlayStation 2 release of *The Grim Adventures of Billy & Mandy*
(**SLUS_214.51**) into native C++ for Windows, built on [PS2Recomp](https://github.com/ran-j/PS2Recomp).

Every function in the game executable has been identified and given a readable name (≈18,400 functions),
so the generated code reads like source rather than `sub_0012ABCD`.

**Status:** the game boots through the loading screens to the title screen and the main menu, and responds
to the controller. It is not yet playable at full speed (see [Current state](#current-state)).

> **No game data is included.** This repository holds only the recompilation *inputs* (function map,
> recompiler configuration, names, runtime overrides and scripts). You need your own copy of the game;
> the recompiler turns its executable into C++ on your machine.

## What you need

- Windows 10/11 x64, 16 GB RAM or more, about 15 GB free disk space
- [Visual Studio 2026 Community](https://visualstudio.microsoft.com/) with the **Desktop development with C++**
  workload (it includes the CMake and Ninja used here). If Visual Studio is installed somewhere other than
  `C:\Program Files\Microsoft Visual Studio\18\Community`, edit `VSROOT` in `build_runner.bat`.
- [Git for Windows](https://git-scm.com/download/win) (the scripts run in Git Bash)
- [Python 3](https://www.python.org/downloads/)
- A disc image of the game, SLUS_214.51 (NTSC-U), and a tool that can extract it, such as [7-Zip](https://www.7-zip.org/)

## Building it yourself

### 1. Clone with the PS2Recomp submodule

```bash
git clone --recursive https://github.com/JasonAdamHD/billy-and-mandy-decomp.git
cd billy-and-mandy-decomp
```

If you already cloned without `--recursive`, run `git submodule update --init --recursive`.

### 2. Extract the game files into the repository folder

Extract the whole disc into the repository root, next to `config.toml`. With 7-Zip:

```bash
7z x "Billy and Mandy (USA).iso" -o.
```

You should end up with these files in the root folder:

| File | SHA-256 |
| --- | --- |
| `SLUS_214.51` | `0b7542e52b4bb017109f569d3a6369945a95f71a4f21d58f9d27ccc278962ce7` |
| `IRXBUNDL.IIB` | `57b352c6266e0e77260d7eba3921b8c4a516619d95bdb9ffa9f9ebb70bca9cc7` |
| `IOPRP300.IMG` | `02d314464c17c715d7e809297b0aba184173a98616b1fafeac27fe33fc480c82` |
| `APP.INI` | `73ef50dfc0e2191e760c5ad219d162da1cedc63a167cd220a0d56cc72e9d0ff6` |
| `SYSTEM.CNF` | `8c1ed7eb7b8cd423c1a5199e2ddcf0a2afeebe111f14dfb1bd410334ac14dd4d` |

plus the `DATAPSX2/` folder (233 files, about 564 MB). Check the executable with:

```bash
sha256sum SLUS_214.51
```

If the hash differs you have a different release or revision of the game, and the function map in this
repository will not match it.

The `.gitignore` is an allowlist, so the game files you extract here are never committed.

### 3. Recompile and build

From Git Bash in the repository root:

```bash
./recomp_and_test.sh 0
```

This builds the recompiler, translates `SLUS_214.51` into C++ under `output/` (about 7 minutes), copies the
generated files into the runtime, and builds `ps2EntryRunner.exe`. The first build compiles about
110,000 files and takes 20–40 minutes; later builds only recompile what changed.

### 4. Check you got the same output

```bash
python tools/hash_output.py output
```

You should see exactly:

```
files:  109805
sha256: a01d39a8e63168f89c22dfa60c746de5eed0c6ecdbfc62d59ba3d03d736c72fe
```

The recompiler is deterministic, so the same executable, `map.csv`, `config.toml` and PS2Recomp commit always
produce byte-identical code.

### 5. Build the fast version (optional, recommended for playing)

Step 3 produces a debug build that logs every function call to `ps2_log.txt` (several GB per run) and runs
slowly. For playing, build an optimised copy without that log:

```bash
cmd //c build_release.bat
```

It compiles into `PS2Recomp/out/build/x64-Fast` (about 20 minutes) and leaves the debug build untouched.
The fast build draws with [parallel-gs](https://github.com/Arntzen-Software/parallel-gs), a Vulkan PS2 graphics
renderer that CMake downloads on the first configure; it needs a Vulkan-capable GPU and driver (no SDK).

### 6. Run

Double-click `Play.bat`. It runs the fast build if you made one, otherwise the debug build, from the
repository root so the game finds its files.

## Controls

A connected gamepad is used when available. Keyboard mapping:

| PS2 | Keyboard |
| --- | --- |
| ✕ | X or Space |
| ○ | C or Escape |
| □ | Z or Numpad 0 |
| △ | V or Numpad 1 |
| D-pad | Arrow keys or WASD |
| L1 / R1 | Q / E |
| L2 / R2 | Left Shift / Right Shift |
| Start | Enter |
| Select | Tab |

F1 shows or hides the runtime debugger.

## Options

Environment variables read by the runtime:

| Variable | Effect |
| --- | --- |
| `PS2X_HIDE_DEBUG=1` | Start with the debugger overlay hidden |
| `PS2X_PLAY_FMV=1` | Try to play the intro movies instead of skipping them (they cannot be decoded yet) |
| `PS2X_AUTOPRESS="cross@600,start@800"` | Press buttons at the given number of seconds after launch, for automated testing |
| `PS2X_DUMP_AT=<hex pc>` with `PS2X_DUMP_RANGE=<hex addr>:<hex size>` | Print guest memory each time a function is called |
| `PS2X_LOG_PRESENT=1` | Log the display registers and the framebuffer shown for each presented frame |
| `PS2X_GS_CPU=1` | Use the CPU graphics renderer instead of the Vulkan (parallel-gs) renderer |
| `PS2X_GS_INTEROP=1` | Show Vulkan frames through a shared OpenGL texture instead of a CPU copy (currently shows black) |
| `PS2X_GS_THREAD=1` | Run the CPU renderer on its own thread |

## Current state

- Boots to the title screen and main menu (Tutorial, Story Mode, VS Mode, Mission Mode, Options, Extras).
- **Slow:** with the fast build and the Vulkan renderer, menus run at about 7-10 frames per second and the
  loading animation up to about 30. VU1 and the IOP (the PS2's I/O processor) are still interpreted.
- **Flash menu crash:** after some time in the menus, the Flash UI player calls through a corrupted object
  (indirect call at 0x50e76c) and the game stops. Under investigation.
- **CPU renderer display workaround:** with `PS2X_GS_CPU=1` the game's finished frame never reaches the
  framebuffer it asks the hardware to show, so that renderer shows its draw buffer instead.
- **Movies are skipped:** the game's FMVs are CRI Sofdec streams decoded by the PS2's IPU (MPEG hardware),
  which the runtime does not emulate yet. `overrides/billy_mandy_overrides.cpp` treats each movie as already
  skipped.

## Repository layout

| Path | Contents |
| --- | --- |
| `config.toml` | Recompiler configuration (stub list, function overrides) |
| `map.csv` | Function map exported from Ghidra: name, start, end and size of every function and code label |
| `names/` | Naming waves, rules and notes behind `map.csv` (`names/HANDOFF.md` is the best starting point) |
| `overrides/` | Game-specific runtime overrides, compiled into the runner |
| `tools/ghidra/` | Ghidra scripts used for naming, function recovery and exporting `map.csv` |
| `tools/hash_output.py` | Fingerprints the generated code (step 4) |
| `build_runner.bat` | Ninja debug build of the runner or the recompiler |
| `build_release.bat` | Optimised build without the per-call trace log, for playing |
| `Play.bat` | Launches the game (fast build if present) |
| `recomp_and_test.sh` | Recompile, sync the generated code, build and optionally run for N seconds |
| `run_and_capture.ps1` | Run the game for N seconds and save window screenshots to `shots/` |
| `PS2Recomp/` | Submodule: the recompiler, runtime and IOP emulator, with the fixes this game needs |

## Changing names or functions

Names come from a Ghidra project of `SLUS_214.51`. After renaming or creating functions, run
`tools/ghidra/ExportPS2FunctionsArgs.java` from Ghidra's script manager with the arguments
`<scratch .toml path> <scratch map.csv path>` (paths without spaces). Copy only the exported `map.csv` here;
keep this repository's `config.toml`, since the exported TOML regenerates defaults this project removed.
Then run `./recomp_and_test.sh 0`. Only the generated files that changed are rebuilt.

## Credits

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors: the recompiler, runtime and IOP
  emulator this project is built on.
- *The Grim Adventures of Billy & Mandy* is © Cartoon Network. This project is not affiliated with or
  endorsed by Cartoon Network, Midway or Sony.
