# Prompt: rebase an existing port on the ultimate base

A fill-in prompt for moving a game's existing Switch port onto this repository, the ultimate base
([renut_nx_ultimate_base](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_base)). It does a full rebase: the
port's new branch starts as the base's whole tree, and only what the game needs is grafted onto it from the original
project. Then it does the not-exact code generation work with its censuses. Replace each placeholder below, then paste
the whole block into a new session. For a new port with no project yet, use
[porting-a-360-game-prompt.md](porting-a-360-game-prompt.md) instead.

A port that has already been rebased (its `docs\ultimate-sync.md` names the base commit it is at) does not need this
prompt again: it takes the base's later ports with `tools/ult_port_sync.py --range <that commit>..<base commit>`
([building.md](building.md), section 7).

| Placeholder | What it is | Example (sa2-nx, Sonic Adventure 2) |
|---|---|---|
| `<GAME>` | The game's name | Sonic Adventure 2 |
| `<SHORT>` | The port's short name, for its notes and data folders | `sa2` |
| `<PORT_DIR>` | The port's git checkout (the original project) | `D:\sonic adventure 2\sa2-nx` |
| `<SHIPPED>` | Its shipped branch, its last release and that commit | `main`, 1.0.6 (`baf523b`) |
| `<WORKTREE>` | The new git worktree the rebase is done in | `D:\sonic adventure 2\sa2-nx-ult` |
| `<TITLE_ID>` | The game's title ID | `58411202` |
| `<XDK>` | The Direct3D library version the game links (`tools/xex_info.py default.xex`, or the port's notes) | `2.0.21076` |
| `<GAME_FILES>` | The extracted game files the port's manifest reads | `D:\sonic adventure 2\sa2-nx\assets\game_root` |
| `<UPDATE_AND_DLC>` | Its title update and DLC, or "none" | the Battle DLC, `...\58411202\00000002\SonicAdventure2Battle` |
| `<REFERENCE_NRO>` | The last release's NRO: the reference for every comparison | `D:\sonic adventure 2\sa2-nx\out\release_1.0.6\sa2.nro` |
| `<PLAYER_FILES>` | What a player's install holds, which must keep working | `switch/sa2-nx`: `sa2.nro`, `sa2.toml` |
| `<SD_FOLDER>` | The one folder on the console's SD card this port may touch | `sdmc:/switch/sa2-nx` |
| `<CONSOLE_IP>` | The console's address on your network (Appendix A of the porting prompt) | `192.168.0.20` |
| `<RYU_FROM>` | The port's own Ryujinx data folder | `D:\New folder\ryu-data\sa2` |
| `<RYU_NEW>` | The rebase's Ryujinx data folder, made from it | `D:\New folder\ryu-data\sa2-ult` |
| `<PORT_DOCS>` | The port's own notes on its game, renderer and CPU work | `docs\sa2-notes.md`, `renut-renderer.md`, `cpu-rounds.md` |
| `<PORT_INSTALLER>` | The port's web installer checkout, or "none" | `D:\sonic adventure 2\sa2-nx-installer` |
| `<CENSUS_SCENES>` | The gameplay that stresses the CPU and the floats | the speed stages, the treasure hunts, the shooting stages, a boss, the kart race, Chao Garden, a long session |
| `<BASE_DIR>` | Your copy of renut_nx_ultimate_base | `D:\renut-ultimate\base` |
| `<INSTALLER_DIR>` | Your copy of renut_nx_ultimate_installer | `D:\renut-ultimate\installer` |
| `<RENDERDOC_DIR>` | RenderDoc 1.46, the portable 64-bit build (Appendix B of the porting prompt) | `D:\tools\RenderDoc_1.46_64` |

```text
Rebase the Xbox 360 game <GAME>'s Switch port (<PORT_DIR>, <SHIPPED>) on the ultimate base (renut_nx_ultimate_base),
so it has every fix and speed-up the base took from the other ports, and so the base's later ports come in with its
sync tool. Then do the not-exact code generation work: run every census, and turn on the not-exact forms this game's
censuses clear. Our primary objective is the Switch port; the PC port is the intermediary step and the census machine.
Work uninterrupted without stopping to ask me anything: make sensible decisions, write them down, and keep going. Do
the work in this main session (no subagents). When the work below is done, report (the end of this prompt).

The method: a full rebase, not a merge
- The new branch starts as the ultimate base's whole tree: its SDK, its renderer, its tools, its build, its docs. That
  is the project from now on. The original project's structure, its SDK and renderer copies, and its versions of the
  files the base also has are not kept.
- From the original project, graft onto it only what this game needs and the base does not have: its game layer (the
  code and config that exist for this game alone) and any piece of general work the base has in no form at all. Every
  piece is one commit, with a line in the plan saying why it is needed.
- Where the base has its own form of something the original project also did (often a later form, taken from this
  very port or from another), the base's form wins and the original's is left behind. Where a shared file needs this
  game's lines, take the base's file and put only those lines back.
- Nothing is grafted because it was there: a piece the game runs without (tested), or one the base does better, stays
  behind. The plan lists every piece left behind, with the reason.

Inputs
- The original project: <PORT_DIR> (git, shipped branch and release: <SHIPPED>). The shipped branch is never changed and
  never pushed. Do the rebase in a new git worktree, <WORKTREE>, on a new branch `ultimate` (git worktree add -b
  ultimate), and make its first commit the base's tree ("the ultimate base at <commit>"). Compare that commit's `git
  ls-files` with the base's: files the base tracks past an ignore rule (FFmpeg's configure-made headers, port 190) are
  lost by a plain copy and `git add -A`. Commit locally as you go, with my GitHub noreply address; never push unless I
  ask.
- The base, read-only (do not modify it): <BASE_DIR>, branch ultimate, its latest commit (docs\changelog.md has a line
  per port). Web installer template, read-only: <INSTALLER_DIR>.
- Game files (title ID <TITLE_ID>, Direct3D <XDK>): <GAME_FILES>. Title update and DLC: <UPDATE_AND_DLC>. If the
  worktree needs the game files inside it, make a directory junction to them, never a copy. Game data is never
  committed, uploaded or distributed. No downloads.
- The reference build: <REFERENCE_NRO>. Every picture and behaviour comparison is against it.
- Ryujinx data folder: make <RYU_NEW> from <RYU_FROM> (its system folders, keys, firmware and Config.json; check their
  size first). Do not change <RYU_FROM>, and do not copy large game files from its SD: use junctions (folders) or hard
  links (mklink /H, files you never write to) to what is already on the disk.
- The original project's own notes on its game: <PORT_DOCS>.

The console and RenderDoc (<BASE_DIR>\docs\porting-a-360-game-prompt.md, appendices A and B, has the details)
- The console at <CONSOLE_IP>: only with my word, through <BASE_DIR>\tools\switch_remote.py with --root <SD_FOLDER>
  (it refuses any other path on the card and has no delete command), arranged with the other port sessions that share
  the console. Do not touch anything else on the card, and do not delete anything there. Before pressing A on the Home
  menu, read the selected tile's name on a screenshot (the forwarders' icons look alike); after the launch, check the
  running title ID; on a wrong title, stop pressing.
- RenderDoc (<RENDERDOC_DIR>) for every picture difference: capture the frame (the PC build with
  banjo_renderdoc_game_s, or Ryujinx's Vulkan back end with tools\run_ryujinx_rdc.ps1), read it with the tools\rdc_*.py
  scripts (tools\rdc_run.ps1), and find the draw and the cause in the capture before changing code.

Read first, in this order
1. <BASE_DIR>\README.md, docs\README.md and docs\building.md (the build, code generation, section 7 on taking the
   base's ports into a port, PGO), then docs\changelog.md (a line per port) and docs\ultimate-fixes.md (each port's
   row: where it came from, how it was tested, its key and default).
2. <BASE_DIR>\docs\off-by-default.md, all of it: section 1 (fixes for one game that break another's picture), section 2
   (the faster forms that are not exact: the second half of this work), sections 3-6.
3. <BASE_DIR>\docs\porting-a-360-game-prompt.md: the base's order of work for a new port. Its steps for the CG-13
   census, the calling-convention census, the three keys decided from the log and the stereo check apply here.
4. <BASE_DIR>\docs\katamari-notes.md (what the base changed for its example game, Beautiful Katamari, at XDK 2.0.5632),
   docs\xdk-notes.md (Renut at other XDKs: find the nearest to <XDK>) and docs\codegen-crash-fixes.md.
5. <BASE_DIR>\docs\optimising-functions.md and docs\hand-and-machine-optimising-guide.md, then docs\sasrt-batch-cc2.md
   (the machine copies' register liveness, and the calling-convention traps).
6. The original project's <PORT_DOCS>, its README and its build notes: what its game layer does and why.

The work, in order
1. The inventory, before any code moves: docs\ultimate-rebase-plan.md in the worktree, the list of what is grafted and
   what is left behind.
   - The lineage. Is this port one the base took work from (the base's README credits table, ultimate-fixes.md's
     rows, a search of the base for the port's name)? Did the port start from banjo-nx / renut-nx, from the base's own
     ancestors, or from its own code? Where it shares an ancestor with the base, find the commits (git log on both)
     and compare the port then, the base then and the base now. Much of an older port's work can already be in the
     base in a later form: it is not grafted again.
   - Each piece of the original project, sorted into three lists:
     a. this game's own layer, to graft: the hooks and device layout at the game's addresses and <XDK> (the base keeps
        per-XDK files, docs\xdk-notes.md), the manifest and the code generation config (functions, overrides with
        their evidence, switch tables, setjmp / longjmp, the CRT), the hot lists, hand-written natives and indirect
        promotions, title update and DLC code, the game's own fixes and enhancements, its tomls, the icon, the player's
        folder;
     b. general work the base already has in some form: left behind, with the base's form named (its port number);
     c. general work the base has in no form: grafted (and written up as a handoff for the base, step 11), or left
        behind with the reason.
   - The original project's work since the point the base last took anything from it: each piece sorted the same way.
   - The base's Katamari layer, which goes: app\src\katamari_keys.cpp and its hooks, the Katamari lists and natives,
     pgo\ and pgo-inplace\, its tomls and tools\make_katamari_tomls.py, dist\switch\katamari-nx, its DLC pack list, its
     shader file records.
   Record the base commit the branch starts from (docs\ultimate-sync.md): later base ports come in with
   tools\ult_port_sync.py --range <that commit>..<base commit>.
   The player's install (<PLAYER_FILES>) must keep working: keep the folder, the NRO's name and the toml's name. The
   base's keys have other names (banjo_*, blur_*, sasrt_*, ...): list each key of the last release, its base
   equivalent, and what the rebased build does with the old line (reads it, or ignores it with a log line). Write the
   table in docs\<SHORT>-ultimate-notes.md.
2. The modules and the generated tree. If the game's manifest has [[modules]] (a second XEX or a DLL), the generator
   writes a folder per module: the base's post-codegen works on one flat folder (Katamari has one module). Decide how
   the passes run on each (post_codegen.py --gen per folder, or the full passes on the main module only: say which and
   why), and check that they find the files by the project's name (port 186), that pointer_fixpoint and direct_calls
   see every module, and that the cross-module calls still resolve.
3. Code generation and the PC port: tools\codegen.ps1 with the game's manifest and config (on a new tree it also writes
   app\generated\rexglue.cmake, which the PC build's configure includes), then python tools\post_codegen.py in full (a run whose generator says "up to date" writes nothing: use rexglue codegen
   --ignore-stamp when the generated text must be fresh). PC build, to gameplay with the title update and DLC. Fix
   what stops it; record each fix in docs\<SHORT>-ultimate-notes.md. Then tools\run_pc_verify.ps1 on the machine
   copies: 0 differences. Then python tools\built_ptr_scan.py (port 191): every code address the game builds in code
   that no translated function starts at is a callback that stops the game the first time it runs; register each real
   one with its evidence.
4. Check the calling convention before trusting the machine copies' liveness: tools\cc_entry_census.py and
   tools\cc_exit_census.py on the generated tree (every module). The entry census must say none; every read the exit
   census lists must be value-independent (look at each). If not, generate with SASRT_REG_LIVENESS_EXIT_ABI=0 (and
   SASRT_REG_LIVENESS_HOOKS_ABI=0 for a vector hit) and write down why. If the game has a setjmp / longjmp pair, check that the machine
   copies keep it working.
5. The Switch build and Ryujinx: the NRO with the game's shader package (tools\switch_shaders.ps1, the DKSH cache with
   tools\deko3d_variant_cache.py), run with tools\run_ryujinx.ps1 and <RYU_NEW>, to gameplay, captures every 15 s, the
   verify keys on. The picture must match the reference build's (the same scenes, and the PC build): every difference
   found in a RenderDoc capture first. A game that runs per frame slows down at a lower frame rate: compare its scenes
   by frame, not by time. The base's off-by-default.md section 1 keys: turn one on in the game's toml only after its
   own A/B shows the same fault. Keys the last release had on and the base has off, and fixes the original project
   made for its own scenes: check each against the base's form, one by one. The base's rule: a key that breaks any
   game's picture is off in the base, and the game that needs it turns it on in its own toml.
   The device layout: d3d.h's Device is Beautiful Katamari's XDK's, and every delta path copies the float constants from
   where it puts them (m_Constants +0x480, VertexShaderF +0x300). If the game's XDK differs, its layout goes into d3d.h,
   the one place every path reads, never path by path (a Gears of War port fixed two paths and left a third reading the
   wrong place). Check the layout guard's line: "delta layout guard ... retired" is right; its error line means the
   constants are elsewhere (renut_delta_layout_guard, 0 once the layout is known right).
   The three keys decided from the log (the base's porting prompt, step 5) and the stereo check: from this port's own
   logs in gameplay windows (the channels the game fills from step 6 a's census run; audio_speaker_config = 1, which
   changes the mix, a run of its own).
6. The not-exact code generation, with its censuses (do it thoroughly). The PC build counts what each form would
   change while it runs the exact forms (banjo_diag_runtime_census = true: the "[census]" lines every 10 s), so each
   census is a PC run in gameplay, not menus: play (or script with input_script) <CENSUS_SCENES>.
   For each item: the census numbers per window, the sites when there are any, a verdict, and a table row in
   docs\<SHORT>-near-exact.md. Then build with the cleared ones on, PC verify (0 differences), Ryujinx to gameplay (the
   same picture), and leave the console measurement (an in-run A/B or ABBA order, my word) as a next step. Where the
   original project already made a not-exact choice of its own, its evidence counts: write it next to the census.
   a. CG-13 (BLUR_FPCR_ONE_MODE: ON in the Switch build, OFF on the PC so the census can count), in the one census run
      that takes the rest too. Run `tools\cg13_census.ps1 -Seconds 300 -Extra "--ssasr_renut_host50_census=true
      --ssasr_submit_worker=true --banjo_renut_snapshot_deltas_verify=true --renut_delta_layout_guard_looks=32
      --audio_channel_census=true --banjo_diag_indirect_census=<WORKTREE>\out\indirect_<scene>.txt"` in each of those
      scenes (a census file a scene). 0 in every window: keep it on. Any count: the script lists the sites
      (tools\census_sites.py --denormal); look at what each computes. A value decaying to zero is harmless; a division
      or normalisation of it, a guard against zero, or a physics step that depends on it is not. If one matters, the
      Switch build gets -DBLUR_FPCR_ONE_MODE=OFF (tell me in the report).
      Read the same logs for the rest now, and write each in the table; the items below decide from these numbers and
      build only what a census build or a guard has to add:
      - b's "[census] near-exact (cumulative)" line, and the r1 mirror line ("[verify] r1 mirror checked after N
        calls ... 0 differences": the forms this build has; c's BLUR_NX_R1_ASSUME is checked again with it on);
      - "[census] ssasr non-exact": i's single operations with an input that is not a single (how far the relaxed
        result lands) and j's denormal singles through float copies;
      - the delta chain's chunks: "delta layout guard ... retired" (an error line stops the work here: step 5's
        device layout), the snapshot verify ("verify: N draws checked, 0 differ"), the "N18 census" (no staged draw
        with a state tag means no tag narrowing) and the capture pool's line;
      - "stack forwards ... checked against the real load: N; 0 differences" (SSASR_CALL_RELOADS is checked again with
        it on), and the indirect call census: `python tools\indirect_promotions.py --census <every scene's file>
        --gen app\generated` makes this game's app\indirect_promotions.txt again (against the original project's
        list: the rebased code's call sites);
      - "[audio] channels with sound" (step 5's stereo check).
      The submit worker is on for these runs only, for the N18 line; they are censuses, not speed measurements. The
      census build runs at about a tenth of the plain build's speed (Beautiful Katamari: some 150 draws a second, a
      white loading screen still at 77 s), so the guard gets 32 looks and the input script's presses come later than
      in a plain run. Beautiful Katamari's run of it (2026-11-28, 300 s): CG-13 0 in 26 windows, denormal singles 0,
      17,120 of 31.6 M single operations with a non-single input, the guard retired after 32 looks (188 changed
      groups, all marked), the verify 0 of 1,472 draws differ, no state-tagged staged draw, FL FR C filled, the r1
      mirror 0 differences in 21.5 M calls, 860,933 stack forwards with 0 differences.
      Not in this run, as each changes what runs or needs its own build: the stale-read and memo censuses (one PC
      census build can carry both, SSASR_STALE_CENSUS=ON and SSASR_MEMO_CENSUS=ON); the guard races (d, h, and i's
      SSASR_GUARD_RELAXED_FLAGS, a guard build of its own so its differences are the flags' alone); the poison race
      (k); the guard on ARM (Ryujinx); sasrt_test_vmx_fuzz; audio_speaker_config = 1.
   b. BLUR_NEAR_EXACT (CMake, off; port 140, blur-nx's): the "[census] near-exact (cumulative)" line counts each:
      #1 signalling NaNs at float copy stores (REX_NX_SNAN_PASS), #2 NaNs into float-to-integer conversions
      (REX_NX_FCTI_NO_NAN), #17 divisions by zero and INT_MIN / -1 (REX_NX_DIV_NO_CHECK), #41 float compares with a NaN
      (REX_NX_FCMP_NO_UN), #42 vector max/min lanes that differ (REX_NX_VMINMAX_FAST), #5 integer compares copying
      XER[SO] = 1 (REX_NX_XER_SO_ZERO), and #19 (REX_NX_NO_TRAPS: a trap only logs) by the trap lines. A switch whose
      count is 0 in every window is cleared; one that counts gets its sites looked at (what the game does with that
      value) before any decision. #2 and #42 only through BLUR_NX_ONLY (blur-nx took them out: 80 NaN conversions a
      second, 4 % of max/min lanes). Use BLUR_NX_ONLY / BLUR_NX_SKIP to build exactly the cleared set. The other
      "[census] near-exact" lines (#3 / #4 FPSCR writes and reads, #32 compare-and-swaps, #38 card marks) are the
      evidence for the related exact items: record them.
   c. BLUR_NX_R1_ASSUME=1 (post-codegen environment; #9, r1_preserve.py --near-exact): how many more functions it takes
      as preserving r1, then PC verify and the run: the r1 mirror check of the guard build must show 0 differences
      (ssasr-nx: 20,157 of 20,490 functions taken, against 8,264 proven; 391.6 M calls checked, 0 differences).
   d. BLUR_LOCALIZE_ABI_TRUST=1 (post-codegen environment; port 150): the machine copies trust the PowerPC ABI's
      non-volatile registers across calls. The case it changes is a callee that breaks the ABI (hand-written assembly,
      setjmp / longjmp): look for any in the game's code (step 4's censuses, the XDK's assembly routines), then the PC
      guard race with it on (banjo_cpu_verify, a dense guard): any non-volatile register difference fails it.
   e. The stack-frame elisions (BANJO_GEN_LR_MOVES_ELIDE, BANJO_GEN_NV_SAVES_ELIDE, BANJO_GEN_BACK_CHAIN_ELIDE;
      tools\frame_traffic.py) and SASRT_HOT_LR_ELIDE. sa2-nx's non-volatile saves elision tinted its characters: the
      game read a stack word it never wrote, every self-check passed, and only a lockstep frame comparison showed it.
      So these need a frame-exact test of every scene the PC can reach (a lockstep capture with and without, compared
      frame by frame), and SASRT_HOT_LR_ELIDE a guard run with 0 differences. The link register's half first; the
      non-volatile saves only if the capture is exact. First, the stale-read census (ssasr-nx's, port 380): a PC build
      with CMake SSASR_STALE_CENSUS=ON (tools\ssasr_stale_census.py) has the elided stores write 0xDEADBEEF and every
      4- and 8-byte guest load count reads of it; the "[census] ssasr stale" lines and RexStaleHit list the readers by
      return address (exit-block restores left out). Every reader must be a dead restore or the guard's own compare:
      classify all of them. One that uses the value (a stack walk through the back chain, a read of the link register)
      keeps that elision off.
   f. BANJO_GENERATED_NO_TRAPPING_MATH: -fno-trapping-math on the generated code. The files that write the guest FPSCR
      keep -ftrapping-math (tools\fp_env_files.py, app\generated\fp_env_files.txt): check that it lists every file
      where the host's float mode changes, in every module. PC verify and the census runs with it on.
   ssasr-nx's switches (ports 372-385; docs/off-by-default.md section 13, all off; its figures as examples):
   g. Loose loop barriers (CMake SSASR_NON_EXACT: REX_SSASR_LOOSE_BARRIERS, tools\ssasr_loose_barriers.py): run
      tools\find_self_loops.py on the Switch ELF and on one built with every barrier kept. A new branch to itself in a
      recompiled function is a poll GCC turned into an endless spin once its barrier was gone: a console hang. Put each
      such function in app\config\loose_barriers_keep.txt and build again until there are none (ssasr-nx had ten). The
      barrier is a compiler barrier only: the CPU's memory ordering does not change.
   h. NaN signs and loose stack forwarding (the same switch: REX_SSASR_NAN_SIGN_FREE, REX_SSASR_FWD_LOOSE): the guard
      race on the PC and the guard on ARM (tools\build_switch_guard.bat, in Ryujinx): x86 and ARM give a NaN its sign
      differently (ssasr-nx took two natives off for NaN signs only the ARM guard showed). The base's guard already
      shows a few NaN-sign-only differences in vector code (Beautiful Katamari: three, NaN inputs through vmaddfp /
      vnmsubfp): that class is known; any other difference is a fault.
   i. Relaxed floats (CMake SSASR_RELAXED_FLOATS: contraction, reciprocals, single operations in float, one Newton
      step on the VMX estimates). It can change the picture: my decision, after its censuses. Its count is a's run
      ("[census] ssasr non-exact": single operations whose inputs are not singles; ssasr-nx: 28,140 of 129 M, at most
      one ulp apart); what it adds here: CMake SSASR_GUARD_RELAXED_FLAGS=ON (the flags on the copies only, the guard
      against the exact originals; ssasr-nx: 72 differences in 67 copies, all a zero's sign) and sasrt_test_vmx_fuzz
      (vrsqrtefp within 196 ulp, vrefp within 129).
   j. The float copies' denormal flush (REX_SSASR_PURE_FLUSH): only where its census (REX_NX_CENSUS 40, a's run:
      "denormal singles through float copies") is 0 in every window; those copies move any 4 bytes, and a small
      integer is a denormal single (ssasr-nx: 467,812 a race: off).
   k. LiveOut trimming (exact by construction, but the liveness model has holes): the poison race
      (banjo_hot_poison_dead = true, banjo_hot_guard_calls = 0, banjo_hot_guard_period = 1048576), where registers
      dead after a return get garbage: the picture right to the end, only known artefacts.
   And the exact options that still need this game's check: SSASR_CALL_RELOADS=1 (post-codegen; checked with
   REX_STACK_FWD_VERIFY in the hooked builds); the submit worker (ssasr_submit_worker:
   banjo_renut_snapshot_deltas_verify 0 of the draws differ; it costs the render thread about 0.9 ms, so only where the
   game thread is the limit; ssasr-nx's console: -0.9 ms a frame at 1,200 draws); the setter log (ssasr_setter_log: the
   same check; ssasr-nx's console +1.2 ms a frame, so only with this game's own console A/B showing a gain);
   ssasr_draw_runs (Beautiful Katamari's PC renderer presented no frame with it: test it before anything else;
   ssasr-nx's console: +0.1 ms, nothing); ssasr_tags_narrow with this game's own ssasr_tags_narrow_map: first the
   "N18 census" line of a's run; no staged draw with a state tag (Beautiful Katamari: none)
   means nothing to narrow, so stop there. Otherwise it names the state tags that come alone; every store in the
   game's code that sets one gives its ranges; the D3D library's processors that set tags from run-time masks get a
   mid-asm hook calling native::MarkStateTagsWide; then the guard race with banjo_renut_tags_check = 1: no "changed
   without a mark" (ssasr-nx: no console gain). On by default, with a log line to check: ssasr_code_blocks_data
   (the data and bss on 2 MB blocks: "[code blocks] ... and data"; their size again in heap, so false in the player's
   toml for a game short of heap; ssasr-nx's console TLB test: no cost) and ssasr_capture_pool_mb (64; with
   ssasr_renut_host50_census its line shows whether the pool still makes and deletes captures).
   Less work on the PC is not a console gain: ssasr-nx's exact host changes (port 385) with N18 measured 30.6 against
   30.5 ms over four console races each. Take them for exactness and consistency, not speed.
   Also the measurement tools for where the generated code's time goes: tools\codegen_census.py and
   tools\generated_code_census.py (they want a console profile: list them for the console step) and
   tools\loop_body_census.py, tools\arm_cost_census.py on the build.
   Run a's census run (CG-13 and the rest) again after any large change to the code generation, and note in the report
   which forms are on.
7. Hot functions, by the base's optimising-functions.md: machine-optimise every function the game's profiles sample (the
   PC's profiler and Ryujinx now; the original project's console profiles and hot lists are evidence too), with
   tools\localize_native.py and tools\switch_hot_bulk.py --merge, verifying each (tools\run_pc_verify.ps1: 0
   differences, counts showing it was reached; a guard build with banjo_hot_poison_dead on). The original project's
   hand-written natives are grafted only where the base's machine copies do not already cover them, each checked
   against the generated code.
8. Smaller game files, if the last release has not done them: the movies to H.265 and the audio to Opus
   (docs/media-compression.md, all of it, before any code). Each re-encode is a stand-in, NAME.x, that the SDK shows to
   the game as NAME: the same size, every header, index and table byte as the game reads it, only the picture or sound
   data replaced (ASF movies are rewritten instead: section 1). It changes the picture and the sound: an option for me,
   ticked by default in the installer.
   - Measure first (the files by kind and size), and keep the guide's order and rules: the movies, raw PCM music,
     compressed audio (XMA, ADX), then LZ4 / ZXC packing of what compresses; the 95 % and 5 % rules, not smaller means
     not changed, small files plain, default.xex (and .xexp) never packed, re-encoded movies ZXC-packed (my rule).
   - The runtime: the base reads every stand-in kind (section 9); the movie hook is each game's, from the port whose
     container matches (section 5's "Container by container" and section 9's table), with the decode at priority
     0x2C, read-ahead on a libnx thread, and nxvideo's VIC copy and clocks kept. Where the original project already
     has its own media code, check it against the base's readers.
   - Make each kind on the PC for a test into a copy of the game folder (never pack a linked folder in place; never
     leave a plain NAME beside its stand-in); test on the PC (libopus-0.dll beside renut.exe), in Ryujinx with the
     software decoder (no H.265 NVDEC there), and on the console for the real decode (rr6-nx: about 5.4 ms of NVDEC a
     frame); watch every movie and listen to a sample of the sounds, loops and seeks included.
   - The installer's part (game.json's movies and audio, the writers of the matching port, its output equal to the PC
     tools' byte for byte): list it with step 10's installer list; change nothing there in this session. Game data is
     never committed, uploaded or shared. Record each kind's size before and after, and the decode's cost.
9. PGO + LTO: the original project's profile does not fit the new code (and a compile-time switch from step 6 needs a
   new one too). Leave the tree ready for an instrumented build and my console session (building.md), and say so.
10. A package for my console, built and checked in Ryujinx:
   - the NRO in the same folder and under the same name as the last release (<PLAYER_FILES>), so a player's install
     keeps working
   - the toml with the shipped settings, a play toml with the diagnostics off, an A/B toml with 30 s halves and a
     do-nothing control target
   - the ELF and a source snapshot, so a console profile maps to source lines
   - notes on what to test on the console (the last release against the rebased build, ABBA order) and how to turn on
     a profile
   The web installer (<PORT_INSTALLER>): list what it needs for the rebased build (the base's installer,
   <INSTALLER_DIR>, takes several games through game.json, title updates, and .lz4b packs, so the port could move onto
   it); change nothing there in this session.
11. Fixes that are not the game's own (an SDK bug, a renderer fault any game could hit, a faster exact form, a census
   finding that applies to all games, general work of list c): write a handoff for the ultimate base (a patch against
   <BASE_DIR> and a note: what, why, how it was tested, its key and default) in <WORKTREE>\out, and name it in the final
   report.

Rules
- Pixel-identical by default: the rebased build draws what the last release draws. Anything that changes the picture
  is a separate config and my decision.
- Not-exact forms only with their census done and written down; never on by guesswork. A form whose census counts is
  off until I decide, with the sites in the notes.
- The console decides performance. The PC and the emulator are proxies: record their numbers as such. Never compare
  load times or frame rates between emulator runs. A change the PC shows doing less work is a candidate, not a gain,
  until its console A/B (ssasr-nx's N18 took a tagged draw from 22 chunks to 13.6 and gained nothing there).
- Remove work, do not move it to another thread or core.
- The PC is shared with other port sessions. One Ryujinx run at a time, only when none runs (tools\run_ryujinx.ps1
  refuses to start beside another; never stop Ryujinx by name, only your own run's process). Check free commit memory
  (8 GB or more) before a run, and restore the emulator SD's toml after each run. Never run two builds, never build
  during an emulator run, never edit sources while a build compiles. A compile that runs out of memory beside another
  session's build: run it again with JOBS=6.
- Check disk space before code generation, builds and packages. If the disk is full, stop and tell me; do not delete
  anything outside this port's own folders.
- TOML: a key appended at the end of a file lands in its last table ([input_p2]); put top-level keys above the first
  table.
- No downloads, no system or firewall setting changes, no git push without my say. Never commit a manifest with local
  paths in it, or anything from the game. Before anything is published, check it for personal details (names, email
  addresses, home or user paths, home-network addresses): commits use my GitHub noreply address.
- Bash heredocs mangle backslashes: write files with the editor tools.
- Credit Renut's renderer (its author permits reuse in my ports), reNut (masterspike52), the ultimate base and the
  ports whose fixes it carries (its README, THIRD_PARTY_NOTICES.md and docs\ultimate-fixes.md name them), and everything
  the original project's THIRD_PARTY_NOTICES.md credits for the parts grafted. GPL-3.0 where the sources are.
- Keep docs\ultimate-rebase-plan.md, docs\<SHORT>-ultimate-notes.md, docs\<SHORT>-near-exact.md and a progress log
  current as you go, so the final summary is accurate.

When done, report:
- what works on the PC, in the emulator, and in the package, against the last release
- the inventory's outcome: what was grafted from the original project, what the base already had (in which form), what
  was left behind and why
- how the modules are generated and post-processed, if there is more than one
- each census: its numbers, the sites, the verdict, and which not-exact forms the build has on
- the media step (step 8): each kind's size before and after, the decode's cost, what the installer needs
- what was measured, and how (no console numbers unless I gave the word for a console run)
- what is next, with the console steps (the A/B of the rebased build against the last release, the cleared forms, PGO)
- the fixes handed off for the ultimate base
- anything that needs my decision
```
