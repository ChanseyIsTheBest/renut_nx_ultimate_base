# Renut at a later XDK: notes from other ports

The base's Renut stands at Beautiful Katamari's XDK 2.0.5632. These are what ports at a later XDK met, for the next
port's remap of the hooks and the device layout. Each is that port's finding on its own image; check it on yours.

## XDK 2.0.11164 (blur-nx, 2026-11-10)

Evidence: blur-nx's `docs/blur-notes.md`, "Renut at XDK 2.0.11164" (D:\blur\blur-nx), with the tools named there.

- **The first release's unused second argument.** D3D_Lock2DSurface, D3D_LockSurface, D3D_LockResource and
  D3DDevice_BeginCommandBuffer take one more argument than at 5632, unused. pacman-nx met three of them at XDK 21250;
  they are already there at 11164.
- **SetGammaRamp and SetPWLGamma** take (device, flags, ramp).
- **D3DVertexShader_Bind** is (shader r3, flags r4, declaration r5, stream strides r6, pixel shader r7). Katamari never
  links it, so it is one of the base's `BANJO_RENUT_UNHOOKED_*` bodies: left unhooked, Blur's ran on Renut's declaration
  record and walked off the guest stack. blur-nx hooks it through `D3DVertexShader_Bind11164_hook` (the flags and the
  pixel shader dropped: Renut keeps the declaration and the strides).
- **XGRegisterPixelShader** is linked and makes the pixel shaders where CreatePixelShader is not linked (Blur); its
  instructions write what Renut's hook writes.
- **The tiling Flags word.** pacman-nx's N&B -> 11164 device-offset table moves N&B's 13196 by +176; Blur's BeginTiling
  stores it at 13364, +168. Check that word against your BeginTiling.
- **The state tables.** `tools/renut_state_tables.py --ref-render 82AD8E48 --ref-sampler 82AD9308` pairs a game's render
  and sampler state tables with Katamari's (port 69's options); Blur's are 0x82B9A888 and 0x82B9AD48, all 121 records
  paired by their defaults.
- **Vertex shaders with two bindings.** Blur makes about 1,000 vertex shaders at run time whose container has flags bit
  0x20 (0x102A1121). XenosRecomp's dump reader skipped them before port 68.

## XDK 2.0.9407.0 (rehatched-nx, Angry Birds Trilogy, 2026-11-16; port 176)

What rehatched-nx found moving Renut's Direct3D hooks to this XDK (its docs/rehatched-notes.md section 4). Every
address below is Angry Birds'; a port finds its own (tools/xdk_sigmatch.py, the hook maps).

- **After any change to a hook address, run `tools/post_codegen.py` in full** (until `direct_calls.py --check`
  reports 0, and `tools/hook_bypass_check.py` (port 291) reports 0 bypassed). Otherwise the calls to the moved
  functions stay direct `__imp__sub_X` calls that skip the hooks: rehatched-nx's device was made the XDK's way and
  its Swap waited forever for a ring pointer no GPU moves.
- **Forms with Flags.** D3DVertexShader_Bind is the first release's (pShader, Flags, pDecl, pStreamStrides, Index):
  bind `D3DVertexShader_Bind_flags_hook` (port 173), not the three-argument hook. SetGammaRamp and SetPWLGamma keep
  Flags, (D3DDevice*, Flags, ramp): bind `D3DDevice_SetGammaRamp_flags_hook` and `D3DDevice_SetPWLGamma_flags_hook`.
  A port whose own hooks of those names already take Flags (blur-nx at XDK 2.0.11164) leaves out port 173's
  block of two-argument `D3DDevice_SetGammaRamp_hook` / `D3DDevice_SetPWLGamma_hook` declarations and their
  Flags wrappers (d3d/hooks/device.cpp): with both, the names are overloaded and BANJO_RENUT_HOOK does not
  compile (Blur's 909f520b).
- **D3DVertexDeclaration_GetDeclaration.** A game that asks for a declaration's elements gets zeros from the XDK's
  own (Renut's declarations are records with no elements in their guest block), and a search for D3DDECL_END runs
  off the stack (Angry Birds' crash at the end of level 1). Bind `D3DVertexDeclaration_GetDeclaration_hook` (port 173).
- **The internal resolve.** D3DDevice_Resolve (Angry Birds' sub_8228CA80) is a wrapper that calls an internal resolve
  (sub_8228B928) with two extra arguments, 0 from the wrapper; the game's renderer also calls the internal one
  directly, so those resolves never reached Renut (black level backgrounds). rehatched-nx maps it onto Resolve's hook
  with a raw hook; its arguments: r3 device, r4 flags, r5 pSourceRect, r6 a value it writes in a PM4 register packet
  and r7 a flag (both dropped), r8 pDestTexture, r9 pDestPoint, r10 DestLevel; on the caller's stack +84
  DestSliceOrFace, +92 pClearColor, +108 ClearStencil, +116 pParameters; ClearZ in f1:

      BANJO_RENUT_HOOK_RAW(sub_<internal resolve>) {
        const u32 sp = ctx.r1.u32;
        D3DDevice_Resolve_hook(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32,
                               LoadU32(sp + 84), LoadU32(sp + 92), ctx.f1.f64, 0, LoadU32(sp + 108),
                               LoadU32(sp + 116));
      }

  Check the layout against the port's own wrapper's call first. Katamari's XDK has no such split.
- **Device offsets** (d3d.h, d3d_native_state.cpp): vertex declaration +12240, pixel shader +13068, vertex shader
  +13072; the device's own (FVF) declaration +12568; the device allocation 0x6080.
- **Re-pointed headers.** Angry Birds re-points one surface header between three EDRAM views each frame: turn on
  `banjo_renut_all_surface_headers` (port 174) with `banjo_renut_chain_reuse` and `banjo_deko3d_scanout_handover` off.

## XDK 2.0.6534 (TFU-nx, The Force Unleashed) and 2.0.11626 (S4ep1-nx, Sonic 4 Episode I), 2026-11-11 to 11-16; ports 191-196

Evidence: [S4ep1-nx](https://github.com/FurryLilManChild/S4ep1-nx) (GPL-3.0, 0.5.1), which started from TFU-nx's tree
(itself from rr6-nx 1.0.1) and keeps TFU's notes in docs/from-tfu/. Each is that port's finding on its own image.

- **The device snapshot.** 2.0.6534's is 12,712 bytes (0x31A8, as N&B's), its last 64-byte chunk 40 bytes; 2.0.11626's
  is 12,888 bytes (0x3258), its last chunk 24 bytes (42 delta read chunks, against Katamari's 39). Port 196's
  `TakeLastChunk` takes 24, 32 or 40: change d3d.h's snapshot size only.
- **The device's private part moves.** From Katamari's offsets, TFU's fields from 0x2F98 sit 8 bytes later (0x2FA0, the
  viewport at 0x3168, the clear colour at 0x3390) and from 0x3504 24 later; Sonic 4's sit 0xB0 past TFU's (the viewport
  at 0x3218, the resolve flags at 0x3434, the clear colour at 0x3440, the depth target at 0x3264). Every hook that
  reads the device by offset (draw.h's field ranges, the Resolve, Clear and Swap hooks, CreateDevice's setup) moves.
- **LTCG.** Sonic 4's Direct3D is linked with link-time code generation: arguments that are always the same are
  dropped. Its CreateDevice and Present take no arguments, draw calls drop constant ones, SetViewport takes floats.
  The first Renut run read garbage. For every hooked function, list the argument registers it reads in the generated
  code and compare them with the hook's signature before the first Renut build.
- **D3DVertexShader_Bind** at 6534 is (pShader, Flags, pDecl, pStreamStrides, pPixelShader): port 173's
  `D3DVertexShader_Bind_flags_hook` takes it (both ignore the fifth argument). Unhooked, TFU's own Bind read a Renut
  declaration record's count (0xF01FFAAE) and walked off the stack.
- **D3D{Vertex,Pixel}Shader_GetFunction** (6534): TFU reads its shaders' microcode back after creating them; the
  originals read a guest layout Renut's records do not have (a memcpy 3.7 GB past .data). Port 196's hook bodies answer
  from the container, kept with `banjo_renut_shader_get_function` on. Sonic 4 does not link them.
- **POSITION6.** 20 of TFU's 1,416 vertex shaders read it (the base's translator has the input).
- **Sonic 4's picture.** It draws 1920x1080 with predicated tiling; `sasrt_renut_depth_restore` drew its title's sky
  white (its reflection pass draws a resolved depth back as colour into EDRAM the main pass's depth shares); off, the
  console's picture.

## XDK 2.0.7978 (banjo-nx, Banjo-Kazooie: Nuts & Bolts' title update 3, its rebase on the base, 2026-11-19; ports 223-234)

Values written into shared files for Katamari's XDK 2.0.5632, which a port at another XDK changes by hand (banjo-nx's list; better beside
the other offsets in d3d.h / d3d_native.h one day):

- `render/draw.cpp` `kDeviceDeclarationOffset` (the device's own declaration, port 213): Katamari 0x2F98, N&B 0x2FA8.
  `render/draw.h`'s `kDeviceOwnDeclarationBegin` / `End` follow it (draw.cpp's static asserts stop the build when they
  disagree), so both are per XDK like d3d.h's offsets. Viva Pinata's XDK 2.0.3529 (vivapinata-nx, 2026-11-24): at
  +0x2E90 (sub_8246B428, `stw r?,11920(r31)`), up to 0x2F78, before the index buffer at 0x2F84; Katamari's 0x2F98 falls
  in that XDK's render targets.
- The device snapshot's end (`d3d::kDeviceSnapshotSize`) and `m_Constants` are per XDK too (port 280, vivapinata-nx):
  Katamari's snapshot ends with the predication word, XDK 2.0.3529's holds RunPredicationState (0x3094) after it, and
  its `m_Constants` is at 0x400, not Katamari's 0x480. Code that compares device snapshots (port 275's tile twins) takes
  both from d3d.h, not from Katamari's numbers.
- `d3d/hooks/device.cpp`, port 169's scaler copy: `device_va + 0x3584`; N&B 0x359C.
- `guest/command_buffers.cpp` `kLastChunkBytes` (port 196's `TakeLastChunk`): 32 at 2.0.5632, 48 at N&B's update.
- d3d.h `kDeviceOffsetViewport` (port 138): Katamari 0x3160, N&B 0x3170.
- Port 87's tiles-once check had vivapinata-nx's 2.0.3529 offsets written in; port 223 reads native::off's.

## XDK 2.0.20209 (ilomilo-nx, 2026-11-24 to 11-25; ports 284-291)

Evidence: ilomilo-nx's `docs/ilomilo-notes.md` ("Renut's Direct3D hooks at ilomilo's addresses", "The device layout"),
D:\ilomilo\ilomilo-nx, with its tools. Bangai-O HD's port (bangaio-nx) is the same XDK build; ilomilo moved the
base's hooks by the route N&B -> Bangai-O (bangaio-nx's three-port map) -> ilomilo (its tools/d3d_propagate_ilomilo.py:
functions paired by shape and call sites), then checked every hook's argument registers with tools/arg_regs_compare.py
(port 291).

- **SetGammaRamp and SetPWLGamma take Flags at 2.0.5632 too.** Katamari's own begin `mr r4,r5; li r5,0`: the base binds
  port 173's Flags forms since port 286, so the line above (11164) is not a later-XDK difference.
- **The lock forms** with the unused second argument (D3D_LockResource, D3D_LockSurface, D3D_Lock2DSurface; not
  Lock3DSurface): port 287's `_unused_hook` forms.
- **The device layout.** ilomilo's zones from Katamari's offsets (its tools/device_offset_votes.py: the aligned
  load/store displacements of the 313 paired hooks): 0 below 10944, then +12, the declaration +180, +184 to the clear
  colour, +192 to the ring words, then +216 / +220 / +224 / +232 word by word, +236 from the command buffer, +240 after
  the gamma ramp, +244 / +248 at the display mode, +1928 / +1936 at the end. Every value equals bangaio-nx's own
  measurement where both have it. Moved fields, Katamari -> ilomilo: the device's own declaration
  (`kDeviceDeclarationOffset`) 0x2F98 -> 0x3050, the viewport 0x3160 -> 0x3218, the depth target 0x31AC -> 0x3264, the
  resolve flags 0x337C -> 0x3434, the clear colour 0x3380 -> 0x3440, ClearZ 0x3390 -> 0x3450, the export count 0x34A8
  -> 0x3590 (Sonic 4's 2.0.11626 values in the section above are the same). The delta capture reads 40 chunks
  (`kDeltaPrivateUnreadEnd` 11776 -> 11904); draw.h's FieldOfOffset bounds 0x2DF8 / 0x2EF8 / 0x317C -> 0x2EAC / 0x2FB0
  / 0x3234.
- **Tiling.** BeginTiling stores the count at +12924, the rects at +12928, Flags at +13364 and the extents at +13356 /
  +13360 (Katamari's fields plus 184); BeginTiling and EndTiling read Katamari's registers exactly. ilomilo draws its
  720 lines with 4x MSAA in predicated tiles of 1280x256: before the hooks were placed only the top 256 lines showed.
- **Memexport.** 2.0.20209 has N&B's two GPU workaround switches (d3d_native_state.cpp `kSkipExportNanVertex` /
  `kSkipExport512Pixels`, 0 at Katamari's XDK): find them by their readers.
- **GetCurrentThreadId** and **XMemFree** are found by shape (`lwz r11,256(r13); lwz r3,332(r11); blr`; XMemFree's
  unique shape against Bangai-O's).
- After placing hooks, `tools/hook_bypass_check.py` (port 291) must say 0 bypassed: `direct_calls.py --check` does not
  see a direct call of a hooked function (ilomilo's first tiled frame drew only its first tile that way).
- `tools/hook_collisions.py` lists the base's hooks whose Katamari address happens to be a function start of the new
  game: each silently replaces an unrelated function until moved or unhooked.

## XDK 2.0.20871 (asura-nx, Asura's Wrath, LTCG; 2026-11-26, ports 292-311)

Evidence: asura-nx's d3d.h, d3d_native.h, d3d_native_state.cpp, hooks/*.cpp, draw.cpp and queued.cpp at 8c329d36
(github.com/BanhMi93P/asura-nx). Every address is Asura's.

- **LTCG wrappers.** Direct3D_CreateDevice takes only pPresentParameters (r3) and stores the device in a global;
  DrawIndexedVertices (device, type, start index, index count) has no BaseVertexIndex; DrawTessellatedVertices takes 4
  arguments; Resolve is (device, flags, rect, dest texture, dest point, dest slice): level, clear colour, Z, stencil
  and parameters dropped; Begin/EndExport are (device, resource); ClearF is (device, flags, rect, colour, Z in f1) with
  no stencil; D3DVertexShader_Bind is (shader, declaration, strides, pixel shader). As TFU and Sonic 4 found: list each
  hooked function's argument registers (tools/arg_regs_compare.py, port 291) before the first build.
- **No CreateVertexShader / CreatePixelShader / CreateVertexDeclaration.** Shaders come through XGRegisterVertexShader /
  XGRegisterPixelShader; the game builds its declarations itself and calls XGSetVertexDeclarationHeader(elements,
  declaration), which asura-nx hooks raw to make a Renut record from the elements.
- **Other forms.** D3D_UnlockResource (resource, base address, mip address); D3DDevice_CreateTexture 8 arguments (type
  last); D3D_SetSurfaceHeader 8 arguments with the EDRAM bytes and Hi-Z tiles written out; D3D_FindTextureSize 14
  arguments; D3D_AlignTextureDimensions 7; D3DQuery_GetData hooked raw.
- **Device layout.** The allocation is 0x6080 (as rehatched-nx's 9407). The vertex declaration 0x2FD0, index buffer
  0x320C, render targets 0x3210, depth 0x3220, stream sources 0x3224, strides 0x3268, textures 0x3278, pixel / vertex
  shader 0x330C / 0x3310, viewport 0x32E0, scissor 0x32FC, predication 0x331C, RunPredicationState 0x3320, tiling
  surfaces 0x3330, tile count / rects 0x3344 / 0x3348, the command buffer 0x3660; the snapshot 0x3320.
  `kDeviceDeclarationOffset` 0x3118; the census and delta private unread bounds 11008 / 12224 (40 read chunks).
- **Memexport's** two workaround switches are guest words at this XDK (Asura's 0x83297914 / 0x83297918).
- asura-nx went back to the rectangle-list shader's run-time corner index (port 164's constant form undone); its reason
  is not given, and the base keeps port 164.

## XDK 2.0.20871 again (S4ep2-XBLA-nx, Sonic 4 Episode II, 2026-11-26, ports 320-328)

Episode II is Asura's XDK (the section above) by S4ep2-XBLA-nx's own tool, not read from the game image. It moved
Episode I's (2.0.11626) hooks and device layout by aligning paired functions (tools/d3d_align.py, d3d_field_votes.py,
port 328) and found the same rule as TFU and Sonic 4: an address one instruction off (its caps table read at 0x820F80E0
instead of 0x821080E0) made every D3DXCreateTexture fail with E_OUTOFMEMORY and the screen sample never-resolved
textures: check each moved address by the code that forms it.
