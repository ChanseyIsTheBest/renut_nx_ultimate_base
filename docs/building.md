# Building katamari-nx

You need your own copy of the game: the code generator and the shader tools read it. Nothing made from it is committed
(`.gitignore`): `app/assets`, `app/generated`, `out/`, `app/out/`.

## Tools

Windows; LLVM clang and the Visual Studio Build Tools (CMake and Ninja) for the PC build (`tools/pc_env.bat` names the
paths); devkitA64, libnx and deko3d from [devkitPro](https://devkitpro.org) for the Switch; Python 3 with `xxhash` and
`Pillow`. The deko3d host tools (`out/deko3d_tools`: SPIRV-Cross and the PC uam) come from `tools/deko3d_build_tools.sh`.

## 1. The game

Extract the disc (`tools/extract_iso.py`) and make `app/assets` point at the folder that holds `default.xex`.

## 2. The PC build: code generation and the shader translator

```bat
tools\build_host.bat
tools\build_pc.bat
```

The PC build runs the code generator (`app/renut_manifest.toml`), the post-codegen passes (`tools/post_codegen.py`) and
Renut's XenosRecomp over `out/renut_shader_dump`. Run it with `tools/run_pc.ps1`.

## 3. The shaders

```bash
python tools/renut_shader_dump.py --image out/renut_image.bin --out out/renut_shader_dump
python tools/katamari_files.py --image out/renut_image.bin --game <game folder> --shaders out/katamari_file_shaders --known out/renut_shader_dump
```

The first takes the 111 containers stored in `default.xex`, the second finds the 84 in the models of `d02.dat`-`d05.dat`
(copy them into `out/renut_shader_dump`). Then:

```bat
powershell -File tools\switch_shaders.ps1
python tools\renut_release_shaders.py
```

`switch_shaders.ps1` translates all 195 to GLSL and makes the everyday build's files (`app/out/renut_switch`, every
shader built into the NRO). `renut_release_shaders.py` makes the release build's (`app/out/renut_switch_release`: the
shader table without SPIR-V, and `sd/deko3d_glsl.bin` to put next to the NRO for tests). `BANJO_CENTROID=0` (or
`switch_shaders.ps1 -NoCentroid`) translates without centroid sampling.

## 4. The Switch build

The everyday build (`app/out/switch`, every shader built in, no LTO):

```bat
tools\build_switch.bat
```

The everyday and released NROs carry only the running form of each machine copy (port 78, sasrt-nx's idea 27: CMake
`SASRT_HOT_GUARD_FORMS`, OFF by default on the Switch and ON on the PC): every copy is called directly, and
`banjo_cpu_verify` / `banjo_hot_guard_*` check none of them (the log says so). For a guard or verify run on Ryujinx or
the console, the guard build (`app/out/switch-guard`, otherwise the everyday build's options):

```bat
tools\build_switch_guard.bat
```

then `tools/run_ryujinx.ps1 -Build switch-guard -Toml <file>`. `GUARD_FORMS=ON` (or `OFF`) sets the option in any
folder `tools/build_switch.bat` builds; `tools/build_switch_release.bat` holds its folder at OFF unless `GUARD_FORMS=ON`
(sasrt-nx's `-Final` and `-GuardForms`).

The released NRO, in folders of their own (`BUILD_DIR` and `CMAKE_EXTRA` of `tools/build_switch.bat`):

1. **Instrumented:** `-DBANJO_PGO=generate -DBANJO_SWITCH_LTO=ON`. Play it: it writes `pgo/banjo_pgo_<N>.stream` next to
   the NRO every 2 minutes, at HOME and at exit.
2. **Merge** on the PC that built it (the streams name its folders): `python tools/pgo_merge.py <folder with the
   streams>` fills `pgo/` (it stops if `pgo/` already has `.gcda` files: move them aside first).
3. **Optimised:** `-DBANJO_PGO=use -DBANJO_SWITCH_LTO=ON -DBANJO_RENUT_RELEASE_SHADERS=ON`. It carries no game shader and
   reads `deko3d_glsl.bin` beside itself. `pgo/` in this repository is the released build's profile. A function changed
   since the instrumented build keeps no profile.

Keep the temporary folder's path free of spaces (GCC's parallel LTO link writes a makefile with it unquoted). Each build
folder keeps its own SDK libraries (`<folder>/rexglue-sdk/out`, renut-ultimate's `REXGLUE_OUTPUT_IN_BUILD`), so the
stages no longer replace each other's, and the SDK runtime and FFmpeg get the folder's LTO and PGO options
(`BANJO_SWITCH_SDK_OPT`). Their counts need an instrumented run of this build: the profile in `pgo/` has none.

The fixed settings are `app/katamari_switch_defaults.toml`, compiled in; the player's are `app/katamari_switch.toml`,
written next to the NRO as `katamari.toml` when it is missing. A key added to `katamari.toml` overrides a fixed one (the
test runs use that: `tools/run_ryujinx.ps1 -Toml <file>`).

## 5. The installer

[renut_nx_ultimate_installer](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_installer) is the web installer
for any port of this base (2026-11-10): a port forks it, writes its `game.json` and fills `release/` with
`tools/make_release.py` from its tree. It makes `deko3d_glsl.bin` (version 2, with the shader table: the NRO is built
with `-DBANJO_RENUT_RELEASE_SHADERS=ON` and carries no game shader) and the DKSH cache from the player's files, with this
base's translator and passes, and packs the archives `game.json` names as `.lz4b` (port 61). Its `wasm/renut_hlsl` and
`wasm/uam` are built from this tree: rebuild them after a change to the translator, its front end or uam, and run its
`tools/sync_from_base.py` after a change to the Python passes. Its README has the steps; Katamari is its example.

The older page, [katamari-nx-installer](https://github.com/ChanseyIsTheBest/katamari-nx-installer) makes the same `deko3d_glsl.bin` in the
browser. For a new release:

```bash
python tools/katamari_shader_files.py --image out/renut_image.bin --game <game folder> --wanted out/renut_shader_dump --out out/katamari_shader_files.json
```

then its `test/release_node.mjs` (its README). `wasm/renut_hlsl` must be rebuilt after a change to the translator
(`app/thirdparty/reblue-XenosRecomp`, `tools/renut_hlsl_frontend.cpp`): its `wasm/src/deko3d/build_wasm_deko3d.sh`.
