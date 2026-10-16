# Online play (netplay)

Two players, each with their own copy of the game and the same build, play the Vs/Mission modes together over
the internet. Every PC runs the whole game; only controller inputs travel. Implementation:
`PS2Recomp/ps2xRuntime/src/lib/ps2_netplay.cpp` (header `runtime/ps2_netplay.h`).

## Playing

```powershell
# Host (player 1). Allow inbound UDP 7777 (router port forward when playing over the internet).
powershell -ExecutionPolicy Bypass -File .\play_native.ps1 -HostGame
# Friend (player 2)
powershell -ExecutionPolicy Bypass -File .\play_native.ps1 -Join <host IP>
```

Options: `-Port` (default 7777) and `-Delay` (input delay in frames, default 3; raise it on a slow connection).
The host's window waits until the friend has joined; the game then starts on both PCs together.

Both players use a temporary copy of the **host's** memory card, which is sent to the friend at the start, so the
unlocks and settings are the host's. Nobody's real save is written during netplay.

## How it works

- **Lockstep.** Pad inputs are latched once per emulated vsync. A player's input for frame T is sampled at frame
  T - delay and sent to the others, so it has usually arrived by the time frame T runs. If not, the frame waits.
  The host relays packets between players (star topology). Each packet repeats the sender's last 16 frames, so
  lost packets are covered without acknowledgements.
- **Desync detection.** Every 60 frames each PC hashes the 32 MB of EE RAM and sends the hash. A mismatch is
  logged as `[netplay] DESYNC`.
- **Session start.** Clients say hello, and the host assigns pad ports. Clients download the host's memory card
  (1 KB chunks), and the host starts everyone at frame 1.

## What makes the game deterministic

Identical inputs must give identical games on different PCs. "Deterministic mode" (netplay, or input
record/replay) changes the runtime as follows:

| Source of divergence | Fix |
|---|---|
| Scheduler events waited for both a cycle and a host-clock deadline, and the guest kept running while waiting | Events fire on guest cycles only; host pacing just holds the EE at vblank (`EeScheduler::processDueDeadlines`) |
| Idle skip chose the next deadline by wall clock | Chosen by cycles (`EeScheduler::waitForEvent`) |
| Real-time catch-up injected EE cycles when the host ran behind | Disabled |
| COP0 Count followed the host clock (the game seeds tables from it) | Guest EE cycles |
| `sceCdReadClock` and memory card file times used the real date | Fixed date + emulated time; fixed card times |
| GIF/VIF1 DMA completion on the graphics worker thread | Graphics work stays on the EE thread |

Two replays of the same input recording produce identical RAM for 12,500+ frames (boot, menus, loading and a
fight). Two local instances connected over UDP stayed in sync for a whole session.

## Testing tools

- `PS2X_INPUT_RECORD=<file>` records player 1's input per frame; `PS2X_INPUT_REPLAY=<file>` plays it back.
- `PS2X_FRAMEHASH=<frames>` logs a hash of EE RAM every N frames.
- `PS2X_FRAMEDUMP=<frame>[,<frame>...]` writes `ram_<frame>.bin` (32 MB).

To check determinism, replay one recording twice and compare the logged hashes. Then diff the RAM dumps at the
first differing frame and find the code that writes the differing addresses.

## Limits / next steps

- Two players: pad ports 3-4 need multitap emulation (`mtapman.irx`).
- Delay-based only. Rollback would need save states of the whole runtime (EE RAM and registers, IOP, SPU2,
  GS/VRAM, scheduler).
- No relay server or NAT traversal: the host must accept inbound UDP.
- A player who drops out has an idle pad from then on.
