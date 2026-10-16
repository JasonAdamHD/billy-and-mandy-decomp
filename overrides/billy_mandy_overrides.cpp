// Game-specific overrides for The Grim Adventures of Billy & Mandy (SLUS_214.51).
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_pad.h"

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

    // Widescreen (PS2X_WIDESCREEN=1): AtGrBuildPerspectiveFrustumMatrix(near f12, far f13, out a0, window a1 =
    // {left, top, right, bottom}, screen a2 = {sx, sy}, offset a3 = {ox, oy}) builds every perspective projection.
    // Narrowing its x scale by 3/4 widens the horizontal field of view to 16:9; the game's own frustum culling
    // uses the same matrix, so the extra width is drawn and kept. The runtime then presents the frame at 16:9.
    constexpr uint32_t kBuildPerspectiveFrustumMatrix = 0x00437F30u;
    PS2Runtime::RecompiledFunction g_originalBuildPerspective = nullptr;

    float readGuestFloat(const uint8_t *rdram, uint32_t address)
    {
        const uint32_t bits = readGuest32(rdram, address);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    void buildPerspectiveWidescreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t out = GPR_U32(ctx, 4), window = GPR_U32(ctx, 5), screen = GPR_U32(ctx, 6), offset = GPR_U32(ctx, 7);
        g_originalBuildPerspective(rdram, ctx, runtime);
        static const bool log = std::getenv("PS2X_WIDESCREEN_LOG") != nullptr;
        if (log)
        {
            static uint32_t logged = 0u;
            if (logged++ < 40u)
                std::cerr << "[widescreen] window (" << readGuestFloat(rdram, window) << "," << readGuestFloat(rdram, window + 4u) << ")-("
                          << readGuestFloat(rdram, window + 8u) << "," << readGuestFloat(rdram, window + 12u) << ") screen ("
                          << readGuestFloat(rdram, screen) << "," << readGuestFloat(rdram, screen + 4u) << ") offset ("
                          << readGuestFloat(rdram, offset) << "," << readGuestFloat(rdram, offset + 4u) << ") ra 0x" << std::hex
                          << GPR_U32(ctx, 31) << std::dec << std::endl;
        }
        const uint32_t m0 = out & 0x01FFFFFFu, m8 = (out + 32u) & 0x01FFFFFFu;
        if (m8 + sizeof(float) > PS2_RAM_SIZE)
            return;
        const float ox = readGuestFloat(rdram, offset);
        float scaleX, shearX;
        std::memcpy(&scaleX, rdram + m0, sizeof(float));
        std::memcpy(&shearX, rdram + m8, sizeof(float));
        scaleX *= 0.75f;
        shearX = ox + (shearX - ox) * 0.75f;
        std::memcpy(rdram + m0, &scaleX, sizeof(float));
        std::memcpy(rdram + m8, &shearX, sizeof(float));
    }

    // Multitap: the game asks mtapman (IOP) whether a multitap is plugged in and, if so, reads players 1-4 from
    // its slots. The runtime emulates one in pad port 1 (local players 2-4 on gamepads 2-4, and netplay players).
    constexpr uint32_t kMtapPortOpen = 0x0011F610u;      // sceMtapPortOpen(port)
    constexpr uint32_t kMtapGetConnection = 0x0011F7D0u; // sceMtapGetConnection(port): 1 = connected

    void mtapPortOpen(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        SET_GPR_U32(ctx, 2, 1u);
        ctx->pc = GPR_U32(ctx, 31);
    }

    void mtapGetConnection(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        SET_GPR_U32(ctx, 2, ps2PadMultitapConnected(static_cast<int>(GPR_U32(ctx, 4))) ? 1u : 0u);
        ctx->pc = GPR_U32(ctx, 31);
    }

    void applyBillyMandyOverrides(PS2Runtime &runtime)
    {
        runtime.replaceFunction(kMtapPortOpen, &mtapPortOpen);
        runtime.replaceFunction(kMtapGetConnection, &mtapGetConnection);
        if (const char *wide = std::getenv("PS2X_WIDESCREEN"); wide && std::atoi(wide) != 0)
        {
            g_originalBuildPerspective = runtime.lookupFunction(kBuildPerspectiveFrustumMatrix);
            if (g_originalBuildPerspective != nullptr)
                runtime.replaceFunction(kBuildPerspectiveFrustumMatrix, &buildPerspectiveWidescreen);
        }
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

PS2_REGISTER_GAME_OVERRIDE("Billy & Mandy: skip Sofdec FMVs, guard Flash level walk and stuck sound loads", "SLUS_214.51", 0u, 0u, &applyBillyMandyOverrides)
