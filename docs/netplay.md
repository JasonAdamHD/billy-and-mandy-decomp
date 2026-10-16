# Online play (netplay)

Two to four players, each with their own copy of the game and the same build, play together over the internet. Every PC runs the whole game; only controller inputs travel. Implementation:
`PS2Recomp/ps2xRuntime/src/lib/ps2_netplay.cpp` (header `runtime/ps2_netplay.h`).

## Playing

```powershell
# Host (player 1). Allow inbound UDP 7777 (router port forward when playing over the internet).
powershell -ExecutionPolicy Bypass -File .\play_native.ps1 -HostGame
# Friend (player 2)
powershell -ExecutionPolicy Bypass -File .\play_native.ps1 -Join <host IP>
```

Options: `-Players 2..4` (host; default 2), `-Port` (default 7777) and `-Delay` (input delay in frames). The
delay is normally automatic: once everyone has joined, the host pings each player for half a second and picks it.
Friends join in any order; the first to join is player 2.
The host's window waits until the friend has joined; the game then starts on both PCs together.

Both players use a temporary copy of the **host's** memory card, which is sent to the friend at the start, so the
unlocks and settings are the host's. Nobody's real save is written during netplay.

## How it works

- **Lockstep.** Pad inputs are latched once per emulated vsync. A player's input for frame T is sampled at frame
  T - delay and sent to the others, so it has usually arrived by the time frame T runs. If not, the frame waits.
  The host relays packets between players (star topology). Each packet repeats the sender's last 16 frames, so
  lost packets are covered without acknowledgements. A network thread receives, relays and answers packets as
  they arrive.
- **Automatic input delay.** An input from one client reaches another through the host, so the slowest path
  is the two largest one-way latencies. The host measures each client's round trip (90th percentile, to cover
  jitter) and picks `ceil((path + 4 ms) / frame) + 1` frames.
- **Desync detection.** Every 60 frames each PC hashes the 32 MB of EE RAM and sends the hash. A mismatch is
  logged as `[netplay] DESYNC`.
- **Players and pads.** Player N is slot N of an emulated multitap in pad port 1 (the game supports 4 players
  through the multitap on a real PS2).
- **Session start.** Clients say hello, and the host assigns player numbers. Clients download the host's memory card
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
fight). Two and three local instances connected over UDP stayed in sync for whole sessions (3-player fight
included). Three instances on one PC run below full speed, because each runs the full game; separate PCs don't
share that load.

## Network tests

The tests ran 4 instances on one PC (host plus 3 clients) with simulated links (`PS2X_NETPLAY_SIM_PING`,
`_JITTER`, `_LOSS`: each client delays its sent and received packets). Each run lasted about 4 minutes, from boot
through menus into a 4-player fight. The stats lines (`[netplay] stats tick ...`) log the fps, the frames that
waited for input and the measured ping every 10 s.

| Clients' pings | Jitter / loss | Delay | Speed | Waits | Desyncs |
|---|---|---|---|---|---|
| 0 / 0 / 0 ms | none | 3 | ~58 fps | clients ~5 ms on 1 frame in 4 (alignment) | 0 |
| 30 / 60 / 80 ms | none | 3 (fixed, before the network thread) | **~39 fps** | most frames, up to 85 ms | 0 |
| 30 / 60 / 80 ms | none | 6 (auto) | ~59 fps | rare, mostly < 10 ms | 0 |
| 30 / 60 / 80 ms | +/-10 ms, 2% | 6 (auto, 90th percentile) | ~58 fps | 3-8% of frames, ~7 ms | 0 |

Too small a delay slows every player to the pace of the slowest path. With the automatic delay, the cost of
high pings is input lag (6 frames = 100 ms at 30/60/80 ms) instead of slowdown. Lowering that lag needs rollback.

## Local multiplayer (one PC)

The same multitap gives up to 4 players on one PC. Player 1 is the keyboard or first gamepad, and players 2-4 are
gamepads 2-4. The game answers `sceMtapGetConnection` from `overrides/billy_mandy_overrides.cpp`, and the pad
stubs report one slot per connected gamepad. `PS2X_MULTITAP=0` turns the multitap off (players 1-2 on pad ports
1-2, as before).

## Testing tools

- `PS2X_INPUT_RECORD=<file>` records player 1's input per frame; `PS2X_INPUT_REPLAY=<file>` plays it back.
- `PS2X_FRAMEHASH=<frames>` logs a hash of EE RAM every N frames.
- `PS2X_FRAMEDUMP=<frame>[,<frame>...]` writes `ram_<frame>.bin` (32 MB).

To check determinism, replay one recording twice and compare the logged hashes. Then diff the RAM dumps at the
first differing frame and find the code that writes the differing addresses.

## Limits / next steps

- Delay-based only. Rollback would need save states of the whole runtime (EE RAM and registers, IOP, SPU2,
  GS/VRAM, scheduler).
- No relay server or NAT traversal: the host must accept inbound UDP.
- The delay is chosen once, at session start; it does not adapt if a connection gets worse during play.
- A player who drops out has an idle pad from then on.
