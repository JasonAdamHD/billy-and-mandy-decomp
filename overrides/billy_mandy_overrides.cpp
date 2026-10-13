// Game-specific overrides for The Grim Adventures of Billy & Mandy (SLUS_214.51).
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

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

    void applyBillyMandyOverrides(PS2Runtime &runtime)
    {
        if (std::getenv("PS2X_PLAY_FMV") == nullptr)
            runtime.replaceFunction(kRunPlaybackLoop, &skipVideoPlayback);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Billy & Mandy: skip Sofdec FMVs", "SLUS_214.51", 0u, 0u, &applyBillyMandyOverrides)
