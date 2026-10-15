// Game-specific overrides for The Grim Adventures of Billy & Mandy (SLUS_214.51).
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>

namespace
{
    constexpr uint32_t kRunPlaybackLoop = 0x00217960u; // PS2VideoPlayer::vRunPlaybackLoop(this, skipCallback)
    constexpr uint32_t kPlayingFlagOffset = 0x13Cu;     // PS2VideoPlayer::m_bPlaying, cleared when the loop ends

    // FMVs are CRI Sofdec streams decoded on the IPU, which the runtime does not emulate yet. Report the
    // movie as finished straight away, exactly as if the player had pressed a button to skip it.
    void skipVideoPlayback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t player = GPR_U32(ctx, 4);
        const uint32_t flag = (player + kPlayingFlagOffset) & 0x01FFFFFFu;
        if (flag + sizeof(uint32_t) <= PS2_RAM_SIZE)
        {
            const uint32_t zero = 0u;
            std::memcpy(rdram + flag, &zero, sizeof(zero));
        }
        static bool logged = false;
        if (!logged)
        {
            std::cerr << "[billy_mandy] skipping FMV playback (set PS2X_PLAY_FMV=1 to attempt it)" << std::endl;
            logged = true;
        }
    }

    // AtFpDisplayManager::vAdvanceFrames (0x4ffd30) walks its active Flash levels (list head at manager+8, link at
    // level+0) in s2 and calls AtFpActionScript::vRunPendingLoadsAndDispatch (0x50e650) on each. Occasionally the
    // walk reaches level+0x10 instead of a level, and the dispatcher then calls through garbage. Until the cause
    // is found, check the argument against the manager's level pool (base at manager+4, 0x5b0-byte slots, count
    // at manager+0x21c), log what the list looked like, and end the walk for this frame instead of crashing.
    constexpr uint32_t kRunPendingLoadsAndDispatch = 0x0050E650u;
    constexpr uint32_t kAdvanceFramesReturn = 0x004FFD94u;
    constexpr uint32_t kLevelSize = 0x5B0u;
    PS2Runtime::RecompiledFunction g_originalRunPending = nullptr;
    uint32_t g_zeroWord = 0u;

    uint32_t readGuest32(const uint8_t *rdram, uint32_t address)
    {
        uint32_t value = 0u;
        const uint32_t physical = address & 0x01FFFFFFu;
        if (physical + sizeof(value) <= PS2_RAM_SIZE)
            std::memcpy(&value, rdram + physical, sizeof(value));
        return value;
    }

    void runPendingLoadsChecked(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t level = GPR_U32(ctx, 4);
        if (GPR_U32(ctx, 31) == kAdvanceFramesReturn)
        {
            const uint32_t manager = GPR_U32(ctx, 19); // s3 in vAdvanceFrames
            const uint32_t pool = readGuest32(rdram, manager + 4u);
            const uint32_t count = readGuest32(rdram, manager + 0x21Cu);
            const bool valid = level >= pool && level < pool + count * kLevelSize && ((level - pool) % kLevelSize) == 0u;
            if (!valid)
            {
                static uint32_t reports = 0u;
                if (reports++ < 20u)
                {
                    std::cerr << std::hex << "[billy_mandy] Flash level walk reached 0x" << level << " (pool 0x" << pool
                              << " x" << count << ", manager 0x" << manager << ") list:";
                    uint32_t node = readGuest32(rdram, manager + 8u);
                    for (int i = 0; i < 12 && node != 0u; ++i, node = readGuest32(rdram, node))
                        std::cerr << " 0x" << node;
                    std::cerr << " | pending-destroy:";
                    node = readGuest32(rdram, manager + 0x18u);
                    for (int i = 0; i < 12 && node != 0u; ++i, node = readGuest32(rdram, node))
                        std::cerr << " 0x" << node;
                    std::cerr << std::dec << std::endl;
                }
                // vAdvanceFrames continues with s2 = *s2; point s2 at a zero word so this frame's walk ends.
                if (g_zeroWord == 0u)
                    g_zeroWord = runtime->guestMalloc(16u, 16u);
                if (g_zeroWord != 0u)
                {
                    const uint32_t zero = 0u;
                    std::memcpy(rdram + (g_zeroWord & 0x01FFFFFFu), &zero, sizeof(zero));
                    SET_GPR_U32(ctx, 18, g_zeroWord);
                }
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
        }
        g_originalRunPending(rdram, ctx, runtime);
    }

    // GmSoSoundManager::vStartSoundIfNotPlaying (0x2070e0) asks the sound device for a sound and then spins
    //     do { soundDevice->vUpdateChannelsAndRpc(0.1f); } while (request->status == 0);
    // with the request at its sp+0x3c. The status is driven by the IOP sound driver, which watches SPU2 voice
    // state the runtime does not emulate, and occasionally it never leaves 0 - the load before a match then
    // hangs forever. When the same request has been pending for 5 seconds, mark it failed (2): the game then
    // releases it and continues without that one sound.
    constexpr uint32_t kSoundUpdateA = 0x004BA610u;
    constexpr uint32_t kSoundUpdateB = 0x004BA650u;
    constexpr uint32_t kStartSoundWaitReturn = 0x00207220u;
    PS2Runtime::RecompiledFunction g_originalSoundUpdateA = nullptr;
    PS2Runtime::RecompiledFunction g_originalSoundUpdateB = nullptr;

    void checkStuckSoundRequest(uint8_t *rdram, R5900Context *ctx)
    {
        static uint32_t pendingRequest = 0u;
        static std::chrono::steady_clock::time_point pendingSince{};
        if (GPR_U32(ctx, 31) != kStartSoundWaitReturn)
            return;
        const uint32_t request = readGuest32(rdram, GPR_U32(ctx, 29) + 0x3Cu);
        if (request == 0u)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (request != pendingRequest)
        {
            pendingRequest = request;
            pendingSince = now;
            return;
        }
        if (now - pendingSince < std::chrono::seconds(5))
            return;
        const uint32_t statusAddress = (request + 0xCu) & 0x01FFFFFFu;
        if (rdram[statusAddress] == 0u)
        {
            rdram[statusAddress] = 2u;
            std::cerr << "[billy_mandy] sound request 0x" << std::hex << request << std::dec
                      << " never became ready; skipping it so loading can continue" << std::endl;
        }
        pendingRequest = 0u;
    }

    void soundUpdateA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        checkStuckSoundRequest(rdram, ctx);
        g_originalSoundUpdateA(rdram, ctx, runtime);
    }

    void soundUpdateB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        checkStuckSoundRequest(rdram, ctx);
        g_originalSoundUpdateB(rdram, ctx, runtime);
    }

    // The vector-normalise helpers detect a zero-length input through the VU0 MAC flag Zx (CFC2 $17 & 8, set by
    // the VADDq.x that copies sqrt(length^2)) and then return a fixed unit vector instead of the scaled input.
    // The recompiler does not update MAC flags for VU0 macro instructions; CFC2 $17 returns whatever the last
    // VU0 microprogram run (VCALLMS, interpreted) left behind. With a stale Zx the helpers returned (1,0,0) and
    // length 0 for ordinary vectors, which flipped the path camera between stages every few frames. Set the flags
    // the VADDq.x would produce before the helper runs: Zx exactly when the length is zero (the field is x only,
    // and a square root is never negative, so no other bit is set).
    struct NormalizeHelper
    {
        uint32_t address;
        int srcReg; // GPR holding the source vector (4 = a0, 5 = a1)
        bool fourD; // length over xyzw (the *VU0 variants) instead of xyz
        PS2Runtime::RecompiledFunction original;
    };
    NormalizeHelper g_normalizeHelpers[] = {
        {0x003BF360u, 5, true, nullptr},  // NormalizeVector4VU0(dst, src)
        {0x003BF3B0u, 4, true, nullptr},  // NormalizeVector4InPlaceVU0(v)
        {0x003C01D0u, 5, false, nullptr}, // NormalizeVector4ToDestReturnLength(dst, src)
        {0x003C0230u, 4, false, nullptr}, // NormalizeVector4InPlaceReturnLength(v)
        {0x003C0290u, 5, false, nullptr}, // NormalizeVector4ToDest(dst, src)
        {0x003C02E0u, 4, false, nullptr}, // NormalizeVector4InPlace(v)
    };

    template <size_t I>
    void normalizeWithMacFlags(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const NormalizeHelper &h = g_normalizeHelpers[I];
        const uint32_t src = GPR_U32(ctx, h.srcReg) & 0x01FFFFFFu;
        float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (src + sizeof(v) <= PS2_RAM_SIZE)
            std::memcpy(v, rdram + src, sizeof(v));
        // length^2 as the VU sums it; the VU flushes results below the normal range to zero.
        float lengthSquared = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (h.fourD)
            lengthSquared += v[3] * v[3];
        ctx->vu0_mac_flags = lengthSquared < 1.17549435e-38f ? 0x8u : 0u;
        h.original(rdram, ctx, runtime);
    }

    template <size_t... I>
    void installNormalizeChecks(PS2Runtime &runtime, std::index_sequence<I...>)
    {
        const PS2Runtime::RecompiledFunction wrappers[] = {&normalizeWithMacFlags<I>...};
        for (size_t i = 0; i < sizeof...(I); ++i)
        {
            g_normalizeHelpers[i].original = runtime.lookupFunction(g_normalizeHelpers[i].address);
            if (g_normalizeHelpers[i].original != nullptr)
                runtime.replaceFunction(g_normalizeHelpers[i].address, wrappers[i]);
        }
    }

    void applyBillyMandyOverrides(PS2Runtime &runtime)
    {
        if (std::getenv("PS2X_NO_NORMALIZE_MAC_FIX") == nullptr)
            installNormalizeChecks(runtime, std::make_index_sequence<sizeof(g_normalizeHelpers) / sizeof(g_normalizeHelpers[0])>{});
        if (std::getenv("PS2X_NO_SOUND_WAIT_GUARD") == nullptr)
        {
            g_originalSoundUpdateA = runtime.lookupFunction(kSoundUpdateA);
            g_originalSoundUpdateB = runtime.lookupFunction(kSoundUpdateB);
            if (g_originalSoundUpdateA && g_originalSoundUpdateB)
            {
                runtime.replaceFunction(kSoundUpdateA, &soundUpdateA);
                runtime.replaceFunction(kSoundUpdateB, &soundUpdateB);
            }
        }
        if (std::getenv("PS2X_NO_FLASH_GUARD") == nullptr)
        {
            g_originalRunPending = runtime.lookupFunction(kRunPendingLoadsAndDispatch);
            if (g_originalRunPending != nullptr)
                runtime.replaceFunction(kRunPendingLoadsAndDispatch, &runPendingLoadsChecked);
        }
        if (std::getenv("PS2X_PLAY_FMV") == nullptr)
            runtime.replaceFunction(kRunPlaybackLoop, &skipVideoPlayback);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Billy & Mandy: skip Sofdec FMVs, guard Flash level walk and stuck sound loads, normalise MAC flags", "SLUS_214.51", 0u, 0u, &applyBillyMandyOverrides)
