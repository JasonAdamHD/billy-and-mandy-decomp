# Native renderer design ("level 2")

Goal: draw the game's 3D directly on the PC GPU instead of emulating VIF1 -> VU1 microcode -> GS,
while the 2D/UI and special effects keep running on the existing emulated path (parallel-gs).

Status: design only. All addresses are EE virtual addresses in `SLUS_214.51` (names as in Ghidra /
`map.csv`). Items marked **(?)** were inferred statically and still need runtime confirmation (M0).

## 1. How the engine renders a frame today

### 1.1 Objects and naming caveats

Several Ghidra class names are misleading:

- `AtGrPsx2Palette` is really the per-frame Psx2 renderer context. It is called **R** below.
  - `R+0x2980` is the matrix provider: world matrix and bone palette.
  - `R+0x2de0` is the GsOpti/VIF packet-builder state.
- `AtGrPsx2_BeginPaletteDmaState` (0x42f550) is "begin scene / set render target".
- `AtGrPsx2_BuildTextureDmaState` (0x431060) is display and draw-buffer setup.

### 1.2 Per-frame flow

1. **Begin scene.** `AtGrPsx2_BeginPaletteDmaState` (0x42f550) does the following:
   - Builds the GS register blocks and the viewport (`vUpdateViewportState` 0x42f2d0, `AtPsx2GsOpti_SetupViewportScale` 0x4a9fd0).
   - When `flags & 3` is set, it binds a texture as the render target instead of the frame buffer (`AtGrPsx2_BindPaletteDescriptor` 0x42f160 -> FRAME/ZBUF/XYOFFSET packet).
2. **Scene walk.** Mesh nodes call `AtGrSubmitMeshDisplayRecords` (0x428b60) and `AtGrPsx2_SubmitMeshDisplayEntries[Ex]` (0x428680/0x428870) through vtables. A mesh holds `count` at +8 and an array of 0x54-byte entries at +0x4c. Each entry is handled one of three ways:
   - **Delayed.** Material flag `0x40000` puts the entry on the delayed list `R+0x3728`.
   - **Transparent.** Material flag `0x600` inserts the entry into a depth-sorted RB-tree (`AtVpInsertDisplayEntry` 0x4c3950; key = depth + material bias at +0x3c) with a snapshot of its matrices and lights.
   - **Immediate.** Otherwise `AtGrPsx2_ApplyMaterialGsState` (0x4266b0) runs, then the entry's pointer-to-member draw function is called through `AtGrDispatchThunk` (0x512290) with `(entry, R, fillMode)`.
3. **Draw functions,** chosen per entry by `AtGrPsx2_SelectMeshDrawFunctions` (0x429f30):
   - Indexed triangles or strips: `vDrawIndexedTriangleOrWire[B]` (0x429c10/0x429c90).
   - Sprites: `vDrawSpriteGsPacket` (0x429b30).
   - Wire subdivision variants (0x4297d0/0x4299a0).
4. **Delayed and sorted lists.**
   - `AtGrPsx2_FlushPaletteSlotList` (0x42fa10) drains the shader-sorted list.
   - `AtGrPsx2_FlushDelayedBatch` (0x4303d0) then calls `vEmitDelayedList` (0x4c3660), which replays the transparent tree back to front, restoring each entry's matrix and light snapshot.
   - Water (`vDrawWaterSurfaces` 0x4c5fc0) is drawn in the delayed phase.
5. **2D,** in submission order after the 3D: Flash UI, fonts, blits, FMV. See section 3.
6. **End frame.** `vFlushFrameDmaState` (0x430b00) does the following:
   - Toggles the display buffer (0x4a40c0).
   - Appends END + FLUSHA (0x4a09c0).
   - Calls `SwapDmaPingPongBuffers` (0x4a0a40).

   The frame is **one VIF1 DMA chain** in a 0x80000-byte ping-pong ring, kicked with CHCR=0x105. Builders append to the half that is not being transferred.

### 1.3 Mesh data

- **On disk.** Records are float vertex arrays (`AtGrPsx2VertexArray` 0x436990) holding:
  - float3 position, float3 normal, float2 UV (one or two sets), RGBA8 colour;
  - u8, u16 or u32 index buffers.

  They are loaded by `AtGrPsx2_LoadMeshRecord` (0x429250) and `AtGrPsx2_InitMeshFromRecord` (0x42a0d0).
- **Static meshes: lazily built VIF chains.** On first draw, `AtGrPsx2_OptimizeMeshGsState` (0x429d10) builds one chain via `AtPsx2VuMeshOptimizer::nBuildFromIndexBuffer` (0x4aefd0). That function batches triangles into **<= 40-vertex / <= 80-triangle** chunks. Each chunk unpacks:
  - s16 quantized positions (scale and offset sent separately);
  - u8 index triplets;
  - s8 normals;
  - 16-bit UVs;
  - RGBA8 colours;

  and ends in `MSCAL`/`MSCNT` + DMA RET. The per-frame packet `CALL`s it (`AtPsx2GsOpti_EmitIndexedTriangleBatch` 0x4a7130).
- **Dynamic geometry: inline float CNT packets.** `BuildIndexedVertexDmaPacket` 0x4a8070 is used by `AtGrPsx2DynamicGeometry`.
- **Skinning runs on VU1** and is rigid/palette style (1-4 matrices per batch, optional per-vertex weights (?)):
  - `vLoadSlotRegisterSet` (0x42dc90) stages up to 5 matrices from the bone-pointer palette at `*R+0x18b60`.
  - `AtPsx2GsOpti_CopyRegSetByIndex` (0x4a7b80) writes them into the packet.
  - No CPU skinning in the draw path.

### 1.4 Per-draw state sent to VU1

The EE caches this state and only re-sends what changed:

- view-projection;
- viewport;
- 8 lights + ambient (`AtPsx2GsOpti_StoreSlotByType` 0x4a9df0, `AtPsx2GsOpti_BuildStatePacket` 0x4a7d00);
- material colour (float, 128 = 1.0);
- UV transform;
- per-material "extended" vec4 block (`vApplyVec4Slots` 0x4bc320);
- GIF tag with PRIM;
- program entry words.

GS register state (TEX0/TEX1/MIPTBP, ALPHA/TEST/CLAMP from `g_Psx2PaletteApplyGsState_Table`) is diffed by `EmitChangedGsRegisterRecords` (0x4ab3b0). **A batch never carries its full state.** A native renderer must track the running GS state, exactly as the GS does.

### 1.5 VU1 microprograms

The microcode is **embedded in the ELF data** at 0x512380 to 0x535200. Each program is a descriptor:

- `name[16]`
- `+0x10` pointer to the VIF packet
- `+0x14` QWC
- `+0x16` entry or load address
- `+0x18` u16[16] entry table

The packet itself is STCYCL/STMASK/STMOD + MPG chunks. `AtPsx2GsOpti_UpdateSlotTag` (0x4ad940) uploads a program by DMA REF only when its slot changes. **The REF source address therefore identifies the program exactly.**

Instruction memory holds three co-resident pieces:

| Instr range | Piece |
|---|---|
| 0..~1330 | main "vertex process" program (slot 0) |
| 1428..~1594 | XFORM skinning subroutine (slot 6) |
| 1596..2045 | lighting subroutine (slot 7) |

| Program(s) | Purpose |
|---|---|
| `IDXTRID`, `IDXSTRPD` | standard lit/textured indexed tris / strips: xform + lighting + trivial reject (ADC), GIF out |
| `IDXTRIMULTI`, `IDXSTRPMUL` | same + second pass (multi-layer material) |
| `IDXTRITOON`, `IDXSTRPTOON` | toon: back-face outline pass expanded along the screen-space normal + shaded pass |
| `IDXTRIHALO`, `IDXSTRPHALO` | toon-like structure, probably rim/halo (?) |
| `IDXSTRPEXTN` | strips with normal extrusion (`XFORMnMEXTN`) |
| `IDXSTRPFAST`, `SCRPOSTCORD` | unlit 1-matrix strips; SCRPOSTCORD generates UVs from screen position |
| `IDXTRIWIRE`, `IDXSTRPWIRE` | wire/untextured |
| `XFORM{1..4}M[N]`, `XFORM1MNOD[N]`, `XFORM{1..4}MEXTN` | skinning with 1-4 bones, with/without normals, dequantize (M) or float (NOD), extrusion |
| `CALCLIGHT`, `CALCHEMLIT` | vertex lighting (directional/point/spot (?)), hemisphere variant |
| `SPRITEM1` | billboard sprites |
| `PARTICLEM1` / `PARTDIST` | particle ring expansion (one packet, entries 4/33) |
| `WATER`, `WATERMUL` | water grid with ESIN waves (heights partly from EE) |
| `CLIPTRIS` | real polygon clipper; no caller found (?) |

**VU1 data memory layout (qw):**

| Range | Contents |
|---|---|
| 0-255 | two GIF output buffers |
| 256-259 | extended material block |
| 260-263 | view-projection |
| 264-265 | viewport offset and scale (depth = K/w) |
| 266-289 | lights |
| 298 | ambient |
| 299 | entry words |
| 300 | material colour |
| 301 | GIF tag |
| 302-317 | bone matrices |
| 318-319 | second-pass tag and colour |
| 320-321 | UV matrix |
| 408-688, 689-969 | double-buffered input batches |

Each input batch contains header, indices, positions, colours, ST, normals and weights.

The programs do **no real clipping**: they only trivially reject and set ADC, and rely on GS scissor. A GPU pipeline needs none of that.

### 1.6 Textures

- **Format.** TIM2 (`AtGrPsx2Tm2Loader` 0x4bf0d0), 4/8/16/24/32 bpp, RGBA8888 CLUTs (CSM1), mipmaps. Some palettes are generated at runtime (HSV).
- **The EE keeps the pixels** (surface+0x38) permanently. 4/8 bpp textures are block-repacked in place by `vPackTextureBlocks` (0x4212c0).
- **VRAM is an LRU cache** (`AtGrPsx2VideoMemoryManager` 0x437710 + `AtGrPsx2TextureCache`). The bind (`AtGrPsx2_SetCurrentPaletteEntry` 0x42da90) uploads lazily into the frame's DMA list and re-uploads after eviction. **A given TBP can hold different textures within one frame.**
- **Identity.** Material+0x50 points to the texture object, which holds:
  - VRAM block +0x30 (TBP at block+0x18);
  - CLUT +0x34;
  - pixel surface +0x48.

  This is a stable key for an engine-level texture hook (HD replacement later).

### 1.7 Display

640x448 interlaced (640x224 field mode possible), double-buffered frame buffers. Colour is PSMCT32/24/16 and Z is Z24/Z16(S) depending on mode (?).

## 2. Interception point

| Option | Where | Pros | Cons |
|---|---|---|---|
| **A. Engine draw call** | `AtGrDispatchThunk` / mesh draw functions | float source data; material + texture objects; easy HD textures | must re-implement matrix/light staging, sorted and delayed replays (draws run twice), GS state tables; misses particles/water/sprites/dynamic geometry; must also suppress the emulated packets |
| **B. Packet builders** | `EmitIndexedTriangleBatch`, `Build*DmaPacket` | state already decoded | many builders; prebuilt chains still need decoding |
| **C. VU1 program level (recommended)** | runtime VIF1 interpreter, at `MSCAL`/`MSCNT` of a known program | every 3D path converges here with all inputs staged in VU memory (bones, lights, colour, GIF tag, entry bits); stream order and the running GS state are known exactly; program identity is exact (DMA REF address / MPG hash); the engine is not touched; suppressing emulation = not running the VU | 40-vertex batches (thousands per frame, so batching on the GPU is needed); positions are s16-quantized (fine); it runs on the graphics worker thread |

**Recommendation: C, plus small engine-level hooks where they add data.** Candidates:

- texture identity / HD replacement (1.6);
- marking scene begin/end and render-target scenes (`BeginPaletteDmaState` flags) for the composite.

C keeps the work in PS2Recomp (VIF1/VU1/GS are already there) and reuses one mechanism for all 3D programs. Every program can also be validated against the interpreter, which stays available as a reference.

### What must be reimplemented in shaders (per batch)

**Vertex shader:**
1. Dequantize: `pos * scale + offset`.
2. Skin: 1-4 bone matrices, optional weights /255.
3. Optional EXTN normal extrusion.
4. `clip = pos * VP`.
5. Vertex lighting: `ambient + sum(max(N.L, 0) * colour * attenuation)`, the CALCLIGHT/HEMLIT formulas taken from the disassembly.
6. Colour = lit * vertex colour * material colour (128 = 1.0).
7. UV affine transform; SCRPOSTCORD screen-space UVs.

**Fragment shader and fixed function:** GS TFX modulate/decal/highlight with TCC, alpha test (TEST), destination-alpha test, alpha blend (ALPHA: (A-B)*C>>7+D, mapped to fixed-function blend where possible and a shader fallback otherwise), Z test/write from TEST/ZBUF, FBA, fog (FGE) if used, CLAMP modes (REGION_REPEAT needs a shader).

**Multi-pass programs** become multiple draws per batch:
- TOON/HALO outline: front-face culled, extruded in screen space by the width in qw 256-258.
- MULTI second layer.

**Sprites and particles:** expand quads in a vertex shader (instanced) from the per-particle records.

**Water:** port the ESIN wave maths, or keep it emulated initially.

### Textures for native draws

Keep a **CPU shadow of GS VRAM**, updated from host->local GIF image transfers (the runtime already sees and swizzles them with `GSMem::Write*`), plus dirty tracking.

At draw time, key the cache on `(TEX0 fields, CLUT, content hash of the dirty-tracked pages)` and decode to RGBA8 `VkImage`s. Textures that are render targets (render-to-texture scenes) are not in the shadow. Draws that sample them stay emulated until M6.

## 3. What stays emulated, and how the hybrid composes

**Emulated initially:**
- Flash UI (`AtFp*`; `AtFpDrawTexturedQuadBatch` builds normal 2-triangle draws through the factory);
- fonts (`vDrawGlyphQuads`, direct GIF);
- 2D blits and quads (`AtGrPsx2_DrawQuadPacket` etc.);
- FMV;
- the downsample/blur chain (`vBuildTexturedQuadMipChain` 0x511620);
- any scene that renders into a texture (`BeginPaletteDmaState` flags & 3);
- `WATER` and particles until ported.

Rule in the runtime: a VU1 batch goes native only if its program is ported **and** the current FRAME_1 targets the display draw buffer. Everything else falls through to the VU1 interpreter + parallel-gs unchanged.

**Composition.** The frame is "3D scene first, then 2D in submission order" (1.2; to be confirmed in M0). Approach in three phases:

1. **Dual-run (bring-up).** Native and emulated 3D both run. The native frame is shown in a debug view (side by side / toggle) for comparison. No composition risk.
2. **Boundary copy + untouched-pixel composite.** Emulated 3D is skipped. At the 3D -> 2D boundary (first non-3D draw into the display buffer after the scene, or the begin of 2D):
   - The native colour+Z target is downsampled into GS VRAM, so 2D blending and any frame-buffer-reading effects see the scene as before. This writes parallel-gs's VRAM buffer from a compute shader; it needs a small hook in parallel-gs (?).
   - At present, the output is the high-res native 3D wherever the final GS frame still equals the boundary copy, and the GS pixel elsewhere. UI is then 1x and 3D is high-res. Translucent UI shows 1x 3D behind it, which is acceptable.
3. **Optional later: native 2D.** Render the GS primitives that target the display buffer (sprites/triangles with GS-VRAM textures) natively too, for high-res UI.

**Graphics thread.** Native batches are recorded on the existing graphics worker, so ordering with GS packets is preserved and the EE thread is unaffected.

## 4. Graphics API

**Recommendation: Vulkan through Granite, on the same `Vulkan::Device` that parallel-gs already uses (`GSGLInterop::device()`).**

- Already linked and initialized. Presentation already goes from a Vulkan image to GL via external memory and semaphores (`gs_gl_interop.cpp`), so the native target composites on the GPU with zero copies.
- One device lets the composite pass read the parallel-gs scanout and the native target directly. The boundary copy can write parallel-gs's VRAM buffer.
- D3D11/12 would add a second device, cross-API sharing, a second shader toolchain and a Windows-only path.

**Open question:** shader compilation. Granite can compile GLSL at runtime if built with shaderc/glslang; otherwise we precompile SPIR-V at build time. To be checked before M2. **Nothing gets installed without asking.**

## 5. Staged plan

**M0: Instrumentation (small, no visual change).**
- The VIF1 interpreter identifies programs by MPG source / hash. It logs per frame: batches per program, FRAME_1 target, the 3D/2D boundary and render-to-texture scenes (`PS2X_NATIVE_TRACE=1`).
- Add a frame-dump tool: VU memory at each MSCAL + GS state. Commit a proper VU1 extractor/disassembler under `tools/`.
- Confirms the ordering assumptions and the open items marked (?).

**M1: Native renderer skeleton.**
- Granite pipeline on the shared device, high-res colour+depth target, frame lifecycle tied to GS vsync/present.
- Debug views (`PS2X_NATIVE3D=0|1|debug`). Draw `IDXTRID`/`IDXSTRPD` + `XFORM1M[N]` untextured, flat-shaded, in dual-run mode.

**M2: Correct static and skinned meshes.**
- All XFORM variants, CALCLIGHT/HEMLIT lighting, VRAM-shadow texture cache, GS state -> pipeline state.
- Batch merging (per-frame streaming vertex/index buffers, one draw per run of identical state).
- Validate against the interpreter: CPU-side reference of the shader maths diffed against VU XGKICK output.

**M3: Remaining 3D programs.**
STRPFAST, SCRPOSTCORD, MULTI/MUL, TOON/HALO outlines, EXTN, SPRITEM1, PARTICLEM1/PARTDIST, WATER/WATERMUL.

**M4: Drop the emulated 3D.**
- Boundary copy + untouched-pixel composite.
- Measure fps; the VU1 interpreter is currently the main graphics cost.

**M5: Enhancements.**
- Internal resolution scale, MSAA, anisotropic filtering.
- Widescreen (projection + engine culling hooks).
- Engine-level texture hook for HD replacement (1.6).

**M6 (optional): Native 2D path / render-to-texture scenes.**

Each milestone stays behind an env flag with the emulated path as the default until it is at parity.

## 6. Risks / unknowns

- **Unconfirmed statically:** exact light-type encoding; the meaning of the extended block 256-259; HALO/MULTI semantics; whether CLIPTRIS or the blur chain is used in practice. M0 traces and the disassembly answer these.
- **Depth:** the VU writes K/w with GS integer Z. The native depth must follow the game's ZTEST mode, and must preserve draw-order semantics where the game relies on GS Z precision or equal-Z passes (multi-pass, outlines).
- **Batch count:** thousands of 40-vertex batches per frame. Merging is required for good GPU performance.
- **Textures:** VRAM re-use within a frame (LRU eviction) makes texture identity by TBP unsafe. Content-hash keyed cache with dirty tracking.
- **parallel-gs hooks:** the boundary copy needs write access to its VRAM buffer; the dependency is pinned and already patched, so this would be another small patch.

## 7. M0 findings (runtime trace, `PS2X_NATIVE_TRACE`)

- **Frame structure.** Display page 0 is the only scanout page. Every frame is drawn into an off-screen
  page at FBP 0x8c in this order: 3D (VU1 batches, with A+D state packets between them), then 2D/UI sprites
  (PATH2). At the start of the next frame one sprite copies 0x8c to page 0 (`G2@8cF0D2`), then 0x8c is cleared.
  The native frame therefore ends at that copy and is shown from page 0 (`ps2native::noteGsDraw`).
- **Programs actually used.**
  - Menus: only `IDXSTRPD`/`XFORM1M`, about 35 two-triangle batches per frame (the menu layers are VU1 quads).
  - Match: about 6.5k batches/s, around 25 triangles each. Mostly `IDXTRID` + `XFORM1MN`, plus `XFORM1M`,
    `XFORM1MNOD` and a few `IDXSTRPD`. Lighting is always `CALCLIGHT`.
  - No multi-bone XFORM, toon, particle or water program showed up in the traced scenes. Characters are
    rigid-skinned through one bone per batch.
- **VU1 call protocol.**
  - `MSCAL 0` only resets the output double buffer.
  - `MSCAL 2` runs the XFORM prologue, which multiplies VP into bones 302-305 in place (`XFORMnM` only; the
    `MN` variants multiply world then VP per vertex), and then the first batch.
  - Each following `MSCNT` processes the next input buffer with the same matrices.
- **Screen mapping.**
  - window(12.4) = clip.xy / w * qw265.xy + (qw264.xy - 1.5*2^23). The offset is a magic float whose
    mantissa is the GS fixed-point coordinate.
  - Z = 16 * qw265.z / w (`FTOI4`).

## 8. Status

**M0 is done.**
- `overrides/billy_mandy_native.cpp` finds the 35 program descriptors in RDRAM and identifies the resident
  main, XFORM and lighting programs by comparing VU1 code memory.
- `PS2X_NATIVE_TRACE=<vsync>[:<n>]` prints the draw stream for `n` vsyncs, plus a per-program histogram every 5 s.
- `tools/vu1_programs.py` extracts and disassembles the microcode.

**M1 is done (dual-run).** Runtime side (PS2Recomp, `native-renderer` branch):
- `PS2Runtime::setVu1Hook` and `setGifPacketObserver` give the game code access to the VU1 and GIF streams.
- `GS::readRawDrawRegisters` returns the GS drawing registers as raw values.
- `ps2native` (`src/lib/native/`) collects decoded batches per frame and draws them in
  `ParallelBackend::Present` on the parallel-gs Vulkan device.
- Shaders are GLSL, compiled to SPIR-V headers at build time by the glslang bundled with Granite.

Game side: `IDXTRID`/`IDXSTRPD` + `XFORM1M`/`1MN`/`1MNOD`/`1MNODN` batches are decoded into object-space
vertices, vertex colours and ST, plus `VP * bone` and the screen mapping.

How to view it:
- `PS2X_NATIVE3D=1` draws the native 3D tinted over the emulated frame.
- `PS2X_NATIVE3D=2` shows the native 3D only.

What it looks like today:
- Geometry lines up with the emulated scene.
- It is untextured and unlit (vertex colours only), so lit characters show black.
- In matches the game produces about 28 distinct frames/s, and native-only mode presents exactly those.
- The title-bar fps counts changed scanouts.
- The VU1 interpreter still runs for every batch.

**M2 (dual-run verified; replace mode working):**
- **Lighting and colour.** `CALCLIGHT` is ported to C++ on the CPU in the override:
  - ambient light, plus directional, point and spot lights, with the nine finalize variants chosen by qw299.y;
  - texgen modes 0, 1024 and normal (reflection mode 256 is counted but not ported) and the 2x3 UV affine;
  - `XFORM2M` two-bone blending.

  The per-vertex cost is small: about 160k vertices/s.
- **Verification.** `PS2X_NATIVE_VERIFY=1` diffs every batch against the VU interpreter's own output:
  - Colours: 70M+ vertices, 0 mismatches, max error 0.00 (all-zero colours are skipped as culled).
  - Positions: about 0.15% of `IDXTRID` vertices differ, all near w = 0, which GPU clipping handles.
  - ST: about 0.4% differ, all on batches with a UV scroll matrix that the interpreter does not apply. The
    program reads the flag word with `ILW` in a branch delay slot, and the interpreter's load latency makes the
    next check see a stale value. Native applies the matrix. Whether the hardware or the interpreter is right
    is open.
- **Textures.** A CPU shadow of GS memory runs on its own thread (`native_textures.cpp`).
  - It is fed in stream order with every GIF packet, including the two GIF DMA fast paths in `ps2_memory.cpp`
    that bypass the arbiter, and `HWREG` uploads.
  - Swizzling uses the new `GSMem::WriteRow`/`ReadRow`.
  - Decoded textures are keyed by the content of the texture's own 256-byte blocks plus format and palette.
    The game streams about 7.5k small uploads/s through a VRAM LRU, so address-keyed caches thrash.
  - `PS2X_NATIVE_SHADOWCHECK=1` compares the shadow with parallel-gs VRAM; sampled CLUTs and texels match exactly.
- **GS state.** TFX/TCC, alpha test (AFAIL KEEP discards), Z test and mask, FBMSK, FBA and the expressible
  ALPHA blend equations are mapped. Framebuffer alpha stores As/0x80 so DST_ALPHA blends work.
- **`PS2X_NATIVE3D=3` (replace).** Handled batches skip the VU1 interpreter entirely. In matches the game then
  draws 45-55 distinct frames/s instead of about 28 (dual-run, worker saturated).
- **Debug aids.** `PS2X_NATIVE_DEBUGVIEW=1` tints draws with a missing texture (magenta), a black texture
  (cyan) or black vertex colours (yellow). `PS2X_NATIVE_TEXSTATS=1` prints texture-cache counters.

**Known gaps** (visible in native-only modes):
- Some objects (statues, parts of characters) are drawn by the VU with zero vertex colour. Their emulated
  colour comes from other draws that native-only mode does not show: direct GS (PATH2) primitive packets in
  the 3D phase, or the unported `IDXTRIMULTI`/`SPRITEM1`/`PARTICLEM1` passes. The GS state at XGKICK time
  matches the state captured at MSCAL (checked over 14.5M batches).
- The 2D full-screen passes are absent, so native-only frames look darker.
- No batch merging yet: about 350 draws per frame, which is fine so far.

**M3/M4: full native frame (`PS2X_NATIVE3D=4`).**
- **Not a composite.** The doc's original M4 (boundary copy + untouched-pixel composite) cannot work here:
  ordinary GS draws are interleaved with the VU1 3D.
- **What the native frame contains:**
  - the native VU1 batches, plus
  - every other GS primitive the game sends to the frame's buffer, replayed natively in stream order
    (`native_gsreplay.cpp`). The replay covers the PATH2/PATH3 packets, the GIF DMA fast path, and the PATH1
    output of VU1 programs without a native port.
- **Effect on M3:** `IDXTRIMULTI`, TOON/HALO, `SPRITEM1` and `PARTICLEM1` are drawn exactly through the
  replay, so porting them is now only a speed item; they run at tens to hundreds of batches/s. Their decode
  specs are in the session scratch (`spec_idx_multipass.md`, `spec_sprite_particle.md`) and summarised in
  section 9.
- **Frame bookkeeping.** The page the game copies to the display is the native draw buffer. Draws into the
  display page (the copy itself) and into render-target pages are left to the emulated GS. None were seen in
  matches (`PS2X_NATIVE_REPLAYSTATS=1`).
- **VU memory writeback.** Some batches read VU memory that earlier program runs left behind (colours in the
  input buffer, lighting accumulators). When a handled batch skips the interpreter, the override writes those
  side effects back. Batches the VU would trivially reject (every vertex outside the same clip plane) are
  dropped exactly like the VU drops them. Without this, statues and characters rendered black.
- **Culling.** `IDXTRID` cull variants (entry bit 1) back-face cull with the VU's rule: keep when the
  window-space cross product is > 0. Strip culling is not done yet.
- **`PS2X_NATIVE_SPLIT=1`** (with mode 2) shows the emulated frame on the left and the native frame on the
  right, for same-frame comparison.
- **Speed.** In matches the game draws 52-57 frames/s, with VU1 interpreter time near zero.

**Fixes after M4:**
- **Speckled ground decal.** The float-input transforms (`XFORM1MNOD`/`MNODN`) take ST as floats, not 4.12
  fixed point.
- **Strip culling.** Vertex k keeps orientation >= 0 when k is even and < 0 when k is odd (the VU's ADC rule).
- **Program identification.** Programs are identified by their full microcode, since several share a prologue.
- **VU matrix semantics are mirrored:**
  - On MSCAL, prologue programs use VP * bones; on MSCNT they use bone memory as it is.
  - The normal-carrying programs use VP * bones every batch.
  - Replace mode applies the prologue's in-place write.
  - MSCNT batches keep the variant chosen at MSCAL.
- **Skinning.** Generalised to `XFORM2M`, `XFORM2MN` and `XFORM3M`. Weights are qw241.x/255 and qw241.y/255;
  the last bone gets the remainder.
- **Black regions in gameplay.** Large dark foreground walls appear in the emulated frame too (split view).
  The game draws them that way.

**Speed ports: `IDXTRIMULTI`/`IDXSTRPMUL`, `SPRITEM1`, `PARTICLEM1`.**
- **MULTI programs.** Pass 1 is the base decode. In UV variants with texgen mode 0, ST is reloaded from the
  UV set the tag selects (low 16 bits of qw301.x non-zero means the zw lanes of 161+).
  Pass 2 is a second batch with the same triangles and culling:
  - tag qw318;
  - colours from the finalize entry at qw299.z, run with material qw319 on pass 1's float colours;
  - ST through the qw318 rules, with affine rows qw322/323.

  `finalizeColor` now takes the colour quadword's raw bits. The ITOF0 entries read them as integers, entries 6
  and 8 read them as floats, and entry 7 clamps lit at 0 only.
- **SPRITEM1 and PARTICLEM1** project their quads on the CPU and submit screen-space batches with exactly
  the GS vertices the VU would kick. MSCNT state is mirrored:
  - SPRITEM1's flags come from the qw299.y read at the end of the previous batch, and the pass count is fixed
    at MSCAL.
  - PARTICLEM1's matrix, corner UVs and GIF tag come from the MSCAL prologue.
  - The prologue and transform addresses in qw301.x/w carry the tag's EOP bit; JR uses the low 11 bits.
  - Only strip and fan PRIMs are handled. Anything else falls back to the interpreter, and so do batches with
    unexpected headers or modes.
- **Kick verification.** With `PS2X_NATIVE_VERIFY`, the expected sprite and particle GS packets are queued
  and compared with the VU's PATH1 output: xy within 2/16 pixel, RGBA within 1, STQ within 1e-3. In
  Underworld, 45,699 kicks (1.41M vertices) matched with 0 mismatches, 0 missing and none declined (reasons
  are counted under `declined:`). The MULTI pass-2 colours are exact (0 colour mismatches over 46M vertices).
- **Speed.** In Underworld the VU1 interpreter time fell from about 830 ms/s to 0 ms/s, and the game now
  runs at 54-59 frames/s instead of 23-40.

**Levels checked** in `PS2X_NATIVE3D=4`:
- Egyptian Tomb, Underworld ("Deep in Evil", `down` twice on the level screen) and Warlockery and
  Sorcerizing ("The Office", `down` three times; 38-58 frames/s, VU1 interpreter idle).
- Coliseum of Chaos (`down` once) shows only the HUD. That level's 3D is black with the native renderer off
  too (the log reports `COLIS4\CAMERA.ADR` not found), so it is an emulation issue, not a native one.
- Menus, character select, battle setup, loading and VS screens.
- When the camera leaves The Office's floating tower, the native frame draws a sky wedge where the emulated
  frame is black (split view). The pre-port build shows the same difference.

**Known verification noise** (`PS2X_NATIVE_VERIFY`, Underworld):
- Colours are exact.
- Some `XFORM1M` MSCNT batches differ in position, but every sampled vertex is far off-screen on both sides.
- Some 4-vertex `IDXSTRPD` HUD strips differ in ST; their GIF tag shows that nothing was emitted.
- ST is read back from the shared qw368+ scratch, which later batches can overwrite before the check.
  About 10-17% of vertices "mismatch" this way, before and after the ports. Kick verification is the reliable
  check for the screen-space programs.

**Fast test input.** Gameplay starts at about 160 s instead of 490 s:

```
PS2X_AUTOPRESS=cross@25,cross@32,cross@38,cross@44,start@49,cross@53,cross@56,cross@59,cross@62,cross@65,cross@68,cross@71,cross@74,cross@77,cross@80,cross@83,start@88,cross@95,cross@109,cross@113,start@119,start@124,cross@139,cross@149,cross@161,cross@171
```

Insert `down@102,down@105` after `cross@95` for the Underworld level. Sprites and particles mostly appear
in combat, so repeated `cross`/`square` presses from 140 s help. `PS2X_HIDE_DEBUG=1` hides the runtime
debug panel, which otherwise covers screenshots.

**Open:**
- Points/lines in the replay (none seen).
- Reflection texgen (mode 256).
- Render-to-texture sampling (none seen).
- Speed ports for TOON/HALO, `PARTDIST` and `XFORM4M`/`EXTN` (not seen at a significant rate yet).
- Upscaling: the native target is still 640x448.

## 9. M3 program notes (from the disassembly, for later speed ports)
- **MULTI/STRPMUL.** Pass 1 is the base emit, with qw301.x selecting the second UV set (zw of 161+). Pass 2
  re-emits every triangle with tag qw318, colour qw319, finalize selected by qw299.z, and ST via qw318.x/w
  and affine qw322/323.
- **TOON/HALO.** Per batch, in this order:
  1. An outline of back-facing triangles (S <= 0), with xy offset by floor(worldNormal.xy * qw256.w), using
     colour/tag from qw256-258.
  2. The main pass, front faces only.
  3. The optional second pass.

  HALO adds ALPHA_2 = Cs + Cd before the outline.
- **SPRITEM1.** Up to 10 sprites per buffer:
  - the centre is at 81+4k and the axes at 201/202+4k;
  - corners are c +/- B +/- A with UV (0,0)..(1,1), and all corners share Q = 1/clip0.w;
  - the output is a tri-strip with ADC on the first two vertices.
- **PARTICLEM1 / PARTDIST.** One qw per particle (xyz, t). Size and colour come from polynomials in qw328-336.
  PARTDIST emits 2-vertex sprites with screen-space UVs.
