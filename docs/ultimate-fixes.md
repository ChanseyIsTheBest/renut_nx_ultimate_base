# The ultimate base's fixes

This repository is katamari-nx 1.0.1 with the fixes below. Most were applied by scripts of exact, asserted edits (every anchor
must occur once, or the script stops), kept in the renut-ultimate workspace rather than here. A few came over as three-way patches from the port's own
commit (marked "patch" below). Each fix keeps the source port's switch name, so a game's toml can turn any of them off. Every
switch is on by default unless the row says otherwise.

"Built" means it compiled for the Switch with no new warnings (at each fix, and as this repository; the LTO link adds five GCC
notes in the SDK's XMA and network code). Nothing here has been run on
the console. Ryujinx and PC runs are below ("Test runs").

The translator, codegen and shader-header changes only take effect when a game's shaders or code are made again. The PC build
(2026-11-08) made Katamari's code again with the base's generator and its post-codegen steps: the same code as katamari-nx's
but port 18's vpkuhus / vpkuwus through a temporary (renut_recomp.3, .37, .110, .111: the old code overwrote v0's bytes before
reading them) and a few choices of the flush-mode and loop-barrier steps. The Switch builds since compile that code. Katamari's
deko3d shader package has not been made again: on the Switch it keeps its old meaning, and the host side is written so that it
stays correct. The PC build makes its shaders again by itself, and must, for port 25.

## Test runs (2026-11-07/08)

Ryujinx (the NRO at `/switch/katamari-nx/katamari-nx.nro` on the emulator's SD, the path the app falls back to when an
emulator gives no argv) and the PC (`app/out/pc`, Renut on an RTX 3060), with scripted inputs (`input_script`).

| Run | Build | Route | Result |
|-----|-------|-------|--------|
| u2 | everyday NRO, katamari-nx's code | menus, 300 s | no error, every renderer counter normal |
| u5 | the same | the save loaded, the hub planet | the 3D hub as expected |
| u7 | everyday NRO, code made again | a new game, the King, the practice room rolling | no error; the same picture as the PC |
| u8lto | LTO + PGO use, the SDK and FFmpeg in it (port 42) | the same | no error; XMA decoding running |
| pc3 | PC, Vulkan | a new game into the practice room | no error, no `[flash]` line, 99 % of texture slots by the memo |
| pc4d3d12 | PC, D3D12 (ports 24's geometry-shader clip and 31's depth remap have their D3D12 forms) | the same | no error; darker than Vulkan, as katamari-nx noted before (the Vulkan form is this port's reference) |

Katamari has no recorded command buffers, no memexport and no fetch-only texture slots: h1, 35, 36, 37 and 39-41 had nothing
to act on in these runs. u1 and ref1 (katamari-nx's own NRO) crashed at 35-42 s in the SaltyNX code of the profiler thread
because the NRO was not at the fallback path; not from these changes.

### Ridge Racer 6 on the base (2026-11-08)

rr6-nx's branch `ultimate` (uncommitted when written): the base's renderer, translator, plume and SDK merged three ways
into rr6-nx from their common starting point (katamari-nx 1.0, 064bff99a2, whose renderer files rr6-nx was copied from).
RR6 keeps its XDK 2135 hooks, device head and adapters, its alpha-near-one and fog-NaN switches (A/B 85-87 there) and its
diagnostics; its own copies of the fixes the base took from it went, so each is defined once. Its own build, shader and
emulator folders (pc-ult, switch-ult, renut_switch_ult, ryu-data\rr6-ult). The merge found port 43: with the base as it was,
RR6's race drew black and its intro video black and white.

| Run | Build | Against | Result |
|-----|-------|---------|--------|
| ab_ult2 | PC, Vulkan, rr6-nx ultimate | rr6-nx master's PC build, same route and save | the race's countdown frame a mean of 0.8 of 255 apart; video, menus, car select the same |
| emu_ab_ult | Ryujinx, rr6-nx ultimate NRO | master's NRO of 2026-11-05 15:34 | race frames the same (mean 1.2 at 265 s); no errors |

What the merge showed about the boundary: the hooks and device layout are per XDK (5632 for Katamari, 2135 for RR6), and a
port's own fixes already in the base have to come out of the port. The A/B target numbers collide between ports.

### Ridge Racer 6 on the console (2026-11-08): what the base costs on the GPU

The first console runs of a game on the base: rr6-nx's `ultimate` branch on the user's Switch, stock clocks (CPU 1020, GPU
460.8, memory 1600 MHz, handheld), the same scripted race each run (rr6-nx's `tools/console_run.py`, its m22 timeline), against
rr6-nx's master build. GPU time from Renut's GPU profile, the race's seven 10 s blocks averaged.

| Pass (ms a frame) | master | ultimate | + port 44 | + `sasrt_deko3d_depth_resolve` off |
|---|---|---|---|---|
| main pass 1440x800 | 7.885 | 9.800 | 7.890 | 7.868 |
| full-screen 1280x720 (3 draws) | 1.619 | 2.608 | 1.617 | 1.618 |
| resolves (9 a frame) | 1.854 | 2.317 | 2.317 | 1.860 |
| present and overlays | 0.741 | 1.158 | 1.112 | 1.157 |
| 256x512, 320x180, 1280x720 depth | 1.237 | 1.511 | 1.245 | 1.240 |
| total | 13.350 | 17.414 | 14.192 | 13.756 |

The "+ port 44" run also toggled targets 73-79 and 86 in-run (4 s halves), which shaves its present a little; the last column
is that build with only the decompress off. The race at 58-60 fps in every column but the second (51-53 cold, 58-59 warm, at 16.4-16.9 ms right on the frame's budget).
Where the 4.06 ms went:

- **3.16 ms: port 25's sign path.** Every texture fetch of every pixel shader carried the sign, gamma and exponent block
  behind a run-time branch, taken by no RR6 texture: weighted by GPU time the pixel shaders had 3.8 times the instructions
  (about 120 more a fetch; shaders without fetches unchanged) and more registers (ps_80391EE201BC4392 252 instructions and 32
  GPRs before, 1,283 and 60 after). **Port 44** (below) folds it away per pipeline: with every bit set each heavy RR6 shader
  compiles to exactly its master size (ps_FEB1A950B191D9BA 312 to 144 words), and the game passes are master's again.
- **0.46 ms: `sasrt_deko3d_depth_resolve` (port 09).** It decompressed a compressed depth target at every unbind, whether or
  not anything read it. RR6 never reads its depth (no depth resolve or hand-over bind in any run; its master never
  decompressed and draws right). **Port 45** (below) decompresses only for a read: with the key on, RR6's run left 37,220
  for later and needed none, and its resolves are master's again (1.859 ms).
- **0.42 ms: present, the game-gamma ramp path.** The ramp costs 0.58-0.76 ms in the base (in-run A/B 86, 9-10 pairs a
  run at 57+ fps) and 0.04 in rr6-nx's master (its A/B 73, 9 pairs). Ruled out on the console, each with that A/B: the
  table's form (256 x RGBA8 by the 8-bit value, 0.15 ms better; 1024 x RGBA32F, 0.22 worse: in-run targets of a test build),
  the upload (every frame, as master: still 0.76), the present shader's structure (master's frame sample and pow in a test
  build: still 0.58) and its registers (10 in both). Left: what surrounds the ramp texture itself (its descriptor, the
  caches). The test builds were not kept. Open.

Ruled out on the console, one toml a run: the 16-bit pair (`sasrt_renut_fixed16_float`, `_clamp`), the copy-engine wait,
`banjo_renut_resolve_clears_both` with `vivapinata_edram_clear_overlap`, `sasrt_renut_deko3d_clear_new`; kameo-nx's
hand-overs never engage in RR6 (0 resolves not drawn, 0 drawn after all). CPU, the race: the drawing thread 14.91 to 14.36 ms
a frame (the base's render-thread rounds and rr6-nx's own copies together), the render thread 7.05 to 6.87.

**Racing, the drawing thread (ports 47 and 48).** The race those runs measured was mostly a parked car (the first ZR press of a run through sys-botbase never reached the game). Driving, RR6 ran at 42-48 fps with the GPU at 14.1-14.7 ms and the render thread at 9.3-9.5: the game's drawing thread was the limit (111-123 % of a core in the profiler's count). About 30 % of it was Renut's object lookups (a read-write lock's unlock, contended with the render thread) and about 13.6 % the guest clock. With ports 47 and 48: 60 fps in every 10 s window, the same stretch and a race from GO with the pack, the drawing thread 86-88 % there (14.5 ms a frame) and 68 % racing (11.3 ms). The GPU, at 13.5-14.7 ms a frame, is now the closest to its limit.

**A trap for every port measuring on the console:** any change to the translator makes every GLSL text, and so every DKSH
cache key, new. The first run of such a build compiles its variants with uam while drawing (155 compiles here, 754 stages on
the generic variant meanwhile), and its GPU times are not the build's. Make the cache on the PC first:
`tools/deko3d_variant_cache.py --generic [--plain-guess <shader table>] --list <the console's pipeline lists> --merge <the
console's cache>`, and check the log's "uam 0 compiles" before comparing.

## The fixes

| # | Fix | From | Evidence there | Switch | State |
|---|-----|------|----------------|--------|-------|
| 01 | Maxwell user clip enable written at every command list's start | rr6-nx 44466322 | console: flyover squares, rocks, cables, chrome car, TV; counters matched Ryujinx | `banjo_deko3d_user_clip_enable` (1; -1 leaves it) | built |
| 02 | NaN/Inf float constants flushed to 0 in the constant copy | kameo-nx 776227708d | Kameo | `banjo_renut_const_nan_flush` | built |
| 03 | Strip restart markers kept out of the index range, rewritten to the host's | rr6-nx f21694b6 | RR6 torn meshes, wedges | `rr6_strip_markers` | built |
| 04 | Texture fetch read from the fetch constant over SetTexture's pointer | rr6-nx 9f33ee3f | RR6 | `rr6_fetch_over_pointer` | built |
| 05 | A resolve's clear flags clear both surfaces they name | kameo-nx 71d0abaf | Kameo console run | `banjo_renut_resolve_clears_both` | built |
| 06 | A 2D fetch read through tfetch3D gets the stacked path | kameo-nx d4a836ec | Kameo | `banjo_renut_fetch3d_on_2d` | built |
| 07 | Both cull bits cull every face | kameo-nx 2b378904 | Kameo audit | `banjo_renut_cull_both` | built |
| 08 | The game's depth bias reaches the GPU as dynamic state | rr6-nx 42516bee (supersedes kameo's poly_offset) | RR6 decals | `rr6_depth_bias` | built |
| 09 | Compressed depth decompressed before it is read | sasrt-nx | console: shadow cascades black | `sasrt_deko3d_depth_resolve` | built |
| 10 | Leaving a copy layout waits for the copy engine | sasrt-nx | console only by nature | `sasrt_deko3d_copy_wait` | built |
| 11 | A new colour target cleared once on deko3d | sasrt-nx | leftovers of other images | `sasrt_renut_deko3d_clear_new` | built |
| 12 | Single-level volume held as a 2D array on deko3d | sasrt-nx, pacman-nx, halo3-nx | Ryujinx only (the patched Ryubing ignores Z); harmless on the console | `sasrt_renut_deko3d_volume_as_array` | built |
| 13 | Vertex kill drops a primitive only when all its vertices are killed | vivapinata-nx 6eecb25a | Viva's ground layers | `banjo_deko3d_vs_kill_and` | built; its flag is at varying location 30 here (26 there) |
| 14 | Multipass (export-only) vertex shaders rasterize nothing | vivapinata-nx fe12c548 | PC vs emulated GPU | `banjo_renut_export_only_draws` | built; the export mode read only in 0x11 containers |
| 15 | Infinite/NaN single-float vertex elements made finite | vivapinata-nx fe12c548 | PC vs emulated GPU | `banjo_renut_vertex_finite` | built |
| 16 | A clear over the same EDRAM in another layout reaches a resolve of it | vivapinata-nx e242c74b | measured against the emulated GPU | `vivapinata_edram_clear_overlap` | built |
| 17 | A depth target smaller than its colour target grows to it | rr6-nx 5da4b39f | RR6 race | `rr6_depth_covers_colour` | built |
| 18 | Recompiler: vpkuhus / vpkuwus through a temporary | rr6-nx 5da4b39f (pacman-nx's report) | Katamari's own generated code has such sites (renut_recomp.3, .110, .111) | none (codegen) | built; reaches Katamari when its code is generated again |
| 19 | Inputs the declaration lacks read (0,0,0,1) | keflings-nx cd71f403 | against the SDK's emulated GPU | `keflings_renut_missing_input_w1` | built |
| 20 | Device loop constants; extra vertex inputs; POSITION2; registers above r31; 16 cube directions | halo3-nx ca6c7fc8, 6bfc7840fe | Halo 3 | `halo3_renut_loop_constants` | built |
| 21 | Host geometry shaders declare FOG0 and POSITION2; a nonzero generated-register byte fills iPointCoord | kameo-nx 7c2b28870c (vivapinata-nx 952a38c2) | Kameo's sprites on the emulator | none | built |
| h1 | Replays inherit the running device | halo3-nx 2f7496f9, patch | Halo 3: 75 % of its draws were dropped | `banjo_renut_replay_inherits` | built; on, replays skip resource objects, slot extents and packet keys |
| h2 | vfetchBound decodes through one function | halo3-nx 1b894cc4, patch | 921,600 decodes, same values | none | built |
| 22 | The ports' translator changes merged: a fetch's own filters and LOD bias, the cube op's values, tessellated lists' control points, indexed literal arrays, bounded raw vertex fetch; version 0x0E containers | vivapinata-nx (three-way from katamari 88e0eaa173), kameo-nx (container_0e.h, from feedingfrenzy-nx) | Viva Pinata, Kameo, Feeding Frenzy | none | built for the runtime; the translator is built by the PC build |
| 23 | The host side of 22: sampler families (5 a sampler, 1024 slots), the deko3d texb variant, tessellated triangle lists' index fetch; DXT3A textures | vivapinata-nx d0bc6bd0d9, 4b7976ad6d | Viva Pinata | none | built |
| 24 | The user clip plane for every vertex shader (no list): plane 0 at 1488 of the shared constants, zero when a draw has none | rr6-nx 45bbd0e09c, vivapinata-nx f346dd4963 | RR6's car reflections, Viva's water | `rr6_user_clip_plane` | built |
| k10 | Kameo's ten GPU changes: colour and scan-out hand-overs, depth sampled while only tested, depth read layout, resolve keeps targets, the gamma ramp copied once and in 8 bits, a cheaper scan-out | kameo-nx 93ef4cd972, patch | Kameo's third console run | `banjo_deko3d_color_handover` and the rest, A/B 73-79 | built; the tenth (a 32-register cap on four Kameo shaders) is Kameo's own list |
| 25 | Fetch signs (gamma, biased), the exponent adjust (every dimension) and the integer format, in the shader after filtering | sasrt-nx, with halo3-nx's gamma fetch | Halo 3 and Sonic Transformed too bright without | `sasrt_renut_texture_signs`, `_exp_adjust`, `_int_format` | built; the PC's mask is 0x1FFF now: its shaders must be made again (the PC build does) |
| 26 | Gamma targets encoded; a target's colour exponent bias on the alpha; pipeline jobs keep those bits | halo3-nx 094a0011, 89029486 | Halo 3's menu | `halo3_renut_target_gamma_bias` | built; constants 924 / 925 (920 / 921 there) |
| 27 | The game's ramp in either form (SetPWLGamma's segments, SetGammaRamp's entries), 1024 entries | halo3-nx 094a0011 | Halo 3 | (`banjo_renut_game_gamma`, off here) | built |
| 28 | 16-bit fixed-point targets as FP16, their resolves clamped to -32..32 | sasrt-nx | Sonic Transformed's HDR scene | `sasrt_renut_fixed16_float`, `_fixed16_clamp` | built; the colour hand-over skips them |
| 29 | The EDRAM depth restore: a depth target takes the words a 32-bit colour target wrote over its EDRAM | sasrt-nx | Sonic Transformed | `sasrt_renut_depth_restore` | built |
| 30 | One EDRAM view read through another (2_10_10_10_FLOAT encoded into 2_10_10_10) | halo3-nx 838ea8c587 | Halo 3's black menu | none | built |
| 31 | A backwards depth range on the PC's back ends (the vertex shader writes the depth) | sasrt-nx | Sonic Transformed | `sasrt_renut_reverse_depth_remap` | built; at 1504 of the shared constants |
| b1 | Render-thread speed-ups: small captures, stream cache at a later sighting, quad index buffer, point sprites on the fast path, NEON delta masks | bangaio-nx 59c2669f8e, 632348a67b, 0e3fe40f4a, patch | "exact" there; Bangai-O's missile storms | `banjo_renut_small_captures` and the rest, A/B 71, 72, 80-82 | built |
| 32 | A frame past 192 MB of upload ring ends on the GPU, nothing presented | kameo-nx fbbcdd3a | a big level ran the console out of memory | `banjo_renut_split_frame_mb` | built |
| 33 | A texture kept when its fetch differs only in signs | sasrt-nx | Sonic Transformed | `sasrt_renut_texture_keep_on_signs` | built; needs 25 |
| 34 | The BeginShaderConstantF4 hook body (KeepConstantF4Pending) | sasrt-nx | Sonic Transformed's respawn effect | `sasrt_renut_constant_f4_pending` | built; a game wires it to its own function |
| f1 | The fetch spec: a tessellated draw's patch kind and its first four fetches' type words become constants, so the generic fetch's switch over fifteen formats is one format's decode | banjo-nx through vivapinata-nx 6eecb25a, patch | Viva Pinata's terrain (up to 580,000 vertices a frame) | `banjo_gpu_fetch_spec`, A/B 83 | built; the pipeline list goes to version 5 (older lists converted); takes effect when a game's shader package is made again |
| b2 | bangaio-nx v6: delta line prefetch, the stream cache's age queue, the render queue's start check | bangaio-nx 436fea96ab, patch | Bangai-O | `banjo_renut_delta_prefetch` and the rest, A/B 84 | built |
| k11 | A whole-buffer copy may take the stream cache up to 256 MB past its limit, so the range copies it replaces can go | kameo-nx df7ca54a52, patch | Kameo | `banjo_renut_whole_over_cap_mb` | built |
| r1 | Frames ahead of the render thread (1, as before; 0: the game waits at its Swap) | rr6-nx a935b673c3 (from sasrt-nx), patch | RR6 | `rr6_frames_ahead`, `rr6_diag_render_delay_ms` | built |
| p1 | The refresh paces the game: the display thread tests the newest image's fence at each refresh and starts the game's next frame after it, as the 360's vertical blank does; the queue drain and the late input guard with it | pacman-nx 72c28ced60, 7008db17e8, a1b65c6446, patch | pacman-nx's first console run: 64 ms of input lag with 14 ms of work | `pacman_vblank_display_sync` (with `banjo_display_sync`, on in Katamari's defaults) | built; `pacman_renut_double_buffer` is here but off |
| h3 | Recorded binds: each draw keeps the vertex shader bind it had when the game made it, and a replay applies the draw's own | halo3-nx b39c4a5ec5 (from sasrt-nx), patch | Halo 3 records its draws at load: a replay took another mesh's layout | `banjo_renut_recorded_binds` | built |
| h4 | A memexport draw whose stream is its slot's newest window over that memory writes into it, with no seed and no readback | halo3-nx 890294867c, patch | Halo 3's particle pools | `banjo_renut_export_in_place` | built |
| 35 | A resolve into a 1x1 32-bit float texture copied back to its guest memory when its GPU work is done, where the CPU reads it | sasrt-nx | Sonic Transformed's glow 20x too dark without (its average luminance read 0) | `sasrt_renut_resolve_readback` | built; the colour hand-over skips such destinations |
| 36 | Memexport results kept on the host four frames after their write-back, and a vertex stream inside one reads them | sasrt-nx | Sonic Transformed's meshes collapsing to a point (replays after the game cleared the memory) | `sasrt_renut_exports_held` | built |
| 37 | A fetch constant set without SetTexture sampled as the GPU reads it (its own texture, keyed by its base address); such slots answered by the texture memo by their fetch words | sasrt-nx | Sonic Transformed binds its textures that way | `sasrt_renut_texture_fetch_only`, `_memo_fetch_only` (`_verify`, off) | built; key `2 \| page << 12` here (bit 0 is port 06's stacked view, and marks a stacked fetch) |

### The audit's fixes (2026-11-07)

| # | Fix | On | Switch | State |
|---|-----|----|--------|-------|
| 38 | A stream with port 15's finite single floats keeps the one-pass swap when none of the draw's vertices has an infinity or NaN there (the guest words tested; the chunked path, which applies it, otherwise) | 15 | none (exact) | built |
| 39 | A replay whose running device holds every inherited word as the recording had it keeps the record's answers (resource objects, shadow sort, packet keys, slot extents): one compare of about 1 KB a replay | h1 | `ultimate_renut_replay_inherit_same` | built |
| 40 | A kept export the CPU wrote other data over is read from guest memory again; a clear (zeros, or one vertex repeated at the stream's stride) still reads the kept results. Eight 512-byte blocks sampled, once a frame per export | 36 | `ultimate_renut_held_exports_guard` | built |
| 41 | Fetch-only textures keyed by a hash of the data they describe too (one page sampled as two textures keeps both), and refreshed across frames instead of bound again | 37 | none | built |
| 42 | The SDK runtime (kernel calls, threads, the audio driver) and FFmpeg (XMA) in the Switch LTO and PGO, C and C++ only; each build folder has its own SDK libraries (`REXGLUE_OUTPUT_IN_BUILD`), so the stages no longer replace each other's | build | `BANJO_SWITCH_SDK_OPT` (CMake, on) | built (everyday and LTO + PGO use); the profile in `pgo/` has no SDK counts until an instrumented console run |
| 43 | 0x0E containers: a literal of several registers keeps them all. container_0e.h (kameo-nx's form) wrote the definition's count in registers (words / 4), but the 0x11 table counts words and the translator reads (count + 3) / 4 registers, so only the first register of a multi-register literal was a literal and the rest read the game's constant registers | 22 | none | found by rr6-nx's merge (2026-11-08): Ridge Racer 6's pixel shaders hold c253-c255 as one literal (22D0/12); its race drew black and its intro video black and white. Fixed, the race matches rr6-nx's master on the PC. kameo-nx has the same line and 3,490 multi-register float literals in its dump (its ps_00078ce0812e1370 reads c254 and c255 through g_C); it looks right, probably because its D3DDevice_SetPixelShader copies the literals into the device's constants (container_0e.h's own note), which Renut uploads. Not checked on Kameo |
| 44 | Port 25's per-fetch sign, gamma and exponent block folded away per pipeline where the draw's fetch constants have none of it: a tools/deko3d_glsl_opt.py pass (plainfetch) ends each fetch's condition with constant 926's bit for its fetch constant; render/draw.cpp PlainFetchMask sets the bits on the pipeline key (fetch_pad, its spare word; the list format unchanged); pipeline.cpp passes 926 and treats the key as specialised (the generic pipeline draws until it is built); deko3d_variant_cache.py reads the word, and --plain-guess makes the all-plain variant from the shader table for lists made before it | 25 | `sasrt_gpu_plain_fetches` | sasrt-nx's design (2026-11-08, its GPU round), measured by rr6-nx on the console the same day: 3.16 ms a frame of Ridge Racer 6's race back, its game passes at its master build's GPU times (the section above). deko3d only; takes effect when a game's shader package is made again (Katamari's has not been) |
| 45 | Port 09's depth decompress done lazily: a compressed depth target leaving the bind point (or bound at the end of a list) is marked pending on its texture with the view it was written through, and decompressed only when a barrier moves it to a read layout (bound through that view again, then the bound targets again) or before it is bound through another view | 09 | `ultimate_deko3d_depth_resolve_lazy` (off: port 09's eager form) | rr6-nx's console race: 37,220 decompresses left for later, 0 needed (it reads no depth), resolves back at its master's 1.86 ms from 2.32. Its reads of depth on a game that has them (sasrt-nx's shadow cascades) not yet checked on a console. The deko3d report line counts them |
| 46 | The SDK's input system under one lock (sasrt-nx's fix, 2026-11-08): InputSystem::RefreshDevices rebuilt the device list (clear, then push_back) with no lock while another guest thread read or rebuilt it, and its 50 ms check (a load, then a store) let two threads through: a double free under XamInputGetCapabilities on sasrt-nx's console. A recursive mutex (devices_mutex_) held by RefreshDevices, DriverForDevice, DeviceInfoFor, Shutdown and SetDeviceAssignment, and by GetCapabilities, GetState, SetState and GetKeystroke before their refresh (after the read hook) | sdk | none (a lock, the same results) | built (the base's and rr6-nx's Switch builds). rr6-nx took the same edits and ran three console races and two Ryujinx runs with it, no error; no crash had been seen there before either |
| 47 | Whether an address is one of Renut's objects answered without locks (rr6-nx, 2026-11-08): records::Lookup (a shared_mutex and an unordered_map) and resource.cpp's OwnsHeader (a mutex and an unordered_set) read a table over the record heap instead: every record and owned header is an o1heap allocation from its one 64 MiB block, so a byte per 16-byte granule says what is there (the record kind + 1 in the low nibble, an owned header in the top bit), in pages of 4,096 made at their first mark and never freed. Marked under the existing locks after the set and map, which are kept for the off position; an address outside the table goes to the locked path while any mark is outside | records | `rr6_records_lockfree` (off: the locked set and map) | rr6-nx's console race: those lookups (through its XDK object test at every bind, unbind, AddRef, Release and GetType) were about 30 % of its drawing thread, most of it the read-write lock's unlock; with this and port 48 its slow part went from 42-48 fps to 60, and its drawing thread from 111-123 % to 86-88 % of a core there. The same answers. Built (the base's and rr6-nx's Switch builds) |
| 48 | The guest clock's short read (rr6-nx, 2026-11-08): UpdateGuestClock's lock-free path made four calls a read (the host count, a cvar accessor, the thread pointer for a census count, the base through an acquire load of a heap object) and a 64-bit divide every result waited on. Settled once in RecomputeGuestTickScalar: the counter inline (mrs cntpct_el0), the base on a cache line of its own, floor(x / den) by a multiply with r = floor(2^64 / den) and one compare (exact: checked against division), no calls | sdk | `clock_count_calls` (off; on: the census's clock count, the old path) | rr6-nx's console race: its timers (a QueryPerformanceCounter and a conversion, about 13.6 % of its drawing thread's busy time in the slow part) left the thread's top functions. The same count. Built (the base's and rr6-nx's Switch builds) |

**Guest atomics**: already in the base, from banjo-nx. `tools/guest_cas.py` (a post-codegen step) writes each `stwcx.` / `stdcx.`
as `REX_GUEST_CAS`, which is `__atomic_compare_exchange_n` seq_cst (`BANJO_SWITCH_GUEST_CAS`, on). Checked in the built
`renut_recomp.0.cpp.obj`: eight `ldaxr` / `stlxr` loops with no barrier after them. The eight `dmb ish` left are cold
blocks behind the runtime byte `g_rex_atomic_legacy_barrier` (0), which puts the barrier back for an A/B. The SDK's own
generator still writes `__sync_bool_compare_and_swap`: a port without the step keeps the barrier.

**PGO and LTO: later** (2026-11-08). No instrumented NRO and no profiling run on the console yet.
Port 42 changes nothing in the everyday build (`app/out/switch`: no LTO, no PGO); its SDK options act only in a folder
built with `BANJO_SWITCH_LTO` or `BANJO_PGO`. The steps for then are in [docs/building.md](building.md) (section 4).

### Where two ports met

- **Vertex input locations 23-27**: halo3-nx's set and vivapinata-nx's morph inputs both used them, and with rr6-nx's TANGENT1 they
  would be 34 locations: the hardware takes 32 and the host binds every row. halo3-nx's by default; vivapinata-nx's with
  `RENUT_VERTEX_INPUTS_MORPH` defined for the translator and the host. TANGENT1 at 28.
- **Varying location 26**: port 20's POSITION2 moves the point size there, where port 13 had its kill flag: the flag is at 30.
- **Shared constants**: 1360 halo3-nx's loop constants (sasrt-nx had its target scale there), 1488 the clip plane, 1504 the depth
  remap (sasrt-nx had 1352, banjo-nx's skin mode here): 1552 bytes, 25 lines.
- **Spec constants**: halo3-nx's 920/921 are 924/925 here (banjo-nx's fetch types own 920-923).
- **A/B targets**: bangaio-nx's 69, 70, 73, 74 are 80, 82, 81, 84 here, and the fetch spec's 69 is 83 (katamari-nx's and
  kameo-nx's own those numbers).
- **Vertex kill**: vivapinata-nx's fetch spec commit has its own copy of port 13's switch and getter: defined once, in
  ultimate.cpp.
- **Resolve readback and hand-overs**: the colour hand-over leaves a resolve undrawn, so it skips a 1x1 R32F destination the CPU
  reads back (35).
- **Fetch-only textures and rr6-nx's fetch over pointer**: port 04 answers first (a slot whose pointer SetTexture(NULL) cleared
  keeps its last texture), so 37 is for slots no SetTexture ever set.
- **Hand-overs**: kameo-nx's leave a resolve in the target's image. The 16-bit clamp (28) skips them; the depth restore (29) and
  the view encode (30) draw a handed resolve out before writing the image.
- **Gamma ramp**: kameo-nx's 8 bits and halo3-nx's 1024 entries together.
- **The fetch signs** go inside the tfetch helpers, not around the fetch in the shaders' text, which tools/early_alpha.py parses.

## Needs the game's own function address

The base binds its D3D hooks to Katamari's addresses, so these are hook bodies a game wires to its own XDK function:
- sasrt-nx `sasrt_renut_constant_f4_pending`: in the base as `KeepConstantF4Pending` (port 34).
- sasrt-nx `sasrt_renut_splice_ended`, `sasrt_renut_pool_wrap_run`: command buffer splices and pool wraps. Not in the base: they
  sit in sasrt-nx's own command buffer hooks (sasrt_renut_hooks.cpp).
- sasrt-nx `sasrt_renut_vblank_pacing`: Swap released at the presentation interval, in its own hooks. pacman-nx's display pacing
  does it in the renderer and is in the base (p1).
- sasrt-nx `sasrt_renut_run_at_splice`: off there, and in its own command buffer hooks. Not in the base.

## Reported by sasrt-nx after its move to the base (2026-11-08), not fixed here

- **h1's inheritance and captured runs:** halo3-nx's replay inheritance (`BuildInheritRuns`, guest/command_buffers.cpp)
  crashed 5.7 s into a sasrt-nx run that replays captured runs from its same-frame slots: a captured run shares its
  HostCommandBuffer with the live table, so the render thread built `inherit_runs` while the game thread re-recorded (and
  cleared) the same buffer. sasrt-nx ships `banjo_renut_replay_inherits = false`. The fix: keep the inheritance data with
  the captured run. Katamari and Ridge Racer 6 record no command buffers; the captured runs are sasrt-nx's own (its same-frame slots),
  so Halo 3's replays do not meet it.
- **memory-export seeding and the copy engine:** `PrepareMemExportLocked` seeds each export window with a copy-engine copy
  (a full barrier before it), but the barrier after it is GRAPHICS only, a 3D wait-for-idle that does not wait for the copy
  engine: on the console an export draw's vertex shader can write the window before the seed lands, and the seed then
  overwrites the exports. `UploadVertexFetchData`'s exported fetch-data copies likewise (GRAPHICS_AND_COMPUTE after).
  sasrt-nx moved both, and `FlushExportReadbacksLocked`'s, to a compute shader on the 3D engine (`fetch_copy_cs`,
  `sasrt_gpu_fetch_copy_compute`), which also turned about 23 full barriers a frame into 3D waits. Ryujinx serialises the
  engines, so only a console shows it. Matters for games that seed memory exports, Halo 3 above all.

## Left out, and why

- **DXN swap**: disputed (the SDK's Xenia texture cache loads k_DXN as BC5 with no swap; Kameo's notes: no independent evidence).
- **kameo-nx's `banjo_renut_poly_offset`**: the same registers as port 08.
- **rr6-nx's alpha near one**: a threshold move, not the GPU's rule, and not measured in either port.
- **rr6-nx's setter release order**: rr6-nx's own XDK 2135 setters.
- **sasrt-nx's resolve target pages**: one host texture across alternating base pages; a game reading the other page's last
  frame would read this one's.
- **sasrt-nx's 16-bit scale** (`_fixed16_scale`): the emulated GPU's fast-path form, off in sasrt-nx.
- **sasrt-nx's ring frame cap**: the same idea as port 32.
- **sasrt-nx's vertex expand tables**: banjo-nx's packed component tables, in the base already, do the same.
- **hexichd-nx's delta runs**: the same idea as bangaio-nx's delta masks (b1).
- **hexichd-nx's object and shadow caches**: they serve the XDK 2135 adapter layer of rr6-nx and hexichd-nx (the game's own
  objects), which the base does not have.
- **kameo-nx's 2 MB pages**: the base has katamari-nx's later version (701c9d32cf).
- **The two-image swapchain** (pacman-nx, nights-nx, from sa2-nx): a latency against stutter choice per game, not a fix. Its
  switch is in the base with p1, off.
- **sasrt-nx's deko3d memory budget**: banjo-nx's (used plus the heap's room, 2026-10-22) is in the base already.
- **The profiler parts of pacman-nx 72c28ced60**, **halo3-nx b39c4a5ec5's codegen tool and diagnostics**: tools of those ports.
- **kameo-nx df7ca54a52's other switches**: in its hunk's context only; in ultimate.cpp already or left out above.

## Not yet

- **the game-gamma ramp's present cost** (ports 27 and k10): 0.42 ms a frame more than rr6-nx's master in Ridge Racer 6's
  race; the ramp costs 0.58-0.76 ms where master's costs 0.04. The table's form, the upload, the shader's structure and its
  registers are ruled out (the RR6 section above).

- **real occlusion queries** (kameo-nx): needs a per-game `D3DQuery_Issue` hook.
- **the last pixel shader** (kameo-nx): depends on a Kameo shadow-pass flag.
