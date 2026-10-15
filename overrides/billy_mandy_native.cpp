// Native renderer front end for Billy & Mandy (SLUS_214.51): identifies the engine's VU1 microprograms, traces
// the frame's draw stream (M0) and decodes VU1 batches into ps2native::Batch for the host GPU (M1).
// See docs/native-renderer.md. Environment:
//   PS2X_NATIVE_TRACE=<vsync>[:<count>]  print the draw stream of <count> vsyncs (default 3) from <vsync>,
//                                        plus a per-program batch histogram every 5 s
//   PS2X_NATIVE3D=1|2                    decode supported batches and draw them natively (1 overlay, 2 native only)
//   PS2X_NATIVE_VERIFY=1                 compare the native vertex colours with what the VU1 interpreter produced
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/native/native_renderer.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    // Program descriptors: name[16], +0x10 VIF packet pointer, +0x14 QWC, +0x16 entry/load, +0x18 u16[16] entries.
    // They sit in the ELF data section between these addresses.
    constexpr uint32_t kDescriptorScanBegin = 0x00512000u;
    constexpr uint32_t kDescriptorScanEnd = 0x00536000u;

    // VU1 instruction-memory slots the engine loads programs into (instruction index).
    constexpr uint32_t kMainSlot = 0u;
    constexpr uint32_t kXformSlot = 1428u;
    constexpr uint32_t kLightSlot = 1596u;

    // VU1 data memory (quadword addresses).
    constexpr uint32_t kQwViewProj = 260u;
    constexpr uint32_t kQwLightColors = 266u;   // + k; w low nibble = type, qw266.w high nibble = light count
    constexpr uint32_t kQwLightVectors = 274u;  // + k: direction (directional, spot) or position (point)
    constexpr uint32_t kQwLightParams = 282u;   // + k: point attenuation (a, b, c) / spot (range, cone)
    constexpr uint32_t kQwSpotPositions = 290u; // + k
    constexpr uint32_t kQwAmbient = 298u;
    constexpr uint32_t kQwEntries = 299u; // x = main variant entry, y = colour finalize entry
    constexpr uint32_t kQwMaterial = 300u;
    constexpr uint32_t kQwScreenOffset = 264u;
    constexpr uint32_t kQwScreenScale = 265u;
    constexpr uint32_t kQwGifTag = 301u;
    constexpr uint32_t kQwBones = 302u;
    constexpr uint32_t kQwUvMatrix = 320u; // two rows: s' = r0.x*s + r1.x*t + r0.w, t' = r0.y*s + r1.y*t + r1.w
    // Input buffer layout relative to the buffer base (XITOP).
    constexpr uint32_t kInIndices = 1u;
    constexpr uint32_t kInPositions = 81u;
    constexpr uint32_t kInColors = 121u;
    constexpr uint32_t kInST = 161u;
    constexpr uint32_t kInNormals = 201u;
    constexpr uint32_t kInWeights = 241u; // x = bone 0 weight, 0..255 (XFORM2M)
    constexpr uint32_t kMaxBatchVertices = 64u;

    struct Program
    {
        std::string name;
        uint32_t slot = 0; // instruction index of its first MPG chunk
        struct Chunk
        {
            uint32_t dest = 0; // instruction index
            std::vector<uint8_t> code;
        };
        std::vector<Chunk> chunks; // all of its microcode (programs can share a prologue, so all of it is compared)
        size_t size = 0;
        uint16_t entries[16]{};         // descriptor entry table (+0x18)
    };

    enum class MainKind : uint8_t
    {
        Other,
        IdxTri,   // VU1IDXTRID
        IdxStrip, // VU1IDXSTRPD
    };

    struct State
    {
        PS2Runtime *runtime = nullptr;
        bool scanned = false;
        std::vector<Program> programs;
        uint64_t identifiedGeneration = ~0ull;
        int current[3] = {-1, -1, -1}; // main, xform, light program indices

        // Captured at MSCAL: object -> clip for the current bone (VP * bone 0).
        float objectToClip[16]{};
        float bone[16]{}; // object -> world (bone 0 as uploaded, before the XFORM prologue)
        float bone2[16]{}; // bone 1 (XFORM2M/2MN/3M)
        float bone3[16]{}; // bone 2 (XFORM3M)
        float viewProj[16]{};
        int bones = 1; // of the resident XFORM program
        bool freshCall = false; // the batch being decoded started with MSCAL (or the MSCNT after the init call)
        int callVariant = -1;   // main-program variant chosen by that MSCAL
        bool haveMatrix = false;
        bool writeBack = false; // handled batches skip the interpreter, so their VU memory side effects are applied here

        // PS2X_NATIVE_VERIFY: last batch's colours, checked against the VU's output at the next call.
        bool verify = false;
        struct PendingVerify
        {
            uint32_t base = 0, count = 0;
            float colors[kMaxBatchVertices][4];
            float st[kMaxBatchVertices][2];
            float screen[kMaxBatchVertices][2]; // window coords as the VU stores them (magic offset included)
            int32_t stInput[kMaxBatchVertices][4];
            bool fresh = false;
            int mainVariant = -1;
            uint32_t flags = 0;
            float uv[8]{};          // qw320-321 as read at decode time
            float rawSt[2]{};       // vertex 0 ST before texgen/affine
            char programs[64]{};
        } pending;
        bool havePending = false;
        uint64_t verified = 0, mismatched = 0, skipped = 0, stMismatched = 0, posMismatched = 0;
        float maxError = 0.0f, maxStError = 0.0f, maxPosError = 0.0f;
        uint64_t unsupportedLight = 0, uvTransform = 0, unsupportedTexgen = 0;
        bool afterInit = false; // last MSCAL was the init entry, so the next MSCNT starts a fresh call

        // Trace.
        bool trace = false;
        uint64_t traceBegin = 0, traceEnd = 0;
        uint64_t traceTick = ~0ull;
        std::string traceToken;
        uint32_t traceRun = 0;
        std::string traceLine;
        std::map<std::string, uint64_t> histogram;
        std::chrono::steady_clock::time_point histogramStart = std::chrono::steady_clock::now();
        std::mutex traceMutex; // GIF packets can arrive from the EE thread as well as the graphics worker
    };

    State g_state;

    uint32_t readU32(const uint8_t *p)
    {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }

    void scanPrograms(State &s)
    {
        s.scanned = true;
        const uint8_t *ram = s.runtime->memory().getRDRAM();
        for (uint32_t addr = kDescriptorScanBegin; addr + 0x38u <= kDescriptorScanEnd; addr += 16u)
        {
            const char *name = reinterpret_cast<const char *>(ram + addr);
            if (std::memcmp(name, "VU1", 3) != 0)
                continue;
            size_t len = 3;
            while (len < 16 && name[len] != 0 && ((name[len] >= 'A' && name[len] <= 'Z') || (name[len] >= '0' && name[len] <= '9')))
                ++len;
            if (len >= 16 || name[len] != 0)
                continue;
            const uint32_t packet = readU32(ram + addr + 0x10u);
            uint16_t qwc = 0;
            std::memcpy(&qwc, ram + addr + 0x14u, 2);
            if (packet < 0x00100000u || packet + qwc * 16u > 0x00600000u || qwc == 0u)
                continue;
            // Walk the VIF packet, collecting its MPG chunks.
            Program p;
            p.name.assign(name + 3, len - 3);
            std::memcpy(p.entries, ram + addr + 0x18u, sizeof(p.entries));
            for (uint32_t pos = 0; pos + 4u <= qwc * 16u;)
            {
                const uint32_t code = readU32(ram + packet + pos);
                pos += 4u;
                const uint8_t cmd = uint8_t((code >> 24) & 0x7fu);
                if (cmd == 0x20u) // STMASK
                {
                    pos += 4u;
                    continue;
                }
                if (cmd == 0x4au) // MPG
                {
                    const uint32_t count = ((code >> 16) & 0xffu) ? ((code >> 16) & 0xffu) : 256u;
                    if (pos + count * 8u > qwc * 16u)
                        break;
                    Program::Chunk chunk;
                    chunk.dest = code & 0xffffu;
                    chunk.code.assign(ram + packet + pos, ram + packet + pos + count * 8u);
                    if (p.chunks.empty())
                        p.slot = chunk.dest;
                    p.size += chunk.code.size();
                    p.chunks.push_back(std::move(chunk));
                    pos += count * 8u;
                    continue;
                }
                if (cmd != 0x00u && cmd != 0x01u && cmd != 0x05u && cmd != 0x10u && cmd != 0x11u && cmd != 0x13u)
                    break;
            }
            if (!p.chunks.empty())
                s.programs.push_back(std::move(p));
        }
        std::fprintf(stderr, "[native] found %zu VU1 programs\n", s.programs.size());
    }

    void identifyPrograms(State &s)
    {
        const uint64_t generation = s.runtime->memory().getVU1CodeGeneration();
        if (generation == s.identifiedGeneration)
            return;
        s.identifiedGeneration = generation;
        const uint8_t *code = s.runtime->memory().getVU1Code();
        const uint32_t slots[3] = {kMainSlot, kXformSlot, kLightSlot};
        for (int k = 0; k < 3; ++k)
        {
            s.current[k] = -1;
            size_t best = 0;
            for (size_t i = 0; i < s.programs.size(); ++i)
            {
                const Program &p = s.programs[i];
                if (p.slot != slots[k] || p.size <= best)
                    continue;
                bool match = true;
                for (const auto &chunk : p.chunks)
                    if (chunk.dest * 8u + chunk.code.size() > PS2_VU1_CODE_SIZE ||
                        std::memcmp(code + chunk.dest * 8u, chunk.code.data(), chunk.code.size()) != 0)
                    {
                        match = false;
                        break;
                    }
                if (match)
                {
                    s.current[k] = int(i);
                    best = p.size;
                }
            }
        }
    }

    const char *programName(const State &s, int k)
    {
        return s.current[k] >= 0 ? s.programs[size_t(s.current[k])].name.c_str() : "?";
    }

    MainKind mainKind(const State &s)
    {
        const char *name = programName(s, 0);
        if (std::strcmp(name, "IDXTRID") == 0)
            return MainKind::IdxTri;
        if (std::strcmp(name, "IDXSTRPD") == 0)
            return MainKind::IdxStrip;
        return MainKind::Other;
    }

    // Supported transforms: 1M, 1MN (dequantised s16 positions, s8 normals), 1MNOD, 1MNODN (float positions
    // and normals) and 2M (two bones blended by a per-vertex weight). normals: 0 none, 1 s8 / 127, 2 float.
    bool supportedXform(State &s, bool &dequantize, int &normals)
    {
        const char *name = programName(s, 1);
        // Blended skinning: XFORM2M, XFORM2MN (s8 normals), XFORM3M.
        if (std::strcmp(name, "XFORM2M") == 0 || std::strcmp(name, "XFORM2MN") == 0 || std::strcmp(name, "XFORM3M") == 0)
        {
            dequantize = true;
            normals = std::strcmp(name, "XFORM2MN") == 0 ? 1 : 0;
            s.bones = name[5] - '0';
            return true;
        }
        if (std::strncmp(name, "XFORM1M", 7) != 0 || std::strstr(name, "EXTN") != nullptr)
            return false;
        dequantize = std::strstr(name, "NOD") == nullptr;
        normals = std::strcmp(name, "XFORM1MN") == 0 ? 1 : (std::strcmp(name, "XFORM1MNODN") == 0 ? 2 : 0);
        s.bones = 1;
        return true;
    }

    const uint8_t *qw(const uint8_t *vu, uint32_t index)
    {
        return vu + (index & 0x3ffu) * 16u;
    }

    void readVec(const uint8_t *vu, uint32_t index, float out[4])
    {
        std::memcpy(out, qw(vu, index), 16);
    }

    void readInts(const uint8_t *vu, uint32_t index, int32_t out[4])
    {
        std::memcpy(out, qw(vu, index), 16);
    }

    // M = VP * B, both stored as four column quadwords (the VU computes ACC = c0*x + c1*y + c2*z + c3*w).
    // Column-major 4x4 helpers.
    void multiply(const float *a, const float *b, float *out)
    {
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
            {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                    sum += a[k * 4 + row] * b[col * 4 + k];
                out[col * 4 + row] = sum;
            }
    }

    bool invert(const float *m, float *out)
    {
        double a[4][8];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
            {
                a[r][c] = m[c * 4 + r];
                a[r][c + 4] = r == c ? 1.0 : 0.0;
            }
        for (int c = 0; c < 4; ++c)
        {
            int pivot = c;
            for (int r = c + 1; r < 4; ++r)
                if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
                    pivot = r;
            if (std::fabs(a[pivot][c]) < 1e-20)
                return false;
            std::swap(a[c], a[pivot]);
            const double inv = 1.0 / a[c][c];
            for (int k = 0; k < 8; ++k)
                a[c][k] *= inv;
            for (int r = 0; r < 4; ++r)
                if (r != c)
                {
                    const double f = a[r][c];
                    for (int k = 0; k < 8; ++k)
                        a[r][k] -= f * a[c][k];
                }
        }
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                out[c * 4 + r] = float(a[r][c + 4]);
        return true;
    }

    // The matrices the VU transforms this batch with. XFORMnM programs (1M, 1MNOD, 2M, 3M) run a prologue on
    // MSCAL that multiplies VP into the bones in place; later MSCNT batches use bone memory as it is (normally
    // those premultiplied values, but the EE may have uploaded new raw bones meanwhile). The normal-carrying
    // programs multiply by VP per vertex every batch. Bones as world matrices come out as VP^-1 * clip matrix.
    void captureMatrix(State &s, uint8_t *vu, bool freshCall, bool prologue, bool writeBack)
    {
        float vp[16];
        std::memcpy(vp, qw(vu, kQwViewProj), 64);
        std::memcpy(s.viewProj, vp, sizeof(s.viewProj));
        float vpInverse[16];
        const bool haveInverse = prologue && invert(vp, vpInverse);
        float *world[3] = {s.bone, s.bone2, s.bone3};
        for (int k = 0; k < 3; ++k)
        {
            float memory[16], clip[16];
            std::memcpy(memory, qw(vu, kQwBones + 4u * uint32_t(k)), 64);
            if (prologue && !freshCall)
                std::memcpy(clip, memory, sizeof(clip));
            else
                multiply(vp, memory, clip);
            if (k == 0)
                std::memcpy(s.objectToClip, clip, sizeof(clip));
            if (prologue)
            {
                if (freshCall && writeBack && k < s.bones)
                    std::memcpy(vu + ((kQwBones + 4u * uint32_t(k)) & 0x3ffu) * 16u, clip, 64); // the prologue's in-place write
                if (haveInverse)
                    multiply(vpInverse, clip, world[k]);
                else
                    std::memcpy(world[k], memory, 64);
            }
            else
                std::memcpy(world[k], memory, 64);
        }
        s.haveMatrix = true;
    }

    int entryIndex(const State &s, int slot, uint32_t address)
    {
        if (s.current[slot] < 0)
            return -1;
        const Program &p = s.programs[size_t(s.current[slot])];
        for (int i = 0; i < 16; ++i)
            if (p.entries[i] == address)
                return i;
        return -1;
    }

    // VU1CALCLIGHT accumulation: ambient + directional (type 4), point (6) and spot (8) lights per vertex, from
    // world positions and world normals.
    void accumulateLights(const uint8_t *vu, uint32_t count, const float (*world)[3], const float (*normal)[3], float (*acc)[4])
    {
        int32_t ambient[4];
        readInts(vu, kQwAmbient, ambient);
        for (uint32_t i = 0; i < count; ++i)
        {
            for (int c = 0; c < 3; ++c)
                acc[i][c] = float(ambient[c]);
            acc[i][3] = 1.0f;
        }
        int32_t first[4];
        readInts(vu, kQwLightColors, first);
        const uint32_t lights = std::min<uint32_t>((uint32_t(first[3]) & 240u) >> 4, 8u);
        auto dot = [](const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        for (uint32_t k = 0; k < lights; ++k)
        {
            int32_t colorInt[4];
            readInts(vu, kQwLightColors + k, colorInt);
            const float color[3] = {float(colorInt[0]), float(colorInt[1]), float(colorInt[2])};
            float vec[4], params[4], spot[4];
            readVec(vu, kQwLightVectors + k, vec);
            readVec(vu, kQwLightParams + k, params);
            readVec(vu, kQwSpotPositions + k, spot);
            switch (uint32_t(colorInt[3]) & 15u)
            {
            case 4: // directional
            {
                const float l[3] = {-vec[0], -vec[1], -vec[2]};
                for (uint32_t i = 0; i < count; ++i)
                {
                    const float d = std::max(dot(l, normal[i]), 0.0f);
                    for (int c = 0; c < 3; ++c)
                        acc[i][c] += color[c] * d;
                }
                break;
            }
            case 6: // point: N.L / (a + b*d + c*d^2)
                for (uint32_t i = 0; i < count; ++i)
                {
                    const float d[3] = {vec[0] - world[i][0], vec[1] - world[i][1], vec[2] - world[i][2]};
                    const float d2 = dot(d, d);
                    const float inv = 1.0f / std::sqrt(d2);
                    const float atten = 1.0f / (params[0] + params[1] * d2 * inv + params[2] * d2);
                    const float l[3] = {d[0] * inv, d[1] * inv, d[2] * inv};
                    const float f = dot(l, normal[i]) * atten;
                    for (int c = 0; c < 3; ++c)
                        acc[i][c] += std::max(color[c] * f, 0.0f);
                }
                break;
            case 8: // spot: N.L * (1 - d / range) inside the cone
                for (uint32_t i = 0; i < count; ++i)
                {
                    const float d[3] = {spot[0] - world[i][0], spot[1] - world[i][1], spot[2] - world[i][2]};
                    const float inv = 1.0f / std::sqrt(dot(d, d));
                    if (params[1] - dot(d, vec) * inv < 0.0f)
                        continue;
                    const float ndotd = std::max(dot(d, normal[i]), 0.0f);
                    const float f = inv - 1.0f / params[0];
                    for (int c = 0; c < 3; ++c)
                        acc[i][c] += std::max(color[c] * ndotd * f, 0.0f);
                }
                break;
            default:
                break;
            }
        }
    }

    // VU1CALCLIGHT finalize entries, by index in the program's entry table (qw299.y selects one):
    // 0 material, 1 vertex*material, 2 lit*material, 3 lit*vertex*material, 4/5 as 0/1 (lit kept for a second
    // pass), 6 untouched, 7 lit*material + vertex*material, 8 colour*material.
    void finalizeColor(int variant, const int32_t vcolInt[4], const float lit[4], const float mat[4], float out[4])
    {
        float vcol[4], a[4];
        for (int c = 0; c < 4; ++c)
        {
            vcol[c] = float(vcolInt[c]);
            a[c] = std::min(std::max(lit[c], 0.0f), 255.0f);
        }
        for (int c = 0; c < 4; ++c)
        {
            switch (variant)
            {
            case 0:
            case 4:
                out[c] = mat[c] * 255.0f;
                break;
            case 2:
                out[c] = a[c] * (mat[c] * 255.0f);
                break;
            case 3:
                out[c] = a[c] * (vcol[c] * mat[c]);
                break;
            case 7:
                out[c] = std::min(c < 3 ? a[c] * mat[c] + vcol[c] * mat[c] : vcol[c] * mat[c], 255.0f);
                break;
            default: // 1, 5, and the cases that keep or rescale existing colours
                out[c] = vcol[c] * mat[c];
                break;
            }
        }
    }

    uint32_t gsColor(const float c[4])
    {
        uint32_t out = 0;
        for (int i = 0; i < 4; ++i)
            out |= (uint32_t(int32_t(c[i])) & 0xffu) << (i * 8); // FTOI0, then the GS takes the low byte
        return out;
    }

    // Returns true when the batch was handed to the native renderer.
    bool decodeBatch(State &s, const uint8_t *vu, uint32_t base, MainKind kind, bool dequantize, int normals)
    {
        int32_t header[4];
        readInts(vu, base, header);
        float headerF[4];
        readVec(vu, base, headerF);
        const uint32_t vertexCount = uint32_t(header[1]);
        if (vertexCount == 0u || vertexCount > kMaxBatchVertices)
            return false;

        ps2native::Batch batch;
        std::memcpy(batch.objectToClip, s.bones >= 2 ? s.viewProj : s.objectToClip, sizeof(batch.objectToClip));
        float scale[4], offset[4];
        readVec(vu, kQwScreenScale, scale);
        readVec(vu, kQwScreenOffset, offset);
        // The offset carries 1.5 * 2^23 so the float's mantissa is the GS 12.4 coordinate; remove it.
        for (int i = 0; i < 2; ++i)
        {
            batch.screenScale[i] = scale[i];
            batch.screenOffset[i] = offset[i] >= 8388608.0f ? offset[i] - 12582912.0f : offset[i];
        }
        batch.depthScale = scale[2] * 16.0f; // FTOI4 of K / w
        uint64_t tag[2];
        std::memcpy(tag, qw(vu, kQwGifTag), 16);
        s.runtime->gs().readRawDrawRegisters(batch.regs);
        batch.prim = ((tag[0] >> 46) & 1u) ? ((tag[0] >> 47) & 0x7ffu) : batch.regs.prim;
        batch.debugTag = uint32_t(s.current[0] * 64 + s.current[1]);
        if ((batch.prim >> 4) & 1u)
        {
            const unsigned ctx = unsigned(batch.prim >> 9) & 1u;
            batch.texture = ps2native::requestTexture(batch.regs.tex0[ctx], batch.regs.texa);
            static const bool shadowCheck = std::getenv("PS2X_NATIVE_SHADOWCHECK") != nullptr;
            static auto lastCheck = std::chrono::steady_clock::now();
            if (shadowCheck && std::chrono::steady_clock::now() - lastCheck > std::chrono::seconds(5))
            {
                lastCheck = std::chrono::steady_clock::now();
                const uint64_t tex0 = batch.regs.tex0[ctx];
                const uint32_t tbp = uint32_t(tex0 & 0x3fffu), tbw = uint32_t((tex0 >> 14) & 63u), psm = uint32_t((tex0 >> 20) & 63u);
                const uint32_t cbp = uint32_t((tex0 >> 37) & 0x3fffu);
                uint32_t clutDiff = 0, texDiff = 0, clutNonZero = 0;
                for (uint32_t y = 0; y < 16u; ++y)
                    for (uint32_t x = 0; x < 16u; ++x)
                    {
                        const uint32_t real = s.runtime->gs().ReadVram(0u, cbp, 1u, x, y);
                        clutNonZero += real != 0u;
                        clutDiff += real != ps2native::debugShadowRead(0u, cbp, 1u, x, y);
                        const uint32_t realT = s.runtime->gs().ReadVram(psm, tbp, tbw, x, y);
                        texDiff += realT != ps2native::debugShadowRead(psm, tbp, tbw, x, y);
                    }
                std::fprintf(stderr, "[native-shadow] tex0 0x%016llx: clut 16x16 differs at %u/256 (real nonzero %u), texels 16x16 differ at %u/256\n",
                             (unsigned long long)tex0, clutDiff, clutNonZero, texDiff);
            }
        }

        // Which variant the main program runs: bit 0 of its entry index = UV transform, bit 2 = lighting.
        int32_t entries[4];
        readInts(vu, kQwEntries, entries);
        // MSCNT batches continue in the variant chosen at MSCAL (only qw299.y is re-read per batch).
        if (s.freshCall)
            s.callVariant = entryIndex(s, 0, uint32_t(entries[0]) & 1023u);
        const int mainVariant = s.callVariant;
        const bool lit = mainVariant >= 0 && (mainVariant & 4) != 0;
        if (mainVariant >= 0 && (mainVariant & 1) != 0)
            ++s.uvTransform;
        const int finalize = entryIndex(s, 2, uint32_t(entries[1]) & 0xffffu);
        float material[4];
        readVec(vu, kQwMaterial, material);

        float world[kMaxBatchVertices][3], normal[kMaxBatchVertices][3], acc[kMaxBatchVertices][4];
        batch.vertices.resize(vertexCount);
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            ps2native::Vertex &v = batch.vertices[i];
            if (dequantize)
            {
                int32_t p[4];
                readInts(vu, base + kInPositions + i, p);
                for (int c = 0; c < 3; ++c)
                    v.pos[c] = float(p[c]) * headerF[2] + headerF[3];
            }
            else
            {
                float p[4];
                readVec(vu, base + kInPositions + i, p);
                std::memcpy(v.pos, p, sizeof(v.pos));
            }
            // Bone weights: 2 bones w0 = x / 255, w1 = 1 - w0; 3 bones w0 = x / 255, w1 = y / 255, w2 = 1 - w0 - w1.
            float weights[3] = {1.0f, 0.0f, 0.0f};
            if (s.bones >= 2)
            {
                int32_t weight[4];
                readInts(vu, base + kInWeights + i, weight);
                weights[0] = float(weight[0]) * 0.00392157f;
                weights[1] = s.bones == 3 ? float(weight[1]) * 0.00392157f : 1.0f - weights[0];
                weights[2] = s.bones == 3 ? 1.0f - (weights[0] + weights[1]) : 0.0f;
            }
            const float *bones[3] = {s.bone, s.bone2, s.bone3};
            for (int r = 0; r < 3; ++r)
            {
                world[i][r] = 0.0f;
                for (int k = 0; k < s.bones; ++k)
                    world[i][r] += weights[k] * (bones[k][r] * v.pos[0] + bones[k][4 + r] * v.pos[1] + bones[k][8 + r] * v.pos[2] + bones[k][12 + r]);
            }
            if (s.bones >= 2)
                std::memcpy(v.pos, world[i], sizeof(v.pos)); // the GPU then applies VP alone
            float n[3] = {0.0f, 0.0f, 0.0f};
            if (normals == 1)
            {
                int32_t ni[4];
                readInts(vu, base + kInNormals + i, ni);
                for (int r = 0; r < 3; ++r)
                    n[r] = float(ni[r]) * 0.00787402f; // s8 / 127
            }
            else if (normals == 2)
            {
                float nf[4];
                readVec(vu, base + kInNormals + i, nf);
                std::memcpy(n, nf, sizeof(n));
            }
            for (int r = 0; r < 3; ++r)
            {
                normal[i][r] = 0.0f;
                for (int k = 0; k < s.bones; ++k)
                    normal[i][r] += weights[k] * (bones[k][r] * n[0] + bones[k][4 + r] * n[1] + bones[k][8 + r] * n[2]);
            }
            if (dequantize)
            {
                int32_t st[4];
                readInts(vu, base + kInST + i, st);
                v.st[0] = float(st[0]) / 4096.0f; // ITOF12
                v.st[1] = float(st[1]) / 4096.0f;
            }
            else
            {
                float st[4]; // the float-input transforms (NOD) take ST as floats too
                readVec(vu, base + kInST + i, st);
                v.st[0] = st[0];
                v.st[1] = st[1];
            }
        }

        const bool hemisphere = std::strcmp(programName(s, 2), "CALCLIGHT") != 0;
        if (lit && !hemisphere)
            accumulateLights(vu, vertexCount, world, normal, acc);
        else
        {
            if (lit)
                ++s.unsupportedLight;
            for (uint32_t i = 0; i < vertexCount; ++i)
                acc[i][0] = acc[i][1] = acc[i][2] = acc[i][3] = 0.0f;
        }
        s.pending.base = base;
        s.pending.count = vertexCount;

        // The VU drops the whole batch (no lighting, no finalize, no output) when every vertex is outside the same
        // clip plane: AND of the CLIP flags (x > w, x < -w, y > w, y < -w, z > w, z < -w) non-zero.
        uint32_t clipAnd = 63u;
        for (uint32_t i = 0; i < vertexCount && clipAnd; ++i)
        {
            const float *m = batch.objectToClip;
            const float *pp = batch.vertices[i].pos;
            float clip[4];
            for (int r = 0; r < 4; ++r)
                clip[r] = m[r] * pp[0] + m[4 + r] * pp[1] + m[8 + r] * pp[2] + m[12 + r];
            const float w = std::fabs(clip[3]);
            uint32_t flags = 0;
            for (int axis = 0; axis < 3; ++axis)
            {
                flags |= (clip[axis] > w ? 1u : 0u) << (axis * 2);
                flags |= (clip[axis] < -w ? 2u : 0u) << (axis * 2);
            }
            clipAnd &= flags;
        }
        const bool rejected = clipAnd != 0u;

        uint8_t *vuMutable = s.writeBack ? s.runtime->memory().getVU1Data() : nullptr;
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            int32_t c[4];
            readInts(vu, base + kInColors + i, c);
            float out[4];
            finalizeColor(finalize, c, acc[i], material, out);
            std::memcpy(s.pending.colors[i], out, sizeof(out));
            batch.vertices[i].rgba = gsColor(out);
        }
        s.havePending = s.verify;

        // When the interpreter is skipped, leave VU memory as the program would: later batches read some of it
        // back (colours left in the input buffer, lighting accumulators, world normals and positions).
        if (vuMutable && !rejected)
        {
            const bool keepsLit = finalize == 4 || finalize == 5;
            for (uint32_t i = 0; i < vertexCount; ++i)
            {
                std::memcpy(vuMutable + ((base + kInColors + i) & 0x3ffu) * 16u, s.pending.colors[i], 16);
                float world4[4] = {world[i][0], world[i][1], world[i][2], 0.0f};
                std::memcpy(vuMutable + ((328u + i) & 0x3ffu) * 16u, world4, 12);
                if (normals != 0)
                {
                    float n4[4] = {normal[i][0], normal[i][1], normal[i][2], 0.0f};
                    std::memcpy(vuMutable + ((base + kInNormals + i) & 0x3ffu) * 16u, n4, 12);
                }
                if (lit)
                {
                    float a[4];
                    for (int c = 0; c < 4; ++c)
                        a[c] = keepsLit ? std::min(std::max(acc[i][c], 0.0f), 255.0f) : acc[i][c];
                    std::memcpy(vuMutable + ((976u + i) & 0x3ffu) * 16u, a, 16);
                }
            }
        }
        if (rejected)
            return true; // nothing drawn, exactly like the VU

        // UV variants (entry index bit 0): texgen selected by GIF-tag w bits 8-10, then an optional 2x3 affine.
        if (mainVariant >= 0 && (mainVariant & 1) != 0)
        {
            const uint32_t flags = uint32_t(tag[1] >> 32);
            const uint32_t texgen = flags & 1792u;
            if (texgen == 256u)
                ++s.unsupportedTexgen; // reflection mapping (ESIN), not ported yet
            for (uint32_t i = 0; i < vertexCount; ++i)
            {
                ps2native::Vertex &v = batch.vertices[i];
                if (texgen == 1024u) // lit intensity ramp
                {
                    v.st[0] = std::min(std::max(acc[i][0], 0.0f), 255.0f) * 0.00390625f;
                    v.st[1] = 0.0f;
                }
                else if (texgen != 0u && texgen != 256u) // transformed normal
                {
                    v.st[0] = normal[i][0];
                    v.st[1] = normal[i][1];
                }
            }
            if (flags & 63u)
            {
                float r0[4], r1[4];
                readVec(vu, kQwUvMatrix, r0);
                readVec(vu, kQwUvMatrix + 1u, r1);
                std::memcpy(s.pending.uv, r0, 16);
                std::memcpy(s.pending.uv + 4, r1, 16);
                s.pending.rawSt[0] = batch.vertices[0].st[0];
                s.pending.rawSt[1] = batch.vertices[0].st[1];
                for (auto &v : batch.vertices)
                {
                    const float s0 = v.st[0], t0 = v.st[1];
                    v.st[0] = r0[0] * s0 + r1[0] * t0 + r0[3];
                    v.st[1] = r0[1] * s0 + r1[1] * t0 + r1[3];
                }
            }
        }
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            const float *m = batch.objectToClip;
            const float *p = batch.vertices[i].pos;
            float clip[4];
            for (int r = 0; r < 4; ++r)
                clip[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
            for (int c = 0; c < 2; ++c)
                s.pending.screen[i][c] = clip[c] / clip[3] * scale[c] + offset[c];
        }
        for (uint32_t i = 0; i < vertexCount; ++i)
            readInts(vu, base + kInST + i, s.pending.stInput[i]);
        s.pending.fresh = s.freshCall;
        s.pending.mainVariant = mainVariant;
        s.pending.flags = uint32_t(tag[1] >> 32);
        std::snprintf(s.pending.programs, sizeof(s.pending.programs), "%s/%s", programName(s, 0), programName(s, 1));
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            s.pending.st[i][0] = batch.vertices[i].st[0];
            s.pending.st[i][1] = batch.vertices[i].st[1];
        }

        auto addTriangle = [&](int32_t a, int32_t b, int32_t c)
        {
            if (uint32_t(a) < vertexCount && uint32_t(b) < vertexCount && uint32_t(c) < vertexCount && a != b && b != c && a != c)
            {
                batch.indices.push_back(uint16_t(a));
                batch.indices.push_back(uint16_t(b));
                batch.indices.push_back(uint16_t(c));
            }
        };

        // Entry index bit 1 selects the face-culled emit routine. Lists keep a triangle iff its window-space
        // cross product (p1 - p0) x (p2 - p0) is positive; strips use the same orientation with alternating sign.
        const bool cull = mainVariant >= 0 && (mainVariant & 2) != 0;
        float window[kMaxBatchVertices][2];
        if (cull)
            for (uint32_t i = 0; i < vertexCount; ++i)
            {
                const float *m = batch.objectToClip;
                const float *p = batch.vertices[i].pos;
                float clip[4];
                for (int r = 0; r < 4; ++r)
                    clip[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
                window[i][0] = clip[0] / clip[3] * scale[0];
                window[i][1] = clip[1] / clip[3] * scale[1];
            }
        auto orientation = [&](int32_t a, int32_t b, int32_t c)
        {
            const float *p0 = window[a], *p1 = window[b], *p2 = window[c];
            return (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p1[1] - p0[1]) * (p2[0] - p0[0]);
        };

        if (kind == MainKind::IdxTri)
        {
            const uint32_t triangles = std::min<uint32_t>(uint32_t(header[0]), 80u);
            for (uint32_t t = 0; t < triangles; ++t)
            {
                int32_t idx[4];
                readInts(vu, base + kInIndices + t, idx);
                if (cull && uint32_t(idx[0]) < vertexCount && uint32_t(idx[1]) < vertexCount && uint32_t(idx[2]) < vertexCount &&
                    !(orientation(idx[0], idx[1], idx[2]) > 0.0f))
                    continue;
                addTriangle(idx[0], idx[1], idx[2]);
            }
        }
        else
        {
            // Up to four strips; their vertex counts are in qw base+1, indices four per quadword from base+2,
            // each strip starting on a quadword boundary.
            int32_t counts[4];
            readInts(vu, base + 1u, counts);
            uint32_t indexQw = base + 2u;
            for (int strip = 0; strip < 4 && counts[strip] > 0; ++strip)
            {
                const uint32_t n = std::min<uint32_t>(uint32_t(counts[strip]), 128u);
                std::vector<int32_t> list(n);
                for (uint32_t k = 0; k < n; ++k)
                {
                    int32_t idx[4];
                    readInts(vu, indexQw + k / 4u, idx);
                    list[k] = idx[k & 3u];
                }
                for (uint32_t k = 2; k < n; ++k)
                {
                    // The VU sets ADC on vertex k when the sign of the orientation differs from k's parity:
                    // even k keeps orientation >= 0, odd k keeps orientation < 0.
                    if (cull && uint32_t(list[k - 2]) < vertexCount && uint32_t(list[k - 1]) < vertexCount && uint32_t(list[k]) < vertexCount)
                    {
                        const float z = orientation(list[k - 2], list[k - 1], list[k]);
                        const bool negative = std::signbit(z);
                        if (negative != ((k & 1u) != 0u))
                            continue;
                    }
                    addTriangle(list[k - 2], list[k - 1], list[k]);
                }
                indexQw += (n + 3u) / 4u;
            }
        }
        if (!batch.indices.empty())
            ps2native::submit(std::move(batch));
        return true;
    }

    // --- Trace -------------------------------------------------------------------------------------------

    void traceFlushToken(State &s)
    {
        if (s.traceRun == 0)
            return;
        char buf[16];
        s.traceLine += s.traceToken;
        if (s.traceRun > 1)
        {
            std::snprintf(buf, sizeof(buf), "x%u", s.traceRun);
            s.traceLine += buf;
        }
        s.traceLine += ' ';
        s.traceRun = 0;
    }

    void traceToken(State &s, const std::string &token)
    {
        std::lock_guard<std::mutex> lock(s.traceMutex);
        const uint64_t tick = s.runtime->eeScheduler().currentVSyncTick();
        if (tick != s.traceTick)
        {
            traceFlushToken(s);
            if (!s.traceLine.empty())
                std::fprintf(stderr, "[native-trace] vsync %llu: %s\n", static_cast<unsigned long long>(s.traceTick), s.traceLine.c_str());
            s.traceLine.clear();
            s.traceTick = tick;
        }
        if (tick < s.traceBegin || tick >= s.traceEnd)
            return;
        if (token == s.traceToken)
        {
            ++s.traceRun;
            return;
        }
        traceFlushToken(s);
        s.traceToken = token;
        s.traceRun = 1;
    }

    uint32_t currentFbp(State &s, unsigned ctx)
    {
        GSRawDrawRegisters regs;
        if (!s.runtime->gs().readRawDrawRegisters(regs))
            return 0xfffu;
        return uint32_t(regs.frame[ctx & 1u] & 0x1ffu);
    }

    void traceHistogram(State &s, const std::string &key)
    {
        ++s.histogram[key];
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - s.histogramStart).count();
        if (elapsed < 5.0)
            return;
        std::string line;
        char buf[96];
        for (const auto &entry : s.histogram)
        {
            std::snprintf(buf, sizeof(buf), " %s:%.0f/s", entry.first.c_str(), entry.second / elapsed);
            line += buf;
        }
        const ps2native::Stats stats = ps2native::takeStats();
        std::fprintf(stderr, "[native-programs]%s\n[native-stats] submitted %.0f batches/s %.0f tris/s %.0f frames/s\n", line.c_str(),
                     stats.batches / elapsed, stats.triangles / elapsed, stats.frames / elapsed);
        s.histogram.clear();
        s.histogramStart = now;
    }

    // The previous batch's input buffer still holds the colours the VU wrote (floats at +121) until the batch
    // after next reuses it. Batches the VU culled as a whole keep their integer input colours; skip those.
    void checkPending(State &s, const uint8_t *vu)
    {
        s.havePending = false;
        for (uint32_t i = 0; i < s.pending.count; ++i)
        {
            uint32_t raw[4];
            std::memcpy(raw, qw(vu, s.pending.base + kInColors + i), 16);
            if (raw[0] < 256u && raw[1] < 256u && raw[2] < 256u && raw[3] < 256u)
            {
                ++s.skipped;
                continue;
            }
            float vuColor[4];
            std::memcpy(vuColor, raw, 16);
            float worst = 0.0f;
            for (int c = 0; c < 4; ++c)
                worst = std::max(worst, std::fabs(vuColor[c] - s.pending.colors[i][c]));
            ++s.verified;
            if (worst > 1.0f)
            {
                if (s.mismatched < 8)
                    std::fprintf(stderr, "[native-verify] %s/%s/%s vertex %u: native (%.1f %.1f %.1f %.1f) vu (%.1f %.1f %.1f %.1f)\n",
                                 programName(s, 0), programName(s, 1), programName(s, 2), i, s.pending.colors[i][0],
                                 s.pending.colors[i][1], s.pending.colors[i][2], s.pending.colors[i][3], vuColor[0], vuColor[1],
                                 vuColor[2], vuColor[3]);
                ++s.mismatched;
            }
            s.maxError = std::max(s.maxError, worst);
            // The VU overwrote the input positions (+81) with window coordinates.
            float vuPos[4];
            std::memcpy(vuPos, qw(vu, s.pending.base + kInPositions + i), 16);
            // (IDXSTRPD keeps other data there, so only IDXTRID batches are compared; vertices behind the camera
            // (VU depth <= 0) are skipped, since near w = 0 the divide amplifies rounding and the GPU clips them.)
            const bool comparable = std::strncmp(s.pending.programs, "IDXTRID/", 8) == 0 && vuPos[2] > 0.0f;
            const float posError = !comparable ? 0.0f : std::max(std::fabs(vuPos[0] - s.pending.screen[i][0]), std::fabs(vuPos[1] - s.pending.screen[i][1]));
            if (posError > 2.0f) // GS 12.4 units: 1/8 pixel
            {
                if (s.posMismatched < 10)
                    std::fprintf(stderr, "[native-verify] pos %s fresh %d variant %d vertex %u/%u: native (%.1f %.1f) vu (%.1f %.1f %.1f %.1f)\n",
                                 s.pending.programs, s.pending.fresh ? 1 : 0, s.pending.mainVariant, i, s.pending.count, s.pending.screen[i][0],
                                 s.pending.screen[i][1], vuPos[0], vuPos[1], vuPos[2], vuPos[3]);
                ++s.posMismatched;
            }
            s.maxPosError = std::max(s.maxPosError, posError);
            // ST scratch (qw368+, shared by both input buffers) still holds this batch's (s, t).
            float vuSt[4];
            std::memcpy(vuSt, qw(vu, 368u + i), 16);
            const float stError = std::max(std::fabs(vuSt[0] - s.pending.st[i][0]), std::fabs(vuSt[1] - s.pending.st[i][1]));
            if (stError > 1e-3f)
            {
                if (s.stMismatched < 12)
                    std::fprintf(stderr, "[native-verify] st %s variant %d flags 0x%x vertex %u: native (%g %g) vu (%g %g) input (%d %d %d %d) 301.x 0x%08x\n",
                                 s.pending.programs, s.pending.mainVariant, s.pending.flags, i, s.pending.st[i][0],
                                 s.pending.st[i][1], vuSt[0], vuSt[1], s.pending.stInput[i][0], s.pending.stInput[i][1],
                                 s.pending.stInput[i][2], s.pending.stInput[i][3], readU32(qw(vu, kQwGifTag)));
                if (s.stMismatched < 3)
                {
                    float now[8];
                    std::memcpy(now, qw(vu, kQwUvMatrix), 32);
                    std::fprintf(stderr, "[native-verify]   uv at decode (%g %g %g %g | %g %g %g %g) now (%g %g %g %g | %g %g %g %g) raw st0 (%g %g)\n",
                                 s.pending.uv[0], s.pending.uv[1], s.pending.uv[2], s.pending.uv[3], s.pending.uv[4], s.pending.uv[5],
                                 s.pending.uv[6], s.pending.uv[7], now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7],
                                 s.pending.rawSt[0], s.pending.rawSt[1]);
                }
                ++s.stMismatched;
            }
            s.maxStError = std::max(s.maxStError, stError);
        }
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last).count() >= 5.0)
        {
            std::fprintf(stderr, "[native-verify] vertices %llu, mismatched %llu, culled %llu, max error %.2f, hemisphere-lit batches %llu, uv-transform batches %llu (reflection texgen %llu), st mismatched %llu, max st error %.4f, position mismatched %llu, max position error %.2f\n",
                         (unsigned long long)s.verified, (unsigned long long)s.mismatched, (unsigned long long)s.skipped,
                         s.maxError, (unsigned long long)s.unsupportedLight, (unsigned long long)s.uvTransform,
                         (unsigned long long)s.unsupportedTexgen, (unsigned long long)s.stMismatched, s.maxStError,
                         (unsigned long long)s.posMismatched, s.maxPosError);
            last = now;
        }
    }

    // --- Hooks -------------------------------------------------------------------------------------------

    bool onVu1(const PS2Runtime::Vu1HookEvent &event)
    {
        State &s = g_state;
        if (!s.scanned)
            scanPrograms(s);
        identifyPrograms(s);

        const uint8_t *vu = s.runtime->memory().getVU1Data();
        if (s.havePending)
            checkPending(s, vu);
        bool freshCall = !event.resume;
        if (!event.resume)
            s.afterInit = event.startPC == 0u;
        else if (s.afterInit)
        {
            freshCall = true;
            s.afterInit = false;
        }
        if (!event.resume && event.startPC == 0u)
            return false; // init entry: only resets the output double buffer

        if (s.trace)
        {
            uint64_t tag[2];
            std::memcpy(tag, qw(vu, kQwGifTag), 16);
            const unsigned ctx = unsigned((tag[0] >> 56) & 1u);
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s/%s/%s", programName(s, 0), programName(s, 1), programName(s, 2));
            traceHistogram(s, buf);
            std::snprintf(buf, sizeof(buf), "V:%s/%s@%x", programName(s, 0), programName(s, 1), currentFbp(s, ctx));
            traceToken(s, buf);
        }

        if (ps2native::enabled())
        {
            const MainKind kind = mainKind(s);
            bool dequantize = false;
            int normals = 0;
            if (kind != MainKind::Other && supportedXform(s, dequantize, normals))
            {
                const char *xform = programName(s, 1);
                const bool prologue = std::strcmp(xform, "XFORM1M") == 0 || std::strcmp(xform, "XFORM1MNOD") == 0 ||
                                      std::strcmp(xform, "XFORM2M") == 0 || std::strcmp(xform, "XFORM3M") == 0;
                captureMatrix(s, s.runtime->memory().getVU1Data(), freshCall, prologue, s.writeBack);
                s.freshCall = freshCall;
                if (s.haveMatrix && decodeBatch(s, vu, event.itop, kind, dequantize, normals))
                    return ps2native::replacesVu1() && !s.verify;
            }
        }
        return false; // the interpreter still runs (dual-run until the emulated 3D is dropped)
    }

    void onGifPacket(uint32_t path, const uint8_t *data, uint32_t size)
    {
        State &s = g_state;
        // Full native frame: everything else drawn into the frame (including VU1 output of programs without a
        // native port) is replayed in stream order.
        ps2native::replayGif(path, data, size);
        if (path == 1u || size < 16u)
            return; // PATH1 is the VU1 output (no transfers), already traced as V: tokens
        if (ps2native::enabled())
            ps2native::observeGif(path, data, size);
        uint64_t tagLo;
        std::memcpy(&tagLo, data, 8);
        const uint32_t flg = uint32_t((tagLo >> 58) & 3u);
        char buf[64];
        if (flg == 2u)
        {
            if (s.trace)
            {
                std::snprintf(buf, sizeof(buf), "I%u", path);
                traceToken(s, buf);
            }
            return;
        }
        // Summarise what the packet does: FRAME_1/2 writes (F<fbp>), vertex kicks (D<count>).
        uint32_t frameWrite = ~0u, kicks = 0;
        for (uint32_t off = 0; off + 16u <= size;)
        {
            uint64_t tag[2];
            std::memcpy(tag, data + off, 16);
            off += 16u;
            const uint32_t nloop = uint32_t(tag[0] & 0x7fffu);
            const uint32_t fmt = uint32_t((tag[0] >> 58) & 3u);
            const uint32_t nreg = ((tag[0] >> 60) & 15u) ? uint32_t((tag[0] >> 60) & 15u) : 16u;
            if (fmt == 2u || fmt == 3u)
                break;
            for (uint32_t loop = 0; loop < nloop; ++loop)
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    const uint32_t desc = uint32_t((tag[1] >> (r * 4u)) & 15u);
                    if (fmt == 0u)
                    {
                        if (off + 16u > size)
                            break;
                        uint64_t q[2];
                        std::memcpy(q, data + off, 16);
                        off += 16u;
                        if (desc == 0xeu)
                        {
                            const uint32_t addr = uint32_t(q[1] & 0xffu);
                            if (addr == 0x4cu || addr == 0x4du)
                                frameWrite = uint32_t(q[0] & 0x1ffu);
                            if (addr == 0x05u || addr == 0x04u)
                                ++kicks;
                        }
                        else if (desc == 0x4u || desc == 0x5u)
                            ++kicks;
                    }
                    else
                    {
                        if (desc == 0x4u || desc == 0x5u)
                            ++kicks;
                        off += 8u;
                    }
                }
            if (fmt == 1u)
                off = (off + 15u) & ~15u;
            if (tag[0] & 0x8000u)
                break;
        }
        if (kicks && ps2native::enabled())
            ps2native::noteGsDraw(frameWrite != ~0u ? frameWrite : currentFbp(s, 0));
        if (!s.trace)
            return;
        int n = std::snprintf(buf, sizeof(buf), "G%u@%x", path, currentFbp(s, 0));
        if (frameWrite != ~0u)
            n += std::snprintf(buf + n, sizeof(buf) - n, "F%x", frameWrite);
        if (kicks)
            std::snprintf(buf + n, sizeof(buf) - n, "D%u", kicks);
        traceToken(s, buf);
    }

    bool readGsRegisters(GSRawDrawRegisters &out)
    {
        return g_state.runtime && g_state.runtime->gs().readRawDrawRegisters(out);
    }

    void applyNativeRenderer(PS2Runtime &runtime)
    {
        State &s = g_state;
        s.runtime = &runtime;
        if (const char *trace = std::getenv("PS2X_NATIVE_TRACE"))
        {
            s.trace = true;
            s.traceBegin = std::strtoull(trace, nullptr, 10);
            const char *colon = std::strchr(trace, ':');
            s.traceEnd = s.traceBegin + (colon ? std::strtoull(colon + 1, nullptr, 10) : 3ull);
        }
        s.verify = std::getenv("PS2X_NATIVE_VERIFY") != nullptr && ps2native::enabled();
        s.writeBack = ps2native::replacesVu1() && !s.verify;
        if (!s.trace && !ps2native::enabled())
            return;
        runtime.setVu1Hook(&onVu1);
        runtime.setGifPacketObserver(&onGifPacket);
        ps2native::setRegisterSource(&readGsRegisters);
        std::fprintf(stderr, "[native] VU1 hook installed (trace=%d, mode=%d)\n", s.trace ? 1 : 0, int(ps2native::mode()));
    }
}

PS2_REGISTER_GAME_OVERRIDE("Billy & Mandy: native renderer front end (PS2X_NATIVE3D / PS2X_NATIVE_TRACE)", "SLUS_214.51", 0u, 0u, &applyNativeRenderer)
