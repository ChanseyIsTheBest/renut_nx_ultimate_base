# Prompt: port an Xbox 360 game to the Switch

A fill-in prompt for starting a new port in a coding-agent session. Replace each placeholder below, then paste the whole
block into a new session.

It starts from this repository, the ultimate base
([renut_nx_ultimate_base](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_base)): katamari-nx with the universal
fixes and optimisations of the other Renut ports folded in (Beautiful Katamari is its example game). The method docs it
names are renut-nx's ([banjo-nx](https://github.com/ChanseyIsTheBest/renut-nx)'s `docs/`), which keeps the same prompt. Its web installer template is
[renut_nx_ultimate_installer](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_installer). To start from another
port instead, change the base's name and the pages of step 1 of the reading list.

| Placeholder | What it is | Example (the Pac-Man port) |
|---|---|---|
| `<GAME>` | The game's name | Pac-Man Championship Edition DX |
| `<SHORT>` | A short name for the port's folder, notes and Ryujinx data | `pacman` |
| `<TITLE_ID>` | The game's title ID | `58410A8C` |
| `<STOP_TIME>` | When the session stops | 6:00 am (AEST) |
| `<GAME_FILES>` | The game's package or files | `D:\pacman\PAC MAN CE DX\58410A8C\000D0000\<package>` |
| `<UPDATE_AND_DLC>` | Its title update and DLC packages, or "none" | `D:\pacman\PAC MAN CE DX (1)\PAC MAN CE DX\58410A8C\00000002\AllinOnePack` |
| `<WORK_DIR>` | The folder this port's files go in | `D:\pacman` |
| `<BASE_DIR>` | Your copy of renut_nx_ultimate_base | `D:\renut-ultimate\base` |
| `<INSTALLER_DIR>` | Your copy of renut_nx_ultimate_installer | `D:\renut-ultimate\installer` |
| `<RENUT_DIR>` | Your copy of renut-nx (the method docs) | `D:\Banjo kazooie port\banjo-nx` |
| `<RYU_DATA>` | The folder of Ryujinx data folders | `D:\New folder\ryu-data` |
| `<SD_FOLDER>` | The one folder on the console's SD card this port may touch | `sdmc:/switch/pacman-nx` |

```text
Port the Xbox 360 game <GAME> to PC and the Nintendo Switch with ReXGlue, starting from the ultimate base
(renut_nx_ultimate_base). Our primary objective is the Switch port; the PC port is only an intermediary step.
Work uninterrupted until <STOP_TIME> without stopping to ask me anything: make sensible decisions, write them down, and
keep going. Do the work in this main session (no subagents).

Inputs
- Game files (title ID <TITLE_ID>): <GAME_FILES>
- Title update and DLC: <UPDATE_AND_DLC>
  Extract packages (LIVE, PIRS or CON) with the base's tools/stfs_extract.py into <WORK_DIR>\game, <WORK_DIR>\update and
  <WORK_DIR>\dlc, outside the repo. Game data is never committed, uploaded or distributed.
- New port folder: <WORK_DIR>\<SHORT>-nx (create it as a new git repo from a copy of the base's files, not its history;
  commit locally as you go, with my GitHub noreply address; never push unless I ask).
- Base, read-only (do not modify it): <BASE_DIR>. Web installer template, read-only: <INSTALLER_DIR>.
- Method docs, read-only (the base was built on renut-nx and does not carry them): <RENUT_DIR>\docs
- Ryujinx data folder for this port: <RYU_DATA>\<SHORT> (make it from <RYU_DATA>\katamari's system folders, keys and
  firmware, not its sdcard).

Read first, in this order
1. <BASE_DIR>\README.md and docs\building.md (the build, the code generation, PGO), then docs\ultimate-fixes.md (one row
   per fix the base took from another port, with how it was tested) and docs\off-by-default.md (what is off and why: the
   keys that fix one game's picture and break another's, the forms that are not exact, the console's measurements).
2. <BASE_DIR>\docs\katamari-notes.md and docs\codegen-crash-fixes.md: the example port's findings and the generator's
   known crashes. docs\xdk-notes.md for the XDK.
3. <BASE_DIR>\docs\optimising-functions.md and docs\hand-and-machine-optimising-guide.md, all of both: the hot-function
   method. Their numbers are other games'; their rules and tools apply to this port. Then docs\sasrt-batch-cc2.md (the
   machine copies' register liveness, and the calling-convention traps).
4. <RENUT_DIR>\docs\porting-start-here.md: the reading order, what is current, the method rules, the build and test
   essentials of this PC.
5. <RENUT_DIR>\docs\porting-guide-renderer.md, all of it. Section 6b lists the items measured SLOWER ON THE CONSOLE:
   the base already has them off; keep them off.
6. <RENUT_DIR>\docs\enhancements-reference.md: the table at the top, then the keys this game uses.
7. <RENUT_DIR>\docs\deko3d-renderer.md, record-time-draw-path.md, codegen-enhancements.md, pgo-lto.md,
   deko3d-shader-optimisations.md, shader.md, controls-and-keyboard.md, and title-update-3.md if the game has a title
   update or DLC.
8. https://github.com/ChanseyIsTheBest/peggle2-nx/blob/main/docs/porting-a-360-game-prompt.md: the first ReXGlue port's
   order of work. It is older. Where docs differ, the ultimate base's win, then renut-nx's.
9. <RENUT_DIR>\docs\xdk-versions.md: the XDK build each of our ported games was made with.

The work, in order
1. Code generation and the PC port. Set up the project from the base's structure (CMake, the SDK, tools, the
   post-codegen passes). Take out what belongs to Katamari first: its function addresses and hooks
   (app/src/katamari_keys.cpp and the hook files), app/src/cpu/hot's lists, hand.txt and natives,
   app/indirect_promotions.txt, pgo/ and pgo-inplace/, its icon, its tomls and their generator
   (tools/make_katamari_tomls.py, app/katamari_switch*.toml, dist/switch/katamari-nx) and its DLC pack list (stale
   addresses also block copies: see "Stale mentions" in optimising-functions.md). Run code generation for <GAME>
   (tools/codegen.ps1, then python tools/post_codegen.py in full), and get it booting to gameplay on the PC with Renut's
   renderer (the Vulkan back end). Fix what stops it (imports, kernel calls, file system, input). Record every
   game-specific fix in docs/<SHORT>-notes.md.
2. Check the calling convention before trusting the machine copies' liveness: run tools/cc_entry_census.py and
   tools/cc_exit_census.py on the generated tree. The entry census must say none; every read the exit census lists must be
   value-independent (look at each). If not, generate with SASRT_REG_LIVENESS_EXIT_ABI=0 (and HOOKS_ABI=0 for a vector
   hit) and write down why.
3. The native deko3d renderer for the Switch. The base already has Renut on plume's deko3d back end
   (app/src/renut_engine/gpu, app/thirdparty/plume, the uam shader pipeline, tools/switch_shaders.ps1 for the shader
   package, tools/deko3d_variant_cache.py for the DKSH cache). Get it drawing this game, with NVK not linked (deko3d
   only). Nothing Katamari decided carries over untested: the game-specific decisions of porting-guide-renderer.md
   section 2 ("What your game decides") each get a census first, including where this game keeps its shaders. The
   base's defaults to settle for this game:
   - SMAA is on (banjo_renut_smaa) but runs only right after the draw whose pixel shader is in
     banjo_renut_smaa_after_ps; until this game's final composite (the last full-screen draw before the HUD) is listed
     there, the 10 s line says "SMAA ... 0 frames". Find it with banjo_renut_diag_draw_log_s in gameplay, not a menu.
   - MSAA is off (msaa_level 1). The game's own count is a decision for me (it changes the picture).
   - The keys of off-by-default.md section 1 fix one game's picture: turn one on in this game's toml only after its
     own A/B shows the same fault.
4. Test in Ryujinx. Build the NRO and run it with tools/run_ryujinx.ps1 and this port's own data folder, to the title
   screen and an attract loop or first gameplay, with captures every 15 s and the verify keys on. Fix every visual
   difference against the PC build. A new shader package means the device compiles its variants on the first run: make
   the DKSH cache again before judging hitches.
5. The enhancements. The base has renut-nx's and the other ports' CPU, GPU and shader enhancements on by default except
   those off-by-default.md lists; each still gets checked on this game (its key and in-run A/B target, in the emulator
   with the verify keys: 0 differences). The section 4 trials (vivapinata_shader_noearly's A/B 151 and the like) are per
   game: measure them on the console before turning one on. The ones that change the picture become configs for me.
6. Hot functions, by the base's optimising-functions.md. Its two tracks replace any fixed top-5 or top-100 target.
   - Machine-optimise as many functions as possible, at every opportunity: every function any session samples (Ryujinx
     and the PC's profiler now, the console when I send a log), with tools/localize_native.py and
     tools/switch_hot_bulk.py --merge, then further eligible functions in batches, watching NRO size, free heap and build
     time. Profile in Ryujinx as well as on the PC, because we are optimising for ARM.
   - Hand-write a function only from console profile data, past the guide's gate, with the evidence recorded in hand.txt.
     A PC or Ryujinx profile never qualifies, however clear it looks, so expect no hand-written natives until I send a
     console profile.
   Verify each against the generated code before it is on (tools/run_pc_verify.ps1: 0 differences, with the counts showing
   it was reached; a guard build with banjo_hot_poison_dead on shows a wrong liveness claim the guard cannot see).
7. PGO + LTO for the Switch build only when I say, in the guide's order: the last batch of machine copies, any
   hand-written natives, the instrumented build, my console session, then the optimised build (building.md). Nothing in
   the tree changes between the training race and the final build; a fix found after training means a new training run.
8. A package for my console by <STOP_TIME>:
   - the NRO
   - renut.toml with the shipped settings
   - renut_play.toml with the diagnostics off
   - renut_ab.toml with 30 s halves and a do-nothing control target
   - the ELF and a source snapshot, so a console profile maps to source lines
   - notes on what to test, and how to turn on a console profile
   And, if the port boots to gameplay, its web installer page: a copy of <INSTALLER_DIR> with this game's game.json and
   tools/make_release.py's release folder (the player's own game files make the package in their browser).
9. Fixes that are not this game's own (an SDK bug, a renderer fault any game could hit, a faster exact form): write a
   handoff for the ultimate base (a patch against <BASE_DIR> and a note: what, why, how it was tested, its key and
   default) in <WORK_DIR>\out, and name it in the final report.

Rules
- Pixel-identical by default. Anything that changes the picture is a separate config and my decision.
- The console decides performance. The PC and the emulator are proxies: judge on the A57 at 1020 MHz. Never compare load
  times or frame rates between emulator runs.
- A console A/B is the same NRO both ways (an in-run A/B or ABBA order) with a control from the same session: the
  console's speed drifts a few percent between sessions and after a reboot. Read fps from Renut's wall-clock 10 s lines;
  the profiler's per-second counters can read high when core 3 is busy.
- Remove work, do not move it to another thread or core.
- The console and the PC are shared with other port sessions. Arrange console time with them, never take it mid-run.
  On the console's SD card touch only <SD_FOLDER>, and delete nothing there.
- One Ryujinx run at a time, and only when none runs and nothing is compiling (a run starved by another session's build
  is void: run it again). Stop only your own run's process, never Ryujinx by name. Check free commit memory (8 GB or
  more) before a run. Restore the emulator SD's toml after each run.
- Never run two builds, never build during an emulator run, never edit sources while a build compiles. Run
  memory-heavy tools (a difftest of a large shader with an export window grew to 10 GB) one at a time and watch memory.
- Check disk space before code generation, builds and packages. If the disk is full, stop and tell me; do not delete
  anything outside this port's own folders.
- No downloads, no system or firewall setting changes, no git push without my say. Before anything is published,
  check it for personal details (names, email addresses, home or user paths, home-network addresses): commits use my
  GitHub noreply address.
- Bash heredocs mangle backslashes: write files with the editor tools.
- Credit Renut's renderer (its author permits reuse in my ports), the ultimate base and the ports whose fixes it
  carries (its README, THIRD_PARTY_NOTICES.md and docs/ultimate-fixes.md name them), renut-nx, sa2-nx, NetherSX2_nx,
  battd_nx and the bejeweled3-nx / zuma ports. GPL-3.0 where the sources are.
- Keep docs/<SHORT>-notes.md and a progress log current as you go, so the final summary is accurate.

At <STOP_TIME> stop, and report:
- what works on the PC, in the emulator, and in the package
- what was measured, and how (same-session control or not)
- what is next
- the fixes handed off for the ultimate base
- anything that needs my decision
```
