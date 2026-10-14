// Native renderer front end for Billy & Mandy (SLUS_214.51): identifies the engine's VU1 microprograms, traces
// the frame's draw stream (M0) and decodes VU1 batches into ps2native::Batch for the host GPU (M1).
// See docs/native-renderer.md. Environment:
//   PS2X_NATIVE_TRACE=<vsync>[:<count>]  print the draw stream of <count> vsyncs (default 3) from <vsync>,
//                                        plus a per-program batch histogram every 5 s
//   PS2X_NATIVE3D=1|2                    decode supported batches and draw them natively (1 overlay, 2 native only)
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/native/native_renderer.h"

#include <algorithm>
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
    constexpr uint32_t kQwScreenOffset = 264u;
    constexpr uint32_t kQwScreenScale = 265u;
    constexpr uint32_t kQwGifTag = 301u;
    constexpr uint32_t kQwBones = 302u;
    // Input buffer layout relative to the buffer base (XITOP).
    constexpr uint32_t kInIndices = 1u;
    constexpr uint32_t kInPositions = 81u;
    constexpr uint32_t kInColors = 121u;
    constexpr uint32_t kInST = 161u;
    constexpr uint32_t kMaxBatchVertices = 64u;

    struct Program
    {
        std::string name;
        uint32_t slot = 0;              // instruction index of its first MPG chunk
        std::vector<uint8_t> signature; // first bytes of that chunk
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
        bool haveMatrix = false;
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
            // Walk the VIF packet to its first MPG.
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
                    const uint32_t bytes = std::min<uint32_t>(count * 8u, 256u);
                    Program p;
                    p.name.assign(name + 3, len - 3);
                    p.slot = code & 0xffffu;
                    p.signature.assign(ram + packet + pos, ram + packet + pos + bytes);
                    s.programs.push_back(std::move(p));
                    break;
                }
                if (cmd != 0x00u && cmd != 0x01u && cmd != 0x05u)
                    break;
            }
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
            for (size_t i = 0; i < s.programs.size(); ++i)
            {
                const Program &p = s.programs[i];
                if (p.slot == slots[k] && std::memcmp(code + p.slot * 8u, p.signature.data(), p.signature.size()) == 0)
                {
                    s.current[k] = int(i);
                    break;
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

    // Single-bone transforms only for now: 1M, 1MN (dequantised s16 positions) and 1MNOD, 1MNODN (float).
    bool singleBoneXform(const State &s, bool &dequantize)
    {
        const char *name = programName(s, 1);
        if (std::strncmp(name, "XFORM1M", 7) != 0 || std::strstr(name, "EXTN") != nullptr)
            return false;
        dequantize = std::strstr(name, "NOD") == nullptr;
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
    void captureMatrix(State &s, const uint8_t *vu)
    {
        float vp[16], bone[16];
        std::memcpy(vp, qw(vu, kQwViewProj), 64);
        std::memcpy(bone, qw(vu, kQwBones), 64);
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
            {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                    sum += vp[k * 4 + row] * bone[col * 4 + k];
                s.objectToClip[col * 4 + row] = sum;
            }
        s.haveMatrix = true;
    }

    uint32_t packColor(const int32_t c[4])
    {
        uint32_t out = 0;
        for (int i = 0; i < 4; ++i)
            out |= uint32_t(std::min(std::max(c[i], 0), 255)) << (i * 8);
        return out;
    }

    void decodeBatch(State &s, const uint8_t *vu, uint32_t base, MainKind kind, bool dequantize)
    {
        int32_t header[4];
        readInts(vu, base, header);
        float headerF[4];
        readVec(vu, base, headerF);
        const uint32_t vertexCount = uint32_t(header[1]);
        if (vertexCount == 0u || vertexCount > kMaxBatchVertices)
            return;

        ps2native::Batch batch;
        std::memcpy(batch.objectToClip, s.objectToClip, sizeof(batch.objectToClip));
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
            int32_t c[4];
            readInts(vu, base + kInColors + i, c);
            v.rgba = packColor(c);
            int32_t st[4];
            readInts(vu, base + kInST + i, st);
            v.st[0] = float(st[0]) / 4096.0f;
            v.st[1] = float(st[1]) / 4096.0f;
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

        if (kind == MainKind::IdxTri)
        {
            const uint32_t triangles = std::min<uint32_t>(uint32_t(header[0]), 80u);
            for (uint32_t t = 0; t < triangles; ++t)
            {
                int32_t idx[4];
                readInts(vu, base + kInIndices + t, idx);
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
                    addTriangle(list[k - 2], list[k - 1], list[k]);
                indexQw += (n + 3u) / 4u;
            }
        }
        if (!batch.indices.empty())
            ps2native::submit(std::move(batch));
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

    // --- Hooks -------------------------------------------------------------------------------------------

    bool onVu1(const PS2Runtime::Vu1HookEvent &event)
    {
        State &s = g_state;
        if (!s.scanned)
            scanPrograms(s);
        identifyPrograms(s);

        const uint8_t *vu = s.runtime->memory().getVU1Data();
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
            if (kind != MainKind::Other && singleBoneXform(s, dequantize))
            {
                if (freshCall)
                    captureMatrix(s, vu);
                if (s.haveMatrix)
                    decodeBatch(s, vu, event.itop, kind, dequantize);
            }
        }
        return false; // the interpreter still runs (dual-run until the emulated 3D is dropped)
    }

    void onGifPacket(uint32_t path, const uint8_t *data, uint32_t size)
    {
        State &s = g_state;
        if (path == 1u || size < 16u)
            return; // PATH1 is the VU1 output already traced as V: tokens
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
        if (!s.trace && !ps2native::enabled())
            return;
        runtime.setVu1Hook(&onVu1);
        runtime.setGifPacketObserver(&onGifPacket);
        std::fprintf(stderr, "[native] VU1 hook installed (trace=%d, mode=%d)\n", s.trace ? 1 : 0, int(ps2native::mode()));
    }
}

PS2_REGISTER_GAME_OVERRIDE("Billy & Mandy: native renderer front end (PS2X_NATIVE3D / PS2X_NATIVE_TRACE)", "SLUS_214.51", 0u, 0u, &applyNativeRenderer)
