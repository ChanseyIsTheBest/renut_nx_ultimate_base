<h1 align=center>renut_nx_ultimate_base</h1>

<p align=center>The Renut renderer for Nintendo Switch ports, with every universal fix and optimisation of the ports so far in one place.</p>

Each Switch port of an Xbox 360 game here (Banjo-Kazooie: Nuts & Bolts, Kameo, Halo 3, Viva Piñata, Ridge Racer 6, Sonic &
All-Stars Racing Transformed, Pac-Man CE DX, Bangai-O HD and the others) carries its own copy of
[reNut](https://github.com/masterspike52/reNut)'s Direct3D-level renderer on deko3d, and each copy fixed things the others
never met. This repository puts them together. It is [katamari-nx](https://github.com/ChanseyIsTheBest/katamari-nx) 1.0.1, the
newest port, with over fifty fixes from the others applied to it. A new port can start from here instead of from one
game's copy.

> [!NOTE]
> As checked in, this builds **Beautiful Katamari**: the game layer (codegen manifest, hooks, keys, hand-written functions)
> is katamari-nx's. The renderer, the shader translator, plume and the SDK are the shared part. You need your own copy of a
> game to build anything; this repository holds no game files and no code generated from a game.

## What's in it

The full list, with each fix's source commit, the evidence for it and its switch, is in
[docs/ultimate-fixes.md](docs/ultimate-fixes.md). In short:

<div align="center">

| Area | Fixes | From |
| --- | --- | --- |
| Clipping and culling | the GPU's user clip enable set at every command list (the console's holes and black squares); the game's user clip plane for every vertex shader; both cull bits cull every face; vertex kill only when every vertex is killed | rr6-nx, kameo-nx, vivapinata-nx |
| Geometry | strip restart markers kept out of the index range; multipass (export-only) shaders draw nothing; infinite/NaN vertex floats made finite; missing inputs read (0,0,0,1); loop constants, extra inputs, POSITION2, registers above r31 | rr6-nx, vivapinata-nx, keflings-nx, halo3-nx |
| Depth | the game's depth bias as dynamic state; compressed depth decompressed before it is read; a depth target grown to its colour target; the EDRAM depth restore; a backwards depth range on the PC | rr6-nx, sasrt-nx |
| Textures | the fetch constant over SetTexture's pointer; fetch signs, exponent adjust and integer formats; textures bound by fetch constant alone; 2D textures read through tfetch3D; DXT3A; volumes as 2D arrays | rr6-nx, sasrt-nx, kameo-nx, vivapinata-nx |
| Render targets and resolves | Kameo's ten GPU changes (colour and scan-out hand-overs, depth read layouts, resolves that keep targets, the gamma ramp once in 8 bits); gamma targets encoded; 16-bit fixed targets as FP16; one EDRAM view read through another; a clear over the same EDRAM in another layout; 1x1 float resolves read back for the CPU | kameo-nx, halo3-nx, sasrt-nx, vivapinata-nx |
| Command buffers and memexport | replays inherit the running device; recorded vertex shader binds; memexport written in place, and its results kept for the streams inside them | halo3-nx, sasrt-nx |
| Shader translator | each fetch's own filters and LOD bias, the cube op, tessellated lists, indexed literal arrays, bounded raw vertex fetch, version 0x0E containers; the fetch spec (one format's decode per pipeline); plain fetches (each fetch's sign, gamma and exponent code compiled away for the draws that have none) | vivapinata-nx, kameo-nx, feedingfrenzy-nx, banjo-nx, sasrt-nx |
| Speed | small captures, the stream cache's age queue, quad index buffer, NEON delta masks with prefetch; whole-buffer copies over the cache cap; the texture memo; a frame split past 192 MB of uploads; the refresh pacing the game as the 360's vertical blank did | bangaio-nx, kameo-nx, sasrt-nx, pacman-nx |
| CPU side | guest atomics with no extra barrier; the SDK runtime and FFmpeg in the Switch build's LTO and PGO; game memory and the NRO's code on 2 MB blocks | banjo-nx, katamari-nx |

</div>

Each fix keeps its source port's switch name, and nearly all are on by default. Any of them can be turned off from a game's
toml, so a port that finds one wrong for its game doesn't have to change the code. The switches added here are in
`app/src/renut_engine/gpu/ultimate.h`.

## State

> [!WARNING]
> **The base itself (Beautiful Katamari) has not run on a real console yet.** Ridge Racer 6, moved onto it in rr6-nx, has
> (2026-11-08): see [docs/ultimate-fixes.md](docs/ultimate-fixes.md), "Ridge Racer 6 on the console".

- Every fix builds for the Switch (the everyday NRO, and an LTO + PGO NRO) and for the PC.
- Port 46 (sasrt-nx, from a console crash) puts the SDK's input system under one lock: a game that reads its
  controllers from more than one thread could free the device list twice.
- On the console the base first cost Ridge Racer 6 4.1 ms of GPU a frame against its own build, at the same frame rate
  but with no headroom left. Port 44 gives 3.2 ms of it back (its game passes at its own build's times), and port 45 (the
  depth decompress only for a read) 0.5 ms more. 0.4 ms, in the game-gamma ramp's present, is still open.
- Ports 47 and 48 (rr6-nx) take the locks out of Renut's object lookups and shorten the guest clock's read. On the
  console they took Ridge Racer 6's drawing thread from its limit (42-48 fps while racing) to 60 fps with time to spare.
- Port 49 brings sasrt-nx's three GPU rounds (45 items, each with its own key and A/B target). In Ridge Racer 6 they
  take 0.4-0.9 ms of GPU a frame (13.6 -> 13.2 racing); in sasrt-nx's game 68 -> 47 ms. The optimiser passes act once a
  game's shader package and DKSH cache are made again.
- Port 50 is sasrt-nx's fourth GPU round: memexport draws drawn with the rasterizer off no longer go through the
  point-sprite geometry shader, and their export windows are read from the constant buffer. sasrt-nx's game went from
  46.6 to 38.8 ms of GPU a frame; games without memexport draws (Ridge Racer 6) are unchanged.
- Port 51 is sasrt-nx's fifth GPU round (its pixel-identical items): a texture fetch's sign, gamma and exponent
  conversions become constants of the draw, so each pixel shader keeps only the conversions its textures use (sasrt-nx:
  -3.9 ms of GPU a frame; Ridge Racer 6, whose fetches are nearly all plain, flat). It also fixes the DKSH cache tool,
  which read the pipeline list 4 bytes off since the first round.
- Port 52 is what was left of sasrt-nx's changes for any game: dynamic resolution that steps down only when the GPU, not
  the CPU, holds the frame (and any range on the Switch), SMAA after a composite with more than one target, the depth
  restore's whole clear, and a 1 s grace before a controller HID reports absent is disconnected for the game.
- Port 53 makes the CPU time a frame the game thread's own work: the frame cap's sleep and the display pacing's wait
  count as waits, so the FPS overlay's CPU figure and port 52's dynamic resolution see the game's real load.
- Port 54 is sasrt-nx's render-thread round: the EDRAM depth restore's source found from each depth target's own few
  candidates instead of two scans of every target a draw, memory-export seeds written into CPU-cached memory, two
  per-draw counters inlined, and A/B lines that give draws a frame and microseconds a draw.
- Port 55 is feedingfrenzy2-nx's SDK fixes: controllers 2 to 4 sign in as guests when a game opens the sign-in screen,
  players 3 and 4 get their own button maps, the leaderboard calls fail safely, the exit no longer crashes, freed memory
  reads as zeros when it is used again, and five opt-in VMX accuracy keys for the code generator.
- Port 56 is mk9-nx's fixes and ds3-nx's polling back-off: faster physical allocations at loads, fallbacks for a rare
  boot hang, audio that keeps going when the output stalls, a fix for huge stray triangles, one-pass shader compiles
  while a draw waits, the first controller to press Start as player 1, and polling threads that sleep instead of
  spinning a core.
- Port 57 is ds3-nx's SDK fixes: the time a console spends asleep or at HOME no longer reaches the game as one huge
  frame, package reads are one at a time, XMA audio no longer replays sound it has played, modules are found by file
  name when a game remaps its paths, and multi-disc games can take the disc swap over.
- Port 58 is SASRT's memory round: a governor that watches the Switch's free memory and gives caches, idle render
  targets, kept thread stacks and GPU memory back before it runs out (SASRT's docked races went from crashing to
  1,080-1,220 MB of CPU-side heap in use, from 1,526), and the fix for a pipeline shortcut that could draw with the
  previous draw's specialised pipeline (SASRT's broken octopus).
- Port 44 and the translator changes take effect when a game's shader package is made again. Before measuring on the
  console, build its DKSH cache on the PC (`tools/deko3d_variant_cache.py`): a first run that compiles its shaders while
  it draws is slower on the GPU than the build is.
- Beautiful Katamari runs in Ryujinx and on the PC (Vulkan and D3D12): menus, the hub, and a new game through to the
  practice room, with no errors logged, and the Switch build's picture matches the PC's. It hasn't yet been compared frame
  by frame with katamari-nx's own build.
- Katamari doesn't record command buffers, use memexport or bind textures by fetch constant alone, so the fixes for those
  are only build-tested.
- The PGO profile in `pgo/` is katamari-nx's and has no counts for the SDK yet.

## Starting a port from it

1. Swap the game layer: `app/renut_manifest.toml` (the codegen config), the game's keys and fixes (`app/src/katamari_*`,
   `app/src/cpu/hot`), its tomls (`app/katamari_switch*.toml`) and its icon.
2. Bind the D3D hooks to your game's own functions. They are bound to Katamari's addresses (XDK 2.0.5632); a game on another
   XDK needs its own device layout. Some hook bodies are here for a game to wire up to its own function, such as
   `KeepConstantF4Pending` ([docs/ultimate-fixes.md](docs/ultimate-fixes.md), "Needs the game's own function address").
3. Build as [docs/building.md](docs/building.md) describes: the PC build first (it runs the code generator and the
   post-codegen steps, and builds the shader translator), then the shaders, then the NRO.

## Build

Windows, LLVM clang and the VS Build Tools for the PC build; devkitA64, libnx and deko3d from
[devkitPro](https://devkitpro.org) for the Switch; Python; and your own copy of the game. The whole process is in
[docs/building.md](docs/building.md).

<div align="center">

| Folder | Contents |
| --- | --- |
| `app/` | The game layer and the renderer (`app/src/renut_engine/gpu`), the shader translator (`app/thirdparty/reblue-XenosRecomp`), plume, the CMake project |
| `sdk/` | ReXGlue SDK with the Horizon layer and the code generator changes |
| `tools/` | Code generation steps, build and test scripts, shader tools |
| `pgo/` | katamari-nx's PGO profile (counts of which code ran, no game code or data) |
| `dist/` | The settings of the test builds |
| `docs/` | Documentation: [ultimate-fixes.md](docs/ultimate-fixes.md), [building.md](docs/building.md) and katamari-nx's notes |

</div>

## Credits

- **ChanseyIsTheBest** — the ports and this base.
- **[Tom Clay](https://github.com/rexglue/rexglue-sdk)** — the ReXGlue SDK, built on the work of the
  **[Xenia](https://xenia.jp)** team.
- **reNut and [masterspike52](https://github.com/masterspike52/reNut)** — reNut's Direct3D-level renderer (reused with its
  author's permission).
- **[StevensND](https://github.com/StevensND/nfsmw-nx)** — nfsmw-nx: the Switch layer of the SDK and much of the method.
- **[nebadasSwifty](https://github.com/nebadasSwifty/masseffect-nx)** — masseffect-nx.
- **[hedge-dev](https://github.com/hedge-dev/XenosRecomp)** — XenosRecomp, the Xenos shader translator.
- **[plume](https://github.com/renderbag/plume)** (from UnleashedRecomp), **[deko3d](https://github.com/devkitPro/deko3d)**
  and **[uam](https://github.com/devkitPro/uam)** by fincs, with SwitchWakerHD's uam patches.
- **[devkitPro](https://devkitpro.org) and [switchbrew](https://github.com/switchbrew/libnx)** — devkitA64 and libnx.
- **[KhronosGroup](https://github.com/KhronosGroup/SPIRV-Cross)** — SPIRV-Cross.
- The ports whose fixes are here: [renut-nx](https://github.com/ChanseyIsTheBest/renut-nx),
  [katamari-nx](https://github.com/ChanseyIsTheBest/katamari-nx), [kameo-nx](https://github.com/ChanseyIsTheBest/kameo-nx),
  [bangaio-nx](https://github.com/ChanseyIsTheBest/bangaio-nx), [pacman-nx](https://github.com/ChanseyIsTheBest/pacman-nx),
  [keflings-nx](https://github.com/ChanseyIsTheBest/keflings-nx), [sa2-nx](https://github.com/ChanseyIsTheBest/sa2-nx),
  [futari-nx](https://github.com/ChanseyIsTheBest/futari-nx),
  [feedingfrenzy-nx](https://github.com/ChanseyIsTheBest/feedingfrenzy-nx), and the Halo 3, Viva Piñata, Ridge Racer 6 and
  Sonic & All-Stars Racing Transformed ports.
- The other libraries listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal

No affiliation with Microsoft, Nintendo or any game's publisher. This repository contains no game files and no code
generated from a game. Users must provide their own legally obtained copy, and everything made from it (`app/generated/`,
the shaders) is made on their own computer. Running homebrew requires custom firmware, which violates Nintendo's ToS and can
get a console banned — your call.

Source code is provided under the GPL-3.0 License (see [LICENSE](LICENSE)), like the ports it builds on. The SDK keeps its
BSD-3-Clause license (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
