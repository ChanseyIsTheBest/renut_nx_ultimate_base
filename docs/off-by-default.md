# Off by default: what is there, and why it is off

The base ships switches that are off by default although they help somewhere: a fix that one game needs but that
breaks another game's picture, a faster form that is not exact, a speed-up the console measured as slower or as no
gain, or a trial the emulator stalls with. This page sorts them by why they are off. Every table has a **Speed**
column: what is known about the switch's speed, and **Slower** where a console run measured it slower.

**The rule (the user, 2026-11-11):** a key that breaks the graphics of a specific game is OFF in the base by default.
The game that needs it sets it in its own toml (or CMake / environment, as below), after checking its own picture.

How to turn one on:
- a key: `name = true` in the game's toml (the in-run A/B list switches many of them during a run);
- a CMake option: `-DNAME=ON` on a build folder (an existing folder keeps its cached value);
- an environment variable at post-codegen: `NAME=1 python tools/post_codegen.py`, then build.

## 1. Fixes for one game that break another game's picture

Correctness fixes, not speed-ups: off because they break a game the base also serves.

| Key | What it does | Why it is off | Speed | Who turns it on |
|---|---|---|---|---|
| `blur_renut_wipe_overwritten` (A/B 218, port 76) | vivapinata-nx's EDRAM wipe (a clear through another layout, applied at the wiped target's next resolve) by exact tile spans, and skipped when another view over the same tiles drew or cleared after it. Blur's frame came out black with the old rule. | vivapinata-nx (2026-11-11), against the SDK's emulated GPU: its fade frames at the attract mode's cuts came out 5-8 % white (fog) with it on, 0 % off and on the reference. The case it misses: a clear through another layout at the same EDRAM base that covers every tile of the mask's target. Off = vivapinata-nx's rule (port 77). | A fix, not a speed-up | blur-nx |
| `banjo_renut_replay_inherits` (halo3-nx) | A replayed command buffer draws with the running device plus only what its recording set, as on the console. Off, each recording's whole device is used, and a recording on a device of its own (Halo 3's no-hardware device) has no targets, so its draws are dropped. | vivapinata-nx (2026-11-11): a hard shadow under its title logo with it on (2,378 dark grass pixels at 21 s), 0 off and on the emulated GPU; its mask's normal frames 5-9 % white on, 2.4-5.9 % off (the reference 2.5-5 %). Port 67's set_words fix does not cure it. Off since port 77. | A fix, not a speed-up | halo3-nx, and any game that records on a device of its own |
| `banjo_renut_blend_skip` (banjo_gpu_blend_skip) | Skips blending where it cannot change the pixel. | Off since banjo-nx package 30: it broke the depth of field (with banjo_gpu_bool_spec) and the deko3d renderer's river water in Banjo. | A speed-up (GPU), not measured since | none so far |

## 2. Faster forms that are not exact

Speed-ups that change results, so they are off unless a game has checked them.

| Switch | What it does | Why it is off | Speed |
|---|---|---|---|
| `BANJO_GEN_LR_MOVES_ELIDE`, `BANJO_GEN_NV_SAVES_ELIDE`, `BANJO_GEN_BACK_CHAIN_ELIDE` (CMake) | tools/frame_traffic.py's three stack-frame elisions for all game code: no guest link register moves, no saves of non-volatile registers that are locals, no back-chain stores. | They change the stale data a guest stack frame holds; sa2-nx found a game that reads a frame word it never wrote (characters tinted). | Faster (fewer stores per call); port 74's `SASRT_HOT_LR_ELIDE` (ON) takes the link register's half for the bulk machine copies only, which passed sa2-nx's frame-exact capture |
| `BANJO_GENERATED_NO_TRAPPING_MATH` (CMake) | -fno-trapping-math on the generated game code. | Not exact (see app/CMakeLists.txt). | Not measured |
| `BANJO_SWITCH_EARLY_RA` (CMake) | Leaves GCC's AArch64 early register allocation pass on. | It miscompiled the XDK's streaming copy in peggle2-nx (large textures came out as garbage on the console only). | GCC's default; no gain measured |
| `renut_format_7e3_rg11b10` (port 75) | Xenos 7e3 targets as 11:11:10 float, half FP16's bytes. | No alpha (the 7e3's two bits read as 1), one mantissa bit less on red and green, two on blue. | Less memory and bandwidth for such targets; sasrt-nx has none to measure |
| `renut_depth_float_no_stencil` (port 75) | Xenos float depth targets (D24FS8) as 32-bit float depth without stencil, half the bytes. | Stencil on those targets then does nothing (Katamari's scene depth is D24FS8 and uses stencil). | Less memory and bandwidth for float depth; not measured |
| `sasrt_texture_long_idle_frames` (port 58; an integer, 0 = off) | Releases host textures no draw bound for N frames. | Known to hang the GPU on the Switch (sasrt-nx, 1800). | A memory saver, not a speed-up |

(Port 72's VMX estimates and fused multiply-add are not exact either, but are ON at the user's word: within 1-3 ulp of
the exact forms, no further from the 360 than before; sasrt-nx's race with them and a fresh profile was +2.4 % fps.
`SASRT_VMX_FAST_ESTIMATES=OFF` / `SASRT_VMX_FUSED_MADD=OFF` give the exact forms. sasrt-nx's idea 46, building
write-once globals into the machine copies, is not in the base at all: sasrt-nx found it unsound.)

## 3. Speed-ups the console measured

All exact. Off because they did not pay on the console, not because they break anything. A game whose frames are
different may measure them again.

### 3a. Slower on the console

| Key | What it does | Speed |
|---|---|---|
| `banjo_renut_shadow_sort` | Depth-only draws of a recorded command buffer replayed sorted by shader. | **Slower:** render thread +1.1 ms, GPU +0.65 ms a frame (banjo-nx's console run of 2026-10-27) |
| `banjo_renut_replay_lookahead` | The next replayed op's lookups prefetched while a draw is drawn. | **Slower:** render thread +2.15 ms a frame (2026-10-27) |
| `banjo_renut_replay_stream_shaders` | The shader memo's sets prefetched two ops ahead instead of one. | **Slower:** +0.7 ms a frame, 4 of 5 periods worse (run c4, 2026-10-27) |
| `banjo_renut_texture_slot_skip` | A draw that changes some texture slots keeps the others without a full FillTextures. | **Slower:** render thread +0.64 ms a frame (night_final, 2026-10-28) |

### 3b. No gain on the console

| Key | What it does | Speed |
|---|---|---|
| `banjo_renut_replay_record_prefetch` | The replay prefetches the next op's shader and declaration records. | Neutral: -0.21 ms a frame, within the noise (night_gpu5) |
| `banjo_renut_host_packets` | A replayed draw's derived state kept by its device state and reused. | Neutral: -0.54 ms and +0.45 ms in two runs (night_gpu5) |
| `banjo_renut_merge_draws` | Consecutive draws that change nothing merged into one host draw. | No effect: Nuts & Bolts has no such draws (the census) |
| `banjo_renut_inline_line_compare` | An older compare of the pushed shared constants. | No effect since 2026-10-23 (the mask compare replaced it); before that it was **slower**: PushSharedConstants 2.7 -> 4.9 % of the render thread (36q) |

## 4. Trials: off until tried on the console, or because the emulator stalls with them

| Key | What it does | Why it is off | Speed |
|---|---|---|---|
| `banjo_display_sync` | A thread follows the display's vsync; the frame limiter releases frames on it. | In Ryujinx every run with it stalled or hung (banjo-nx runs 65, 67, 68); for a console trial. | A latency change; not measured on the console |
| `banjo_renut_core_layout` | Pins the drawing thread to core 0 and the other game threads to cores 1-2 at the first Swap. | Hung the first Swap in Ryujinx in 4 of 12 runs (re-masking running threads). | Faster in sa2-nx: render thread 12.5 -> 11.5 ms, game thread 17.6 -> 17.3 ms |
| `banjo_renut_deferred_chain` | Begin*Vertices draws join the snapshot delta chain instead of a whole device copy. | The chain is one for every device; a game drawing on two devices from two threads gets a delta on the wrong device (emulator hangs in 4 of 8 runs). | Less copying; not measured since |
| `banjo_deko3d_eager_kicks`, `banjo_deko3d_latency_trim` | Input-latency A/Bs: the frame sent to the GPU in more, earlier parts; the render thread waits for the GPU to trim its queue. | Latency trials, per game. | Lower input latency is the aim; frame time may rise |
| `banjo_deko3d_skin_once`, `banjo_deko3d_light_branch`, `banjo_deko3d_gpr32` | GPU A/Bs of Banjo's fur and lit shaders: skinning once a frame, per-light branches, a 32-register cap. | The same pixels; whether faster is the console's to say (their configs/*_on.toml). | Not measured on the console |
| `banjo_gpu_zcull_greater` | Asks the driver for ZCULL in the GREATER direction (reversed depth). | A console trial; the picture should be the same. | Not measured |

## Keeping this page

When a port reports that a key breaks its picture (with a comparison against the reference), its default goes to false
under the rule above, it gets a row in section 1 and in docs/ultimate-fixes.md, and the port that needs it is told to
set it in its own toml. A switch measured slower on the console goes in 3a with its numbers.
