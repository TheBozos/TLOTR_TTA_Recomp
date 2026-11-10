# Recompilation notes: The Lord of the Rings: The Third Age (NTSC-U, SLUS-21027)

How the port is brought up, every fix and how it was found, and what remains open.

## BozoEngine (since 2026-11-09)

The port moved off the PS2Recomp fork onto [BozoEngine](https://github.com/TheBozos/BozoEngine) (submodule
`BozoEngine/`), as DC, TMB and SC3 did: no PS2 hardware model at any level, every library and kernel call answered
natively. Everything below this section, from "The disc" on, is the PS2Recomp era, kept for its findings (addresses,
the game's behaviour); its code is in the history before `8ade037`.

- **Build** (CMakeLists.txt, modelled on DC_Recomp's): `tools/tta_overrides_list.py` lists the addresses `runtime/`
  registers, `bozo-externals` the externals from them and the symbol map, `bozo-recomp` translates SLUS_210.27 into 32
  files (`build/bozo/tta_gen/`, ~40 s), `tta-headless` (runtime/tta_main.cpp) runs it on the free host and prints
  what is not native yet (unknown device accesses by site, library calls by name, the end reason).
- **Symbol map.** The ELF is stripped. `bozo_symbolmap.py` against Dark Cloud's and Twisted Metal: Black's
  executables names 314 functions: the kernel calls, libc/libm, much of libgraph and libsif. Their SDKs (2000-2001)
  are older than TTA's (2004), so libcdvd, libmc, the fileio client and libpad2 did not match: those were read from
  the code into `recomp/tta_symbols_reviewed.txt`, one line of evidence each. `tools/tta_rpc_names.py` pairs every
  sceSifCallRpc caller with (bound server id, function number) in TTA and the references: that alone named libmc's 12
  clients (server 0x80000400) and fileio's 6 (0x80000001). `scripts/symbols.sh <reference ELFs>` rebuilds the map.
- **Boot, step by step** (each was a stop of the headless runner): syscall 116 at 0x32d25c (SetSyscall from the SDK's
  kernel patch: `InitSys` 0x32d268 named, the engine answers it); syscall 120 (`sceSifSetDChain` inside
  `sceSifInitCmd`: `sceSifInitRpc` 0x32a9c0 named); the CD init's bind loop on 0x80000592 (`sceCdInit` 0x27dd08, then
  `sceCdMmode` 0x27dff0, S-command 0x22 with 2 = DVD); syscall 107 in the IOP reset (`sceSifResetIop` 0x32cd50,
  `sceSifRebootIop` 0x32cef8, `sceSifSyncIop` 0x32cea8, `sceSifLoadModule` 0x32cd08); EA's sound handshake (below);
  `sceGsResetPath` 0x275968 (VIF1/GIF reset registers) and `sceGsSyncPath` 0x275a08; libpad2's bind loop on DBCMAN
  0x80001300 (below). Frames 0 and 1 then end through the native sceGsSyncV.
- **EA's sound driver** (runtime/tta_audio.cpp, tools/tta_bozo_audio.py). AUDIOPF.IRX and LIBSD.IRX are relocated to
  0x4b400 and 0x45a00 and translated by `tools/native_irx` (422 + 63 functions); two AUDIOPF functions are only reached
  through pointers (0x4b400, libsd's transfer callback; 0x4c69c, a thread body) and are given as entries. The game
  (0x226bd0) and the driver (0x5926c) talk only through SIF commands and software registers: each sends set-sreg
  (0x80000001) with the address of its buffers (registers 0x1e and 0x1f) and polls its own until the other has
  written it; the driver also sends user commands to the handlers the game registers (0, 1, 2 at 0x226b60/80/a0, in
  libsif's table at 0x41b018+0x14). The port overrides the game's `sceSifSendCmd`/`isceSifSendCmd` (set-sreg into the
  IOP side's registers, user commands to the driver) and runs the driver's commands as the game's handlers on
  libsif's packet buffer (0x41af40), one at a time. AUDIOPF's start polls a libsd transfer's completion flag in a
  call-free loop and clears its buffers byte by byte: BozoEngine#80 adds the IOP software registers, sifcmd 4-7,
  cdvdman 4/10/13/75, `useReadyAudioCompletionsAfterImports()` and `setRootCallBudget()` (TTA: 50,000,000).
- **Pads: libpad2** over libdbc (DBCMAN 0x80001300, DS2U_D). EA's pad manager (init 0x2154b8, end 0x2156d0, per frame
  0x2153e8/0x214f10 after a 10-frame warm-up) calls scePad2Init/CreateSocket (ports 0 and 1)/GetState/
  GetButtonProfile/Read/GetButtonInfo and libvib's sceVibGetProfile/SetActParam. BozoEngine#81 answers them natively
  on libpad's controllers: an 18-byte report (libpad's DualShock 2 report without its header, pressures in the
  controller's order L1 R1 L2 R2), a 4-byte profile (the game compares it every frame: a fifth byte would drop the
  pad), state 1 with a controller. Note: the 0x27ea40 cluster (IPU DMA registers) is libmpeg/libipu, not the pads.
- **VU0 microprograms** (tools/tta_vu0_gen.py): the 23 VCALLMS entries of the two microcode images in the ELF (file
  offsets 0x249a84 and 0x239d94) translated to C++ at build time, one function per entry, registered with
  `Registry::vu0Program`. 0x21b348 (it cleared VU1 data memory) keeps only its seven words of the game's own.
- **Movies: libmpeg on the SDK's model** (BozoEngine#82, `MpegOptions{referenceLayout = false, paced = false,
  sdkNodata = true}`): TTA feeds raw MPEG-2 video (the .PS2 files: 0x1000-byte "MPG2" blocks) through
  `sceMpegAddBs` from its Nodata callback 0x1be698 and ends the input with a 0 there; it paces itself.
- **Rendering** (runtime/tta_render.cpp, from a map of the game's renderer kept locally in build/bozo/render_spec.md).
  Every draw goes through one VIF1 DMA chain per frame (LLList, 0x3b5be8), built as plain memory: natively it is
  built and never sent, and the draws are taken at the game functions that append them.
  - 0x212478, the frame swap (frame counter, sceGsSyncV, sceGsSwapDBuffDc, the display registers): publishes the
    frame to BozoEngine's renderer and is the frame boundary. `port::frameEnd` defers a stop, so it is the last thing
    the override does.
  - 0x216470, the chain kick: the END tag and the game's {Count, done} bookkeeping, nothing sent.
  - 0x216530, the game's quadword memcpy (native): its one source outside RAM, the frame end's copy of the VU1
    programs' statistics from VU1 data memory (0x1c3718), reads zeros. That frame end then clears those four VU1
    quadwords with `sq` (0x1c3740): called from there, the copy returns to the game's own clear of its seven words
    (label 0x1c3770, v0/a0 set as the loop wants), past the VU1 stores. (A native frame end calling the game's
    functions was tried first: a callee that blocks, here 0x26c290 at frame 365, leaves an override no way to go on.)
  - 0x22af40, the movie picture's upload (one GS image transfer per 16x16 macroblock): nothing uploaded; the picture
    libmpeg decoded (`moviePictureHook`) is the frame's backdrop when the game drew its movie that frame.
  - The renderer has to be switched on by the port (`setEnabled(true)`, `setPresentsAlone(true)`), as TMB does: off,
    the window presents nothing.
  Result (2026-11-10): 600 headless frames with no unknown register access; in the window the EA GAMES and THX movies
  play at 60 frames/s. Next: the 2D through LLPrims 0x218910 (legal screen, loading screen, title, menus), text, then
  the 3D shaders with meshes and textures converted offline from the player's disc.
- **2D** (runtime/tta_2d.cpp, 2026-11-10). LLPrims 0x218910(prim, xyz, rgba, st, n) is the game's immediate
  primitive (loading screen, title, menus, effects). Nothing is read from the packets the game builds (room rule:
  no GIF/VIF scanning, no GS register shadow):
  - vertices: the call's own floats, through the matrix the game picked (P = *(0x36f870), P+0x90: 0 P+0x40, 1 RC+0x60,
    2 RC+0x2e0, RC = **(0x36f350)), divided by w when w != 1, colours x 255 truncated, as its VU0 code does;
  - material: the game's draw state LLDisSt DS = *(0x36f850), read only. Context 1 block: ALPHA +0x100, TEST +0x140,
    FRAME +0x150, CLAMP +0x170, TEX0 +0x1a0, TEX1 +0x1b0 (context 2 at +0xe0 more); PRMODE +0x6b0 (TME 0x10, ABE 0x40);
  - target: only the displayed frame, double-buffered at FBP 0 and 0x70. The menus draw into an off-screen 256-wide
    target at FBP 0x1c0 (TBP 0x3800 when read back: blurred background), not drawn yet. The full-screen opaque sprite
    0x211de4 draws at each frame start is the frame's clear (Renderer::setClearColour), not a quad over the movie.
  - textures: tools/tta_textures.py converts every texture group on the disc once (build/bozo/textures, ~40 s, 3,690
    files): the SCP chunk streams (SHOC/SHDR, zlib `Ldat`), every "TXF " v0x212 container in any resource payload
    (txfe/txfs/txs3/txs4/txfx, and the UI's apxf: 5,366 groups) plus the loading screen's group in the ELF (0x260880);
    the pixel block is written as PSMCT32 into a GS-memory model and read back per PSM (T4, T8, CT32; CLUTs CT32
    CSM1), offline only. At runtime 0x1babe8 (the resource manager's load: object +0 payload, +0x1c type, +0x20 index,
    +0x24 size) tracks where each resource lies, and 0x219ca8 (a group's upload) maps each record's relocated TBP0
    and PSM to its converted file: by (type, index, offset in the payload, record) first (the UI reuses small ids:
    0x1c names 582 images), else by the record's 64-bit id. A draw uses the texture only when its TEX0 window is the
    record's size: the UI renderer 0x305330 reads a group's memory as one 512 x 512 8-bit image (TBW 8 over 32 x 32
    records, CLUT at the group's base), which no record describes; open.
  Result: the loading screen (the One Ring and its Elvish line) drawn natively.
- **The UI: EA Apt** (2026-11-10; map in build/bozo/ui_spec.md, local). TTA's front end and HUD are EA Apt movies
  ('apxf' resources: titl, apti, cons, txf_ ...); 'fuse' resources are PFR fonts (Frutiger, Neue Hammer Unziale),
  'subT' the localised strings. Shapes and bitmaps go through 0x2c9a08 to the strips 0x218fb8(prim, xyz, adc mask,
  rgba, st, n), drawn natively like LLPrims (a2 is a "closes no triangle" bit mask, a3 the colours: 0 = P+0xc0). Text:
  each field's string is rasterised by the game's own PFR code (0x2fb040) into a 512 x 512 8-bit canvas in EE memory
  (TM = *(0x37c490), canvas TM+0x18: +0 w, +4 h, +8 TBP, +0x20 pitch, +0x2c pixels; palette (i << 24) | 0xbfbfbf, the
  coverage over grey): plain linear pixels, made a texture each time the game sends the canvas (0x2c80e8). Apt's
  masks write alpha only (FRAME FBMSK 0xffffff): skipped. The title screen is complete.
- **libmc: sceMcSync** (0x27d058) was missing from the symbol map (TTA's SDK is newer than the references'): the
  game's own copy waited on a semaphore the native libmc never signals, and after New Game the save manager polled
  sceMcGetInfo forever. Named, a pad script (CROSS at 2600, 3020, 3380, START at 4100) takes a headless run to the
  first battle (Berethor against the Black Riders: the battle HUD at ~6000 frames). TTA_RES_TRACE=1 lists resource
  loads (the scene ~2500, the battle's bMAN/cbin/gmv ~5500), TTA_3D_TRACE=1 the 3D shader renders by node key.
- **3D** (runtime/tta_3d.cpp; maps in build/bozo/render_spec.md and skin_spec.md, local). tools/tta_meshes.py converts
  every LLObj resource once (build/bozo/meshes, mesh format 2): the 'OBF ' payload's 0x100-byte node records, each
  node's VIF packet as vertex batches (types 0-5: float positions, s/t, RGBA, ADC = R & 1; type 21: an instance
  matrix, a 256-colour table and, for instance 1 of a group, the shared packet: (x y z)(s t index|ADC)), and the
  skinned 'scbm' (type 17: bind-pose positions, normals, s/t, four skeleton bones + weights from each chunk's palette
  stream, texture ids from material names through the game's name hash 0x26ce88; 7% of batches use a second layout,
  skipped). Runtime key: the node's record (node+0) in its resource: (type, index, payload size, record offset).
  Draws: the type-21 render 0x221178 (instanced terrain, trees), the type-0 render 0x2207a8 (ground, objects), and
  LLObj_Render 0x1bdbe8; matrix RC+0x2e0 (object -> GS screen, RC = **(0x36f350)) x the instance matrix, negated (the
  game's w is negative in front of the camera; a PC GPU clips w < 0). Characters at 0x225fb0 from G = *(0x403a60):
  bones *(G+0x290), W *(G+0x294), sections G+0x280, fade G+0x2bc, lighting G+0x00..0x9f, skinned on the CPU, screen =
  viewport (RC+0x440/0x450) x RC+0x260 x W. Open: type 5 (0x221630, another argument), water (19), effects, the
  render targets (blur, glows); the battle's 3D is not yet checked against the console (the camera swings during the
  Black Riders' attacks; characters come out dark). TTA_DETERMINISTIC=1 runs the window on the game's clock.
- **3D, 2026-11-10 continued.** The projection is the game's: w > 0 in front (an earlier "negate the matrix" came from a
  probe vertex behind the camera and drew the scene behind the camera through the centre). The intro cinematic's forest
  now looks as on the console. Textured batches keep a cut-out alpha test. Characters (behind TTA_SKIN=1 for now):
  the skinned vertices carry the skin program's /1024 (unit-height models; scale E+0x474 in bone 0); the pose builder
  0x133298/0x133148 is native (runtime/tta_pose.cpp: quaternion rows as VU0 0xc28, B[i] = Local . B[parent], bone
  controllers E+0x164, B[i] = InvBind[i] . B[i] read from main RAM; skin_spec.md section 8). Open: (1) the sampled pose's
  quaternions have a wrong w (|q|^2 up to 60, x/y/z plausible): the blends' VU0 program 0xdb0 as translated by
  tools/tta_vu0_gen.py is the suspect; w is rebuilt from xyz meanwhile (TTA_POSE_KEEP_W=1 keeps the game's);
  (2) some drawn characters are never posed by this path (their E+0xe8 stamp lags the frame id): another pose path,
  or G pointing at another object (G = E + 0x1d0 from 0x1b1b18 and 0x11ab40/0x2251b0). Probes: TTA_SKIN_PROBE[2-5],
  TTA_ANIM_PROBE, TTA_POSE_TRACE.
- **Characters fixed** (2026-11-10): tools/tta_vu0_gen.py wrote an LOI's I value before the same pair's upper
  instruction ran; on VU0 the new I is seen from the next pair. TTA's sine series (VU0 programs 0xd38, 0xdb0) load each
  coefficient one pair ahead, so every sine was wrong: the animation's quaternions came out with |q|^2 up to 60 and
  0xdb0 never slerped. tools/tta_vu0_check.py (an independent VU0 micro-mode interpreter) replays recorded calls
  (TTA_VU0_DUMP=<entry> writes them) and matched the old translation bit for bit with the old I timing, so that was the
  only difference. With the fix, every pose is unit-length, and the characters (behind no switch now; TTA_NO_SKIN=1
  hides them) ride, walk and fight as in the game. TTA's world has y pointing down.
- **The first battle, native** (2026-11-10): the GS readback 0x228118 (an 8 x 5 StoreImage through the GIF FIFO from
  the frame end's 0x1d3e58) is a no-op: there is no GS memory. The intro cinematic and the first battle play at 60
  frames/s with everything drawn natively (the diagnostic switches are read once: tta::env).
- **Faces** (2026-11-10): the skin influence records store each weight rounded to x/127 (a 50/50 vertex sums to
  128/127); blended as stored, a face ~1.8 units above the model origin moves ~6% of its width: torn lips, eyelids
  over the eyes. tools/tta_meshes.py normalises the weights; the faces are clean (offline check with the runtime
  bones: TTA_BONE_DUMP=<frame>). Also: the skin VU stores a blended matrix in its cache slot only when position.w lacks
  bit 15. The second skinned layout (150 parts) is shadow/silhouette meshes for the set-5 VU1 program, not drawn yet.
- **Apt's masks** (2026-11-10): Apt clips with the depth buffer, not alpha. Mask shapes are drawn with every colour
  channel masked (FRAME FBMSK 0xffffffff: depth writes only; a full-screen one resets), then the masked content with
  TEST ZTST GEQUAL/GREATER. Natively the mask's triangles are kept and the content is cut to their union (convex
  clipping, runtime/tta_2d.cpp): the round minimap. Apt strips also carry a per-vertex ADC mask (0x218fb8's a2), and
  SCISSOR_1 (DS+0x110) clips the 2D on the CPU. TTA_2D_TRACE=8 prints every 2D draw with FBMSK/TEST/ALPHA/PRMODE.
- **The battle's ground** (2026-11-10): the Black Riders stood over the sky. Two causes. (1) Every terrain and static
  batch (sky dome included) is drawn with PRIM 0x7c, ABE on and opaque texels (alpha 0x80); Z writes were tied to ABE,
  so nothing wrote depth and the sky dome, drawn after the bridge, painted over it. Z writes now follow ZBUF_1's ZMSK
  (DS+0x120 bit 32; the scene set-up 0x1a8600 turns them on). (2) The renderer divides z by w per vertex and wraps GS Z
  past its range like the GS, so a triangle reaching behind the camera or past the near Z gets a garbage depth: the
  VU programs never send those (ADC / the clipping programs). runtime/tta_3d.cpp cuts the triangles in clip space to
  w > 0 and 0 <= z/w <= 2^24-1 first (TTA_NO_NEAR_CLIP=1 turns it off). The batch key's TEX0 is the game's texture
  key, not the GS's: its TCC/TFX bits are not the draw's. Fog is per vertex (F = min(K/w, 255), K = DS+0x54c, FOGCOL
  DS+0x720); the first battle's scene has fog off (K = 2550000). Water (type 19, 0x2221b8) is drawn at rest. Debug:
  TTA_3D_TRACE=2 (fog, water, nodes not converted), TTA_3D_DUMP_FRAME=<game frame> (every node drawn with its screen
  bounds), TTA_3D_ONLY=<record offsets> (draw only those).
- **Characters' alpha and layers** (2026-11-10): the skin's default material template 0x377aa0 sets ALPHA_1 0x44
  ((Cs - Cd) As + Cd) and TEST_1 0x5180b: alpha >= 0x80, a failing texel still written in colour but not in Z (AFAIL
  FB_ONLY). Drawn as two passes (opaque texels with Z, the rest blended without): hair and cloak edges blend instead
  of showing as flat strips (TTA_SKIN_NO_AFAIL=1 for the old look). 0x1b1b18 draws every character four times a frame,
  one per layer ("01".."04" through 0x1bf6b8) with G+0x2b0 = 0, 0x20000, 0x200, 0x1000; layers 02-04 are empty for
  the characters seen so far. 0x200 / 0x1000 now blend as their templates say (Cs FIX + Cd / Cd - Cs FIX, FIX = the
  fade, no Z writes). Open: 0x80000 (a coincident copy of the screaming Nazgul, drawn untextured in FOGCOL on the PS2
  per the template) is still drawn textured; 0x20000's render-target switches. TTA_SKIN_LAYERS=<mask> (1, 2, 4, 8,
  16 for 0x80000) draws only some layers; TTA_3D_TRACE=2 lists the modes by model.
- **Character shadows are shadow volumes** (2026-11-10, mapped, not drawn yet). The type-18 render 0x2255d0 runs on a
  skinned node (only Berethor's scbm 70063 so far, ~1-2 a frame) with G+0x280 = only the model's shadow section (the
  second part layout the converter skips: vertices, then edge {v0, face, face, v1} and face lists). It sets PRMODE
  0x40 (flat, untextured, blended), the template 0x377a10 (ALPHA 0x44, TEST 0x50000: no alpha test, Z GEQUAL),
  clears FBA, and uploads 14 qwords to VU 0 through 0x21d438 (the matrices from 0x225218: clip, W, the light vector
  from G+0x298, default 0x377990, at row 8; viewport rows 9-11). Set 5's program 0x178 (overlay 0x355780) transforms
  the mesh, finds the silhouette edges from the faces' facing to the light, extrudes them and draws the caps and
  sides through 0xce8 with +/- signs (front / back faces): a counting pass. The darkening is then the set-5
  full-screen passes (layer hooks 0x21a778 / 0x21aa88, MSCAL 0x1ea8). A native port needs the shadow parts
  converted, the volume built on the CPU, a counting target (BozoEngine has no stencil) and the darkening pass.
  Probes: TTA_SHADOW_PROBE=<game frame> (the VU block), TTA_TYPE18_DEBUG=1 (the pass's nodes and GS target).
- **Shadow volumes, native (opt-in, TTA_SHADOWS=1)**. tools/tta_meshes.py converts the shadow parts (raw q0.x bit 15;
  every 16-bit field ^ 0x8000 as the VU sees it): q0 = (vertices, strip length, faces, edge entries), q1 =
  (influence records, strip start, faces start, edge start); vertices 2 qwords from q2 {x y z /1024 x scale, w: < 0
  blend the next record and cache it at VU 158 - w, > 0 the matrix cached at VU 159 + w, 0 keep}; strip entries' .w
  = a vertex (bit 15: no triangle ends there); faces (.x .y .z). Output: skinned triangle lists, prim 0x8003 (never
  drawn as a surface). Berethor's hull: 415 triangles. At the type-18 pass the runtime skins them, sweeps each
  triangle along the light (G+0x298, default (2, -2, 0)) into a closed prism, counts the faces into the renderer's
  stencil by screen winding (z-pass) and blends the frame toward black where the count is not 0 (BozoEngine's
  Material stencilOp / stencilNotZero / stencilClear, as SC3 does). Opt-in because PCSX2 shows no character shadow in
  the first battle: the light, reach and strength are not checked against the console. TTA_SHADOW_SHOW=1 draws the
  volumes in colour, TTA_SHADOW_UP_ONLY=1 / TTA_SHADOW_NO_STENCIL=1 test the stencil, TTA_SHADOW_REACH /
  TTA_SHADOW_ALPHA tune it, TTA_SHADOW_ORDER=1 prints the pass order, light and bounds.
- **Shadows checked in a PCSX2 GS dump** (Shift+F8 in PCSX2: one frame's GS packets, .gs.zst; parsed offline by a
  throw-away script): layer 4 is the shadow pipeline. 0x21a778 inverts Z in place (FBP 0x100 = the Z buffer, white -
  Cd, FBMSK alpha), 0x21aa88 clears the count buffer 0x180 (colour and Z share it), then per character the volume's
  strips go into 0x180 with Z GREATER against the inverted Z and additive wrapping colour (B +1 / 0xff), and
  0x21ae10 darkens the frame where the count is not 0: Cd x (1 - TA0/128), TA0 = *(0x3779a0) (G+0x2b4 when G+0x298
  is set): 0x13-0x20, i.e. 15-25% darker. 0x21b0f8 restores Z. The native shadows now use that strength and are on
  by default (TTA_NO_SHADOWS=1 off). The console's haze is not a post pass: it is scene geometry (large mipmapped
  textured fans, huge additive strips, a faint veil), the Glows objects (types 9/20) not drawn natively yet.
- **The mist: the Glows objects** (mapping in progress). Shader types 9 and 20 (ShaderObject_Glows_Ps2.c) have no
  render at +0x2c; their table entries 13-16 are 0x21f010, 0x21f088, 0x21f980 (render, many a frame in the forest
  and the battle) and 0x21fce8; 0x21ea18 / 0x21ea20 run around them, and 0x21c420 (with 0x21c098) generates their VU
  microcode each frame into BSS (0x3f6f00 / 0x3faf00). A glow: S = *(instance + 4); +0xa0 flags (0x200, 0x2000:
  0x21ef58 instead), +0xa4 intensity times a near/far ramp (+0x134's +0xe0 / +0xe4 against +0x8c's +0xa8), skipped
  below 0.005; +0x88 a size given to 0x21f090, which sets the draw state, uploads matrices (0x210d10, 0x213140,
  0x213068) and kicks the mesh at +0x134 (+0x46 count) through the packet at +0x138 (12 qwords). The level files hold
  no type-9/20 nodes: the glows come from the effects (SFX/SFXGlow.c). On the console they are the forest's haze:
  large mipmapped T4/T8 fans across the screen, huge additive strips (light shafts) and a faint grey-blue veil.
  Types 1-4 never render in the first level; type 5's render is 0x221760 (0x221630 is its per-frame set-up).
- **The forest haze is the type-19 "water" module (mist banks)**, found in a PCSX2 GS dump of the Black Riders scene:
  11 huge translucent ribbons (6-vertex strips = 3 rows per edge, vertex alpha 0x1a / 0x40 / 0x66, a 256x256 T4
  mist texture, (Cs - Cd) As + Cd), each drawn again with alpha 0 (a no-op). Natively the at-rest conversion (one quad
  per edge) draws one small dark sheet. The geometry is built each frame on the CPU by the animator 0x2236a0 (~1000
  instructions: rows, the alpha gradient, waves from the sine table 0x403788) and kicked by the driver 0x2246e8;
  a native port of that animator is the way to the haze. The Glows (types 9/20) are particle billboards (blocks of
  16 particles at S+0x230, 0x300 bytes each, formatted by 0x21ea28: V4-8 range, 3 x 16 scalars, 16 x {position,
  (.., angle)}, 16 RGBA5551 colours; texture id = *(S+0xc8); world or S+0x94 object matrix when S+0x84 & 4; drawn
  by microcode 0x620 of the 422-qword image at 0x350690: random jitter seeded from the position, rotation, alpha
  from a base plus a per-particle term). Probes: TTA_GLOW_DUMP=<game frame> (each glow's DMA blocks and records),
  TTA_POST_DUMP.
- **The console look: the screen filters, native** (2026-11-10). The PS2 frame was twice as bright as ours (mean 61
  vs 30 in the intro close-up) and green-hazed in the forest: not the display (PMODE only averages the two read
  circuits one line apart) but the screen filters 0x1c1d34 .. 0x1d235c, 2D sprites drawn with the frame as a texture:
  the frame (TBP0 0 / 0xe00 = FBP 0 / 0x70, PSM CT24) into the 256 x 256 target FBP 0x1c0 (FBW 4), blurred onto
  itself, then composited back over the frame (0x1d235c: 30 draws, alpha ramping) with the grade sprites (an
  additive 0x7f x As, a tint at alpha ~0x3a). runtime/tta_2d.cpp now gives each off-screen FBP one of the renderer's
  auxiliary targets (FBW x 64 by the scissor's height; the game's window is centred on 2048), samples TBP0 = FBP x 32
  from it, the frame being built (source 0; the 3D draws report its FBP) or the other half of the double buffer
  (kPreviousFrame), and draws these in the 3D frame's list. The intro close-up's mean is now 58 / 59 / 49 against the
  console's 61 / 58 / 50. TTA_NO_TARGETS=1 for the old look. Also native: the type-19 mist banks from the animator
  0x2236a0 (rows along each edge, base / extruded, waves; TTA_NO_MIST, TTA_MIST_SHOW): dark ground mist.
- **PCSX2 as the reference** (2026-11-10): D:/PCSX2 with the ISO; save state slot 1 = the intro's first close-up
  (native frame ~3751). Next to it the native frame is much darker and crisper: the console image is brighter, misty
  (the background washes to grey-blue) and soft. The full-screen glow / mist passes (set 5, layers 4/5/0xc/0x19) are
  the biggest remaining difference in the forest and the first battle.
- **Window build** (`tta`, TTA_WINDOW=ON by default): the same port in BozoEngine's SDL3/OpenGL window with the PC's
  pads (DevicePad) and sound (AudioDevice), held to 60 frames/s (FramePacer); `--capture-dir DIR --capture-frame N`
  saves frames, `--headless` runs it as tta-headless. The VU0 programs file is generated in its own folder: bozo-recomp
  clears `tta_gen/` when it runs.

## The disc

- `SYSTEM.CNF`: `BOOT2 = cdrom0:\SLUS_210.27;1`, `VMODE = NTSC`.
- `SLUS_210.27` (SHA-1 `d88ded370592114169fa18f0ff73b9f610a94bd0`): ELF32 R5900, entry `0x00110008`, **stripped** (no
  symbol table, no `.mdebug`). `.text` 0x00110000-0x00348D18, `.vutext` 0x00348D80 (66 KB of VU microcode), 39
  `.DVP.overlay` sections (VU1 microcode overlays), `.data`, `.rodata`, `.bss` up to ~0x449000.
- `MODULES/`: `IOPRP280.IMG`, `LIBSD`, `SIO2MAN`, `SIO2D`, `DBCMAN`, `DS2U_D`, `MCMAN`, `MCSERV`, `AUDIOPF.IRX` (the
  game's sound driver). `ps2net iop-compat` (PS2_Network_Recomp): every import of these modules is provided by the
  fork's IOP kernel.
- `DATA/GAME` (2.3 GB, per-area folders and `.SCP`/`.SAS` scene packs), `DATA/MOVIES` (1.8 GB).

## Function map

- `ps2_analyzer` (fork 9020eda) recognises 102 SDK functions with runtime handlers and 268 library functions without
  handlers from the embedded signature database.
- Ghidra 12.1.4 headless with the Emotion Engine extension and `ExportPS2Functions.java` produces
  `recomp/ghidra/tta_functions.csv` (`scripts/ghidra.sh`, 81 s of auto-analysis): 9,207 functions and 39,138
  executable labels.
- Every analyzer stub lands on a Ghidra function start except `mceIntrReadFixAlign@0x0027CD30` and
  `mceGetInfoApdx@0x0027D120` (memory card library). Ghidra only has labels there, and neither address is inside
  another function: functions Ghidra missed, not mid-function names.
- No `sceVu0*` HLE stubs: the game's own libvu0 runs (Drakengard found HLE libvu0/libm names landing mid-function).
  libm (`sinf`, `cosf`, `sqrtf`, `__ieee754_*`) is stubbed; check it if math goes wrong.

## Recompile

- `ps2_recomp`: 48,345 entries, 48,235 recompiled, 110 stubs, 0 decode failures, 0 errors, 4,892 warnings (almost
  all unresolved `JR`/`JALR` promoted to fallback entries: switch tables and function pointers, which dispatch through
  the function table). 381 MB of C++.
- `ps2_vu_aot`: 38 microcode fragments, 7,921 instruction pairs -> 41 files in `generated/vu1/`.

## To check (from the other ports, TheBozos/PS2Recomp unless noted)

- `ps2xIOP/src/modules/dbcman.cpp` only answers the version-check RPC (0x80001363); the game reads the pad through
  libdbc + `DBCMAN.IRX`/`DS2U_D.IRX`, so pad input likely needs the rest of DBCMAN's RPCs.
- Already on main, nothing to do but watch for the symptoms: SQRT.S reads ft (58b5c8b), VU0 macro FMAC flags (9fa7e4e),
  masked DMAC completions stay pending until CIM is re-enabled (02398e0), no thread switch inside interrupt handlers or
  DI sections (b1870f5), MPEG HLE demux lead per game (bea000e, `ps2_stubs::setMpegMaxPicturesAhead`).
- VU1 microcode lives in 2 KB `.DVP.overlay` chunks: VP2S merges chained MPG uploads into one `ps2_vu_aot` fragment
  (not on main yet). Never build the fragments with MSVC `/fp:fast` (-0 becomes +0).
- C++ vtables built at run time show up as missing call targets; VP2S has a fix for `jr ra` empty methods (not on main).

## Runner build

- With full debug info the runner was ~3 GB (2.8 GB of DWARF for the generated code) and Windows refuses to load it
  ("Exec format error"). `CMakeLists.txt` moves the DWARF to `ps2EntryRunner.debug` after each link (136 MB exe left).
- Build time (16 threads, 34 GB): a full rebuild went from 55+ min to ~12 min, an edit to `runtime/*.cpp` takes ~80 s.
  - `register_functions.cpp` is one constructor of ~214,000 stores: 35 min at `-O2 -g`, alone at the end of every full
    build. It is built outside the unity batches at `-O0 -g0`.
  - The generated code gets line tables only (`-g1`, the runtime library keeps `-g`): 507 MB of DWARF instead of
    2.8 GB, faster batches, link and split.
  - `runtime/*.cpp` is outside the unity batches. Source properties need `TARGET_DIRECTORY ps2EntryRunner`: the target
    is created in the submodule, and properties set from this directory alone are silently ignored.
  - `rsync -rc` of the 48,800 generated files took 5-6 min on MSYS2; `tools/sync_tree.py` (content hashes in
    `build/generated.manifest`) does it in seconds.
  - Fork 281f7d9: generated sources declare only the functions they call by name (84 files of 48,810) instead of
    including the 4.2 MB `ps2_recompiled_functions.h`, so new entry points or stubs rebuild only the changed files.
    An edit to a runtime header still rebuilds all the generated code.
  - Fork 373bc18 (VP2S, on main as 3c04393): generated code includes only `ps2_runtime_macros.h` and
    `ps2_recomp_calls.h` (over `ps2_recomp_abi.h`), not `ps2_runtime.h`. The fork's `ps2EntryRunner` PCH still listed
    `ps2_runtime.h`, `ps2_syscalls.h` and `ps2_stubs.h`, so on TTA (and JC2) a runtime header edit still rebuilt every
    generated file; fork branch `tlotr/runner-pch` makes the runner PCH those two light headers.
- What VP2S does for its build (VP2S_Recomp `port/CMakeLists.txt`, 2026-10-28, a runtime header edit ~20 s instead of
  13-15 min), for TTA to take up:
  - PCH = the two light headers above.
  - `UNITY_BUILD_MODE GROUP`, groups by guest address (`VP2S_UNITY_RANGE_SHIFT` 13: 8 KiB of guest code per group,
    ~470 groups): one changed function rebuilds one group, a new file shifts no other batch. TTA still uses the fork's
    BATCH mode (32 files in name order).
  - Generated code at `/O2 /Ob1` (MSVC); runtime `/GL`, link `/LTCG:INCREMENTAL /CGTHREADS:16 /OPT:REF /OPT:ICF
    /INCREMENTAL:NO` (relink ~2.5 min to seconds). TTA builds with MinGW GCC, which has no incremental LTO link.
  - VU1 AOT fragments in their own target and job pool, unity off (TTA does this too). No ccache.
  - Pitfalls: never switch branches in the build tree (a checkout rewrites header mtimes: full rebuild; build from a
    worktree and check out commits by hash), a PUBLIC compile definition on `ps2_runtime` forces a full rebuild, and a
    `file(GLOB)` of module directories needs `CONFIGURE_DEPENDS`.

## Boot

1. All 8 IRX load. The EE then spins at 0x00226D60-0x00226D78 (`FUN_00226bd0`, EA's SIF handshake): it sends its
   buffer addresses to the IOP as SIF software registers 0x1F/0x1E (system command SET_SREG, cid 0x80000001),
   `AUDIOPF.IRX` polls `sceSifGetSreg` (sifcmd ordinal 6) and answers with SET_SREG towards the EE, and the EE polls
   its own SREGs through its sceSifGetSreg (0x0032A250). Fork fix: SET_SREG is handled on both sides without a
   registered handler, and sifcmd ordinals 6/7 read and write the IOP SREGs (test in `iop_emulator_tests`). The
   game's own `sceSifGetSreg@0x0032A250` is stubbed to the runtime's.
2. Past the handshake (GS setup, vsync callback, DMA/GIF/VIF traffic), then
   `[guest-branch:missing-target] JALR 0x198690 -> 0x1C45A0`: a C++ virtual method only reachable through the
   pointer at 0x37EDF4. `tools/find_code_pointers.py` scans `.data`/`.rodata`/`.sdata` for words pointing into
   `.text` at addresses the function table does not have, keeping those that open a stack frame or follow a
   `jr ra`: 497 targets, added as `ptr_*` entry points in the TOML.
3. Past the vtable call, then the indirect call at 0x26CBA4 jumps to 0xFFBF00D0 (the encoding of
   `sd ra, 0xd0(sp)`). 0x26CB80 calls `g_allocator->vtable[0x1C]` with `g_allocator` at 0x378888, set once by
   0x26BA50 to `new Allocator` (8 bytes from EA's own allocator 0x1B8538, vtable 0x37ECD0, slot 0x1C = 0x26B978).
   The first call goes to 0x26B978; a later one finds the object overwritten. `TTA_TRACE_ALLOC=1` logs the object,
   its vtable and the slot whenever they change.
4. The allocator global is 0: `operator new` (0x1B83A8 -> EA's 0x1B8538 -> 0x1BA838 -> 0x2500F8) returned NULL, the
   constructor wrote the vtable to guest address 0, the first call went through `*(0)`, then address 0 was reused.
5. EA's heap (0x1B8478, run by state 1 of the main state machine 0x1C29C8 before state 3 creates the allocator):
   `0x1BD698` gives `0x2000000 - end of BSS`, the heap is one newlib `malloc` of that minus 16 KB, retried 16 KB
   smaller until it fits. The game's `sbrk` (0x328C30) stops at the runtime's `EndOfHeap` (0x1F00000, the runtime
   keeps the top 1 MB): the heap gets 0x1AB31F0 at 0x448E20, and every allocation from it fails, 24 bytes included.
   The allocator is dlmalloc 2.6 (arena 0x3A9790: create 0x24E788, add region 0x250978, memalign 0x2500F8, malloc
   0x24F780; no MORECORE, so requests come from the top chunk, `top->size & ~5` against the request + 16).
6. `TTA_MEMTOP=0x1F00000` (fits EA's first malloc under `EndOfHeap`) gives the same heap size and the same failures:
   not the size. The arena dump looks sane (bins self-linked, top = region base + 0x20, region node linked).

7. Boot 9 (a copy of the generated 0x24F780 with prints, `runtime/tta_probe_dlmalloc.cpp`): at the first malloc the top
   chunk's size is 0, though it was 0x1AB2FD1 right after setup. The runtime's own guest heap (`guestMalloc`, for HLE
   stubs) starts at the SetupHeap base 0x448E10, inside the span TTA's sbrk hands to EA: the HLE `sceGsResetGraph`
   builds its 128-byte GIF packet there and the high word of DISPFB lands on the top chunk's size field. Fork 2d2ae00
   (local): `PS2Runtime::pinGuestHeap`, used by `applyTta` to put the runtime's heap in the 1 MB under the reserved
   area (EndOfHeap then returns 0x1E00000), and SIF command packets from a reserved pool.
8. Boot 11 (pinned heap): EA's heap holds 0x19B31F0 bytes (about 25.7 MB) and its allocations succeed. The boot
   stops at `JALR 0x119DEC -> 0x12B8D0`, a leaf behind alignment nops that the `jr ra` rule missed: the script now
   steps over them (246 more entry points).
9. Boot 12 stops at `JALR 0x1C75CC -> 0x1C62C0`, in code Ghidra never disassembled (317 such gaps, about 39 KB). The
   script now also takes, given `tta_functions.csv`, every pointer target outside all Ghidra functions; the
   recompiler runs such an entry up to the next known function (98 more, 841 in all).
10. Boot 13: first pictures. A ring of particles turns with a garbled line of text (the loading screen), double
   buffered, for the whole run. Once, in a handler on the invocation stack (sp 0x1FFFFF0), `JALR 0x126434 -> 0`:
   0x125FA8 calls a method of the object at 0x3A1728, which 0x1552C0 (state machine, 0x1C2EA8) only sets later.
   The game's VSync handler 0x212638 runs with its hook (0x3F6E54) still 0, so it is not that one.
   `TTA_TRACE_CALLS=1` logs the first calls of the functions around it.
11. Boot 15 (missing-target log with the thread and invocation): `thread=1 invocation=HleCall`. The fork's HLE
   `sceIpuInit` ran guest code at a fixed 0x126428, another game's copy of libipu's static `SetD4_CHCR(1)`; in TTA
   that address is inside 0x125FA8. The helper only writes D4_CHCR = 1 with DMA suspended, so the stub now does that
   (fork ebc558a, with the pinned heap and the log). Boot 16: no missing target left; the loading screen still turns.
   The main state machine (0x1C29C8, byte state at 0x3BA700, table at 0x35FC98 of {enter, update, exit, next})
   sits in state 3 (enter 0x1C3DD8, update 0x1C41F0, which runs the per-frame updates of 0x1C4018), and no file
   is read: the game's libcdvd (0x27C808-0x27EA18) is mostly unnamed recompiled code.
12. Boot 17 (fork ebc558a, `PS2X_STATE_DUMP=15 PS2X_CALL_TRACE=1 PS2X_WATCH=0x3BA700`): same picture. The state
   stays 3; the EE thread waits on vsync in the frame swap (0x2124B0); the only SIF RPC is the pad library's
   (sid 0x80001300, call 0x8000131A, every frame); `sceSifSendCmd` runs about 2900 times a second (EA's sound
   commands to AUDIOPF); no libcdvd or `sceOpen` call at all, so the game has not started loading anything.
13. The loading screen is EA's UStream (`SPECIF/UStream.c`, request queue 0x3F2A48, wait loop 0x1D6880 drawing
   frames through its callback). Its requests go to EA's AUDIOPF.IRX by SIF commands (IOP handlers 0x627FC/0x6282C
   wake a worker that answers with SIF command 0 to the EE handler 0x226B60). AUDIOPF builds each disc path with
   sysclib `sprintf("\%s\%s.%s;1", dir, name, ext)`; the IOP's sprintf copied the format literally, so every
   `sceCdSearchFile` failed (256 tries per file, 144 files). Fork ef79b6a formats the varargs (and logs a missed
   search once per name). Boot 25: the files are found and read, the loading screen ends.
14. Boots 26-32: a dark screen follows, the same in every frame, with the CPU and the paraLLEl-GS renderer alike.
   The game is not stuck: the main loop runs every frame (frame limiter 0x1C36C8 spins on the vsync counter
   0x3F65B4), VU1 runs 16k microprograms per dump, the pad works (SIO2 trace: DS2U_D sets the DualShock 2 in
   pressure mode and polls it) and scripted START/CROSS presses change nothing. A GS dump (`PS2X_GS_DUMP_AT`)
   shows full-screen 8x8 sprite passes into the off-screen buffers 256 and 384, and the 3D strips drawn to the
   display buffer have every Y at 0x7FFF and X at the guard band (1535.9/2559.9): their vertices are projected
   off screen. They are built on the EE (0x218FB8 and 0x218910: FPU and VU0 macro code through the scratchpad).
   `PS2X_VU0_CHECK=1` finds no compiled/interpreted VU0 difference.
15. Boots 33-41: the strips came from VU1 (XGKICK of an AOT-compiled microprogram), not from the EE. The EE side
   checks out: the mesh renderer 0x2C9A08 multiplies its matrices with the VU0 microprogram at 0xA80 (probe: the
   product is exact) and 0x218FB8 only draws a full-screen quad. With `PS2X_VU_AOT=0` (VU1 interpreter) the loading
   screen shows the One Ring with its inscription and a readable line of text, then the memory card check box;
   `PS2X_VU1_EXACT_FLAGS=1` with AOT on does the same, so the AOT observer analysis misses one of TTA's MAC/status
   flag readers and skips flags it needs (reported to sc3-port). `applyTta` sets PS2X_VU1_EXACT_FLAGS=1 until then.
   After the memory card check the screen goes black (no movie call, no further disc read).
16. Boots 42-52: after the memory card check the save manager waited forever in `kState_STALL_FOR_UI`: its timer
   (0x1BE428) reads COP0 Count, which the runtime never advanced. Fork branch `tlotr/cop0-count`: Count and the
   performance counters (TTA reads `$25` 432 times) run with the EE cycle clock. Then the legal screen shows, and
   the boot movies start: TTA feeds `sceMpegAddBs` only from its `sceMpegCbNodata` callback, which the HLE never
   called, and real `sceMpegAddBs` sends `((size + 19) >> 4) << 4` bytes (TTA passes 0xFB0 for 0xFC0-byte blocks).
   Fork branch `tlotr/mpeg-nodata` fixes both. The CPU GS rasterizer draws the movie frames as solid magenta;
   paraLLEl-GS shows them (EA GAMES logo, New Line), so `applyTta` requests paraLLEl-GS. After about 30 movie frames
   the game calls `sceMpegInit` (0x27EA40) from the Nodata callback when its packet queue runs dry, to reset the IPU;
   the HLE stub wipes every stream and callback, so the TOML now leaves `sceMpegInit` to the game's code.
17. Boots 53-54 (2026-10-28, fork main with both branches merged, submodule on `tlotr/native`): EA GAMES and New Line
   play to the end, then a black screen. The Nodata callback 0x1BE698 takes the next packet from its queue
   (0x1BE648); when the queue is empty it calls `sceMpegInit` and returns 0, which is how TTA ends a movie. The HLE
   ignored the result and GetPicture waited forever. Fork `tlotr/mpeg-nodata-end`: a callback that fed nothing and
   returned 0 ends the input (`inputFinished`, `streamEnded`), the pictures left are shown and `sceMpegIsEnd` reports
   the end. Boot 54 reaches the title screen (New Game / Load Game / More); the attract movie plays and returns to it.
   Mind the MPEG HLE's logs: GetPicture's lines stop after 32 on purpose, frame 31 is not where decoding stops.

18. Boots 70-78 (2026-10-29): a pad script through New Game (`PS2X_PAD_SCRIPT`, Cross every 6 s from 55 s) reaches
   in-engine 3D (forest, a Black Rider) at ~15-20 game frames/s on the native defaults, then the game dies on the
   next scene load. The old configuration (paraLLEl-GS, interpreted IOP, EE clock) dies the same way, so it predates
   the native work. Seen: dlmalloc's `malloc_consolidate` (0x2506C0) looping on a fastbin cycle, a malloc bin
   loop (0x24F980), and a virtual call at 0x2B61CC (0x2B6180 walks an array of objects at [this+0xC0], count
   [this+0xC4]) through an object whose vtable word points into code: an object freed and reused while still listed.
   Tools in `runtime/tta_overrides.cpp`: `TTA_HEAP_CHECK=1` (a free of a chunk already on a fastbin: EA's dlmalloc
   marks those with size bit 2), `TTA_HEAP_WATCH=<addr>` (every malloc/free of one block, with the callers),
   `TTA_HEAP_WALK=1` (the whole heap walked around every malloc/free: no failure in boot 78, so the corruption is
   not inside the allocator), `TTA_TRACE_MPEG=1` (GetPicture's buffer is 1024 macroblocks, enough for 512 x 448).
   Fork `tlotr/mpeg-nodata-caller`: the Nodata callback runs on GetPicture's thread (it frees game memory; as a
   queued invocation it could cut into another thread's malloc). Right on its own, not the cause here.
   Boots 79-87: runs are not reproducible even with the EE clock, `PS2X_PAD_CLOCK=ee` and `PS2X_GS_SYNC=1`, but the
   crash always comes ~110-120 s in, during the in-engine intro cinematic (letterboxed forest), not on a load. The
   fastbin check caught `free(0x48)`: a child vector at +0xC0 of an object of the 0x2B6178 / 0x2B65A8 family held
   data 0x38 (the object was already destroyed; 0x2BA268 destroys them, 0x2C123C adds children to them later).
   Pressing nothing after the menus changes nothing. A 10 ms IOP CD completion (the HLE completes in 128 cycles)
   moved the crash to a JAL into code with no function: the scan of every JAL in the executable found 10 targets
   Ghidra and the recompiler missed (leaves after a function's padding), now `jal_*` entries in the TOML. Still open.
   Boots 88-92: one run with `PS2X_VU_AOT=0` reached the first battle (Berethor against two Black Riders), but two
   more with the interpreter crashed the same way, and `PS2X_VU0_CHECK=1` showed no data difference between the
   compiled VU0 microcode and the interpreter: the VU AOT is not the cause.
19. Boots 93-101: the cause was the IOP completing TTA's CD reads at once (AUDIOPF's sceCdRead completion callback
   128 IOP cycles after the call). With a 10 ms completion two runs of two got past the intro; fork
   `tlotr/iop-cd-timing` models the drive (`PS2X_IOP_CD_SECTORS_PER_SECOND`, `PS2X_IOP_CD_ACCESS_US`; off by default):
   with 2700 sectors/s and 1 ms access one run in three crashed, with 10 ms access three in three got past it.
   `applyTta` sets 2700 and 10000 us. Boot 101 (defaults only, three Cross presses) reaches the first battle at ~400 s:
   Berethor against two Black Riders, the battle menu and hint text clean. The game logic has a race the instant
   reads exposed; the drive timing hides it the way the console's drive does.
20. Frame rate (`TTA_FPS_LOG=1`, boots 103-113): the intro film plays at its encoded 15 fps; in-engine scenes were
   held at 15 too. TTA's vsync handler (0x212638) stores Count + 16.68 ms (the next vblank's Count) at 0x3F65C8; the
   frame loop (0x1C37B8) reads it through 0x212628 and spins on MFC0 Count until within 0.17 ms of it, else a whole
   period: here a spin iteration can outlast that window and every frame lost a vblank or two. `applyTta` now makes
   0x212628 return the current Count (`TTA_SKIP_VBLANK_SPIN=0` keeps the spin): sceGsSyncV paces the frame, the
   in-engine cinematic runs at 30 and the battle at ~18.5 (was 14.5), which is now the host's speed. Ruled out:
   `PS2X_VU1_EXACT_FLAGS` (no change), the EE clock choice (15 either way).

## Native PC port (2026-10-28 / 29)

The goal, shared with TMB, VP2S and JC2: no PS2 emulation left. Fork `docs/native.md` (on `native/renderer`) has the
layers. What TTA runs by default now (submodule on `tlotr/native`):

**What still counts as emulation (room rule, 2026-10-30: "no more emulation, even if it is a small thing").** Native
means draws built straight from the game's own data, with no GIF packet, no GS state machine and no copy of GS memory.
By that rule several layers below, listed earlier as native, are not:

- **`gsWrites` and the GS front end.** Every draw TTA hands to the GL back end, the C++ ports of 0x460 and 0x268
  included, carries the register writes of its PS2 packet (TEX0 with its CLUT load, CLAMP, TEX1, ALPHA, TEST, ...),
  and `GS::submitNativeBatch` replays them through the front end's state machine; the back end takes textures, CLUTs
  and targets from that GS state.
- **The VU1 sandbox.** `Vu1StreamAdapter` runs every other program (the characters: 8df1e059 0x10 / 0x190 / 0x1F8,
  d43b1a88 0x620, ...) as VU1 microcode on its own VU copy, and `GifToNative` parses the XGKICK packets back.
- **The CPU copy of GS memory.** Every texture upload is swizzled into it pixel by pixel (`GSMem::WriteP8` /
  `WriteCT32`) and mirrored to the GPU's VRAM texture: 6.8 MB a frame in battle (boot 153).
- **VIF1 unpacks, the VU0 microprogram 0xA80 (mesh math), the movies' vblank pacing.**

The order of work: the VU1 programs translated to C++ (SC3's `scripts/vu1_port_gen.py` does it ahead of time,
checked call by call against the sandbox), then draws built from their inputs with materials and textures from the
game's data, no GIF packet and no GS front end; then the GS memory copy goes with the last user of it. The battle
performance work of boots 145-153 (uploads, swizzle) speeds up this emulation layer and is secondary to removing it.

- **GS: GSNativeBackend** (OpenGL, VP2S's `native/gs-backend`), chosen by `applyTta` through
  `PS2Runtime::requestNativeGsRenderer` (fork `tlotr/native-gs-request`; `PS2X_GS_RENDERER` still overrides). No
  paraLLEl-GS. It draws the movies, the title screen and its off-screen passes (a 512 x 448 picture composed from
  targets at FBP 0x1C0 and 0x100). Three fixes were needed: a first draw that is native recorded no GS thread (no GL
  context, null targets), frames narrower than 640 failed the host copy (magenta), both now on `native/gs-backend`;
  and TTA draws its movie pictures with the undefined PSM 0x20, which the front end now reads as PSMCT32 (fork
  `tlotr/gs-unknown-psm`; the CPU renderer showed them magenta).
- **IOP: the eight IRX recompiled to C++** (`scripts/iop_recomp.sh`, the fork's `tools/iop_recomp.py` with pointer
  seeds: `jc2/iop-recomp-scan`, written with JC2): AUDIOPF 394 functions, LIBSD 59, SIO2MAN 49, SIO2D 26, DS2U_D 45,
  MCMAN 199, MCSERV 24, DBCMAN 73, none interpreted. AUDIOPF's SIF handlers (0x627FC, 0x6282C) and worker (0x62858)
  are only passed as pointers, so the tool's entry/export/JAL walk had missed them. `PS2X_IOP_NATIVE` is read per
  module load (fork `tlotr/iop-native-late`) and `applyTta` sets it (=0 for an interpreted A/B).
- **Time: COP0 Count on the host clock** (fork `tlotr/cop0-wallclock`, `PS2Runtime::setCop0WallClock`). TTA does
  not count vblanks: its main loop 0x1C3588 takes the frame step in ms from MFC0 Count (factor 1000 / 294912000) and
  then spins on Count until 1/60 s has passed (0x1C37C0). With Count on the host clock its engine and its limiter run
  on real time with no override of its loop. `TTA_WALLCLOCK=0` gives the emulated EE clock back.
- **Sound and IOP timers on the host clock**: `scripts/run.sh` exports `PS2X_SPU2_WALLCLOCK=1` and
  `PS2X_IOP_WALLCLOCK=1` (the SPU2 reads its switch before the override runs).
- **VU1**: the microprograms run AOT-compiled (C++) by default. `TTA_NATIVE=1` takes VU1 off the emulated path: the
  fork's generic `Vu1StreamAdapter` (`native/vu1-stream`) runs every program on its own VU1 copy (the sandbox) in
  stream order, VU1 skips them, and what they draw goes to the GL GS back end at their place in the GS stream (fork
  `tlotr/native-vu1-gs`, also on `tlotr/native`). Boots 116-124 got TTA's first battle to match the VU1 picture at
  the same 19.5 fps:
  - the native frame never ended in game: TTA's frame swap 0x212478 calls sceGsSwapDBuffDc only outside the game
    (a0 = 0), so `tta_native.cpp` now ends it at 0x212478 too (movies still end it at 0x277448);
  - every frame was black: TTA uploads textures as `DIRECT 1` (an IMAGE tag, NLOOP 32767) then `DIRECT 0x7fff` (the
    pixels). VIF1 feeds the second to the image without asking the adapter, which read the pixels as GIF tags and,
    when they looked like draws, numbered groups VIF1 never did; every GS marker after that missed its group. The
    adapter now follows a pending image as the VIF1 interpreter does;
  - textures came out as noise: the native renderer's texture cache decodes from a CPU copy of GS memory, while TTA
    reuses texture pages and CLUTs within the frame and textures from its own GL targets. With
    `Config::gsWrites` (TTA turns it on) the sandbox's batches are drawn by the GS front end, each replaying the
    register writes its packet made (TEX0 with its CLUT load, CLAMP, TEX1, ALPHA, ...), so textures, CLUTs and
    targets are the back end's, in stream order;
  - characters' textures were wrong: TTA sends texture and CLUT uploads between a program's MSCAL and its MSCNTs
    (programs 0x1F8 and 0x1E68), so an MSCNT now opens a draw group of its own on both sides.
  Hot programs in battle (`TTA_VU1_STATS=1`, per 5 s): 8e45395d:0x460 ~20500 (a textured, fogged triangle strip with
  guard-band clipping; its MSCAL only sets up, each batch is drawn by an MSCNT after its V3-32 unpacks), 0x268 ~8000,
  8df1e059:0x10 / 0x190 ~4400 each. Next: translate those into the port's own draws (`Choice::Port`).
  Default since boots 125-130: two 12-minute runs (Cross every 6 s from 55 s), native (128) and VU1 (129), both reach
  the battle, Berethor's scene, the Minas Tirith clip and the Scenes menu (Nazgul Agenda 2/109) at the same frame
  rates; boot 130 runs the defaults to the battle at 19.6 fps. Boot 125 hung once in dlmalloc on a loading screen
  (pc 0x24FAB8); 127 and 128 with the same script did not, so it is taken as the game's heap race (item 19), still
  watched. SC3's `sc3/native-gif-q` (Q carried to the GS for perspective UVs) is in. `TTA_NATIVE=0`: the VU1 path.
- **VU1 0x460 and 0x268 in C++** (`runtime/tta_vu1_port.cpp`, the adapter's `Choice::Port`, matched by the FNV-1a
  of their code): the matrix at VU 0-3 maps straight to GS screen coordinates, Q = 1 / w, the colour from the table
  at VU 136 + index, F = min(129.w * Q, 255), bit 15 of the index the ADC; the triangles go to the GL back end with
  the header's A+D writes (`gsWrites`) and are clipped to the near plane in C++ (the VU's guard-band pass, 0x830,
  is the GPU's job). The first try (boot 132) took vf5 / vf6 (VU 125 / 126) for the screen mapping: they are only
  0x460's clip-test space, and the battle came out as streaks. Boot 140: the battle matches, at 26 fps.
- **Frame loop on the PC's clock** (`TTA_NATIVE_LOOP=0` for the vblank pacing): the frame swap's sceGsSyncV
  (return 0x2124B0) becomes `EeScheduler::waitHostDeadline` (fork `native/frame-sleep`) to the frame cap's next
  deadline, then `accountCycles` for the real time and `presentGuestFrame`, with `setNativeFrames(true)`, as VP2S
  and JC2 do. Movies keep the vblank wait: the MPEG HLE presents on EE-time vsync ticks, and with native frames the
  logo films played at 2.8 fps (boots 134-138; the frame swap's flag at 0x36F35C is 0 in game too, so it cannot tell
  them apart). A frame is a movie's while sceMpegGetPicture (0x27ED48) was called in the last half second. Boot 140:
  movies at 15, menus up to 60, in-engine 35-41 (was held at 30), battle 26 (was 19.9).
- **No CPU copy of GS memory** (`TTA_TEXTURE_CACHE=1` brings it back): `TTA_HOST_PROFILE=<file>` (a sampler of
  host instruction pointers, `runtime/tta_host_profile.cpp`) put a third of the busy thread's battle time in the
  native renderer's texture cache (`TextureCache::uploadLocked`, `GSMem::WriteP8` / `WriteCT32`), a copy of every
  texture upload that no TTA draw reads since they all take the GL back end's textures (`gsWrites`). Off with
  `setUploadsWanted(false)`: boot 145 battle 28.2 fps (was 25.0), same pictures. `TTA_VU1_THREAD=1` (the adapter on
  the VIF1 thread) is slower there, 25.4, and stays off.
- **Where the battle frame goes now** (boots 147-153; START at 80 s skips the intro film, the battle is up by
  ~130 s). The profiler (`TTA_HOST_PROFILE`) now ranks threads by CPU time (`QueryThreadCycleTime`, the driver's
  thread too) and attributes a sample outside the image to the first return addresses into it on the stack. In
  battle: the game thread ~0.7 core, the GS thread ~0.7, NVIDIA's driver thread ~0.6. The GS thread swizzles every
  texture upload into GS memory pixel by pixel (`GSMem::WriteP8` / `WriteCT32`, ~25%) and waits ~40% inside
  `GSNativeBackend::Present`. `PS2X_GS_NATIVE_STATS=1` (fork 00c1480) shows why: per frame 329 draws but 1715
  `glTexSubImage2D` of the VRAM texture, 26635 blocks (6.8 MB, 1.6 times GS memory), only 3 state read-backs and
  0 ms in the fence; the wait is a read-back making the GS thread wait for the driver to get through the uploads.
  Next: far fewer, larger uploads (whole rows merged, a pixel buffer), then a block-wise swizzle.
  `native/gs-backend` (19edc88, acd11f5) breaks TTA's battle (darker, 64x64 block garbage) and is slower: not
  merged (local repro `tlotr/gs-backend-try`).

Boots 55-69 on the way there; boot 69 reaches the title screen with every default and no environment.

## Open

- Movie sound from the 2nd film on: JC2 found the sceMpeg HLE keeping one more copy of the stream callbacks for every
  sceMpegAddStrCallback on a handle whose sceMpegDelete is not routed (jc2/native cc18c10). TTA plays several films.
- The CPU GS rasterizer's magenta movie frames (unsupported texture path).
- Fork branches not on main yet: `native/vu1-stream`, `tlotr/mpeg-nodata-end`, `tlotr/runner-pch`; the submodule
  is on `tlotr/native` (native/renderer + main + those three).
- VU1 AOT flag observer: which TTA microprogram reads a flag the analysis treats as unread (sc3-port).
- The text on the loading screen is garbled (font texture or CLUT upload).
- HLE `sceGsExecLoadImage` copies the whole image into a guest packet: images over ~1 MB would not fit the pinned heap.
