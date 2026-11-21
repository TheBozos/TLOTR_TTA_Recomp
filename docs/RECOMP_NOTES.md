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
  fade, no Z writes). 0x80000 (the screaming Nazgul's coincident copy, template 0x377d40 = *(0x377db0), which sets
  nothing but PRIM 0): 0x225fb0 writes PRMODE 0x68 (Gouraud, fog, blended, no texture) and skins send F = 0, so it
  is a flat FOGCOL silhouette (0x524842 in the scream) over the textured one, blended by the vertex alpha: the
  Nazgul turns into a pale wraith under the scream's green filter (TTA_SKIN_SILHOUETTE_TEXTURED=1 for the old
  textured copy; no console capture of the scream yet). Open: 0x20000's render-target switches. TTA_SKIN_LAYERS=<mask> (1, 2, 4, 8,
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
- **The mist: the Glows objects** (mapped; native since 2026-11-11, below). Shader types 9 and 20 (ShaderObject_Glows_Ps2.c) have no
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
- **The Glows, native** (2026-11-11). They are EA's ears_rt_sfx "Particle System" components (type descriptor
  0x3782c0: create 0x22ec88 / 0x22edc0, update 0x22efe8 -> 0x22d5b8 writes the particles), rendered as shader types
  9/20. On the console they are most of the forest's mist (about 38 blocks of 16 huge grey veils, alpha 5-15, per frame
  in the Berethor close-up), plus sparks, smoke, light shafts and the battle's foliage vignette. PCSX2's GS dump showed
  them as untextured strips until the reader honoured PRMODECONT 0: TTA sets PRMODE with CTXT, TME and ABE, so the
  glows draw in context 2 (TEX0_2 / ALPHA_2 / TEST_2), as many other scene draws do. A block (48 qwords, laid out by
  0x21ea28): +4 the first and end particle, +0xc ages A[16], birth sizes B[16], alpha rates C[16], +0xd0 16 x
  {position xyz + angle, velocity xyz + spin}, +0x2d4 RGBA5551 tints (0x10 is 1.0, the top bit mirrors U). Constants:
  the 12-qword packet at *(S+0x138), +0x10.. = VU c24..c32 (gravity, colour rate / floor / ceiling / birth, anchor and
  spin cap / damping, drag / size cap / growth / reach, atlas rate and alpha cap, atlas count and frame size); matrix
  RC+0x2e0 as 0x21f090 / 0x210d10 leave it, focal RC+0x524, aspect RC+0x5e4. Microcode 0x620 per particle: position +
  (v - g) reach (1 - 1/(1 + drag A)^4) + g A; angle + spin (1 - damping t) t, wrapped; half size clamp(B + growth A,
  0, cap) x focal / w (y x aspect); colour (birth + rate A) x tint, alpha min(birth + C A, cap + capRate A), clamped;
  a 4-vertex strip (the first two ADC), the atlas frame from the age jittered by VU1's random seeded from the
  position. runtime/tta_3d.cpp's glowRender lets 0x21f980 build its blocks, collects the blocks it chains (0x2163c8,
  48 qwords starting 0x6e018000) and draws each particle as a screen-space quad (Material::useQ) with the game's
  TEX0_1 / ALPHA_1 / TEST_1 / ZBUF_1. Up to ~1300 particles a frame; the forest and battle frame rate is unchanged on
  average. TTA_NO_GLOWS=1 turns them off. Trap: with no pad input the game sits on the title screen, where the only
  effect is the UI's "UI_bloom" (one particle); level effects only exist in game (build/pad_battle.txt).
- **The glows' texture is the effect's record, not the GS place** (2026-11-11, later). In the intro's Black Riders
  scene the native glows washed the screen out in green (frame mean 144 / 233 / 163 at host frame 5801): the glows
  took their texture from where TEX0 points (TBP0 0x3013, T8), and the UI's text canvas (0x2c80e8, grey, alpha ~0xe7)
  is placed there too; the effects send their own textures to that scratch place outside 0x219ca8. Each glow's
  texture record is *(S+0xc8) (a group record: the 64-bit pack id first, its home TBP0 at +0x10, e.g. 0x3623) and the
  type-19 mist banks' is the geometry's +0x1c: runtime/tta_3d.cpp's effectTexture looks them up by the id first. The
  real mist textures are soft (alpha mean 5 and 30 of 0x80). The microcode CLIPs each corner against c12 = (2048,
  2048, 2000.1, 2000), but it is not a 2000-pixel cull: the console's GS dump of the Black Riders shot draws veils
  with corners at +-2046 (38 blocks of 16, colour 0x50, alpha 6, the last scene draws #7843..7880). The port skips
  only corners past the GS's +-2048 (TTA_GLOW_GUARD to try another). Natively that shot has ~255 live particles
  (11 glow renders) against the console's 608: the forest's remaining haze gap. Scanning RAM for glow systems
  (S+0xa0 0xb0, +0xa4 1.0, +0xc8 a record with a mist id; TTA_GLOW_SCAN=1 counts them every 60 game frames): the
  console's save state at the close-up has 30 (12 / 12 / 6 by mesh 0x88c690 / 0x89a710 / 0x899b10), ours 12 there,
  but ours rise to 22-33 later in the intro and fall back to 11-14 at some cuts (game 4320, 4740, 5100): the
  same systems, spawned and removed on the cutscene's clock. The console's game frame counter (0x3b5720) is not a
  cutscene clock (4294 there against our 3706 at the same shot), so the two moments are not matched yet.
  Clocks found by diffing native RAM dumps 60 game frames apart (TTA_RAM_DUMP=<game frame>,..., build/ram_<n>.bin):
  float seconds at 0x3a4be0 / 0x3a2364 / 0x3a4bdc (+1.0) and frame counts at 0x3a4be8 / 0x3a230c / 0x3bab24 (+60).
  Natively the game runs 60 game frames per game second; the console's save state has ~59 overall, and PCSX2 showed
  the game at 24-30 frames a second in the forest. So the native game logic runs at 60 where the PS2 ran at 30 or
  less. Tested (a temporary switch charging two vblanks a frame, so the main loop steps 1/30 s): the mist is the
  same at 30 fps logic, systems peaking at 29-33 on the cutscene's clock and 38 particles a glow against 42 at 60.
  So the frame rate is not the haze gap: our count swings 11-33 within seconds, and the one console moment
  compared (the Black Riders shot) likely caught ours in a low phase.
  Correction (same day): with build/pad_intro.txt the native run reaches the first battle at ~host frame 5890 and
  the Nazgul acts first (Nazgul Scream at 5900, Berethor at 0 HP by 6391), then plays a further misty cutscene: the
  native "sword" shot at host 6800 is on that branch. The console's save state (slot 1) runs the intro's end into
  the battle with Berethor's turn first and the menu waiting. So shots after ~5890 are not comparable, and the
  (resolved below: the console's state is a later battle; the first one is scripted for Berethor to lose).
  Driving PCSX2 from a fresh boot to check failed (key presses did not reach the pad reliably; it fell into the
  attract trailer).
  The game's RNG (2026-11-11): its own additive lagged-Fibonacci generator (0x1ba9c8, 20 words at 0x3f96b8, index
  0x3f9708; 93 callers, animation included), seeded once at boot (0x1c2e30 -> 0x1bab38 -> 0x1bab70, Park-Miller
  16807 to fill both tables) from sceCdReadClock: BCD seconds + minutes + hours + 10. Lua's randomseed (0x261668)
  reseeds the first table; newlib's rand (0x320428) is only math.random's and stays at 1 through the intro.
  BozoEngine's sceCdReadClock answers a fixed 12:00:00 unless Libraries::cd.hostClock, which nothing set: every
  native boot had the same RNG. tta_main now sets it when the game runs in real time (TTA_DETERMINISTIC keeps the
  fixed clock; TTA_CLOCK=host / fixed). With clock seeds the Nazgul's opening move varies (Nazgul Scream, Wraith
  Slash with a double critical), but it still acts first and Berethor still falls: the opening turn is not the
  RNG's. Settled: the first battle is scripted. Played with only the New Game press (build/pad_newgame.txt; the
  intro script's press at host 3020 skips the whole prologue film, the one at 3380 nothing that matters) the
  native run goes prologue film -> Berethor in the forest -> the Black Riders -> the battle opening on the Nazgul
  Scream -> Berethor falls -> Idrial's arrival (light burst) -> her cutscene, by host ~24000. The console's slot-1
  state is later: its battle with Berethor's menu and the 'Attack a single target' hint is the next one, after
  Idrial joins. Corrected again: the user remembers Berethor acting first in the first battle, and a PCSX2 session
  at that battle (read from the running process: the EE's 32 MB are one block, found by the ELF's code bytes)
  shows Berethor's menu, the 'Attack a single target' hint and the queue Berethor / Nazgul 1 / Nazgul 2, in the
  same level (e01c01.scp), encounter (Enc01_Nazgul) and with the same stats record (0xcd1f78: HP 177, AP 65).
  There newlib's rand state has moved (0x2781e494): its only caller in the game is the Lua C function
  GetRandomActionInType (0x2679b8 -> 0x17e4d8), the AI's pick. Natively it stays 1 through the Nazgul Scream: the
  AI never picks and the Scream comes first. Not the cause: the RNG seed, 30 fps logic (a temporary switch), the
  pad's idle state, the intro's skip presses, the time manager 0x3bac10 (only its step differs, 1/60 vs 1/30).
  The Lua C functions called around the battle start natively (a temporary probe over the 96 registered through
  0x264498): only GetScaledElapsedTime each frame and SwitchToCombatCam (game 5819); the turn order is decided in
  the C++ battle code (BattleModeDTB.c / Combat.c), not traced yet. Open bug.
  **Fixed (same day): the game's rand.** The turn queue (manager around 0x3a5578; entries {time, .., .., combatant,
  ..}, base time 0x3a557c; current actor 0x3a5a24; Berethor 0xcd11e0, the Nazgul 0xccad40 / 0xcc8220) is built at
  the battle's start by 0x161a48 -> 0x180ee8 -> 0x180018 -> 0x1802b8: each entry starts at 100 (0x17fc30), then
  0x17fd40 adds a jitter when the tuning struct *(0x3d48a8)+0x104 (10.0) is not 0: time += int(f(rand % e[8]) /
  10) - int(e[8] / 10), with newlib's rand (0x320428). Berethor's e[8] is 11: 99 for any roll but 10. The symbol
  map names 0x320428 'rand', so BozoEngine answered it with its native libc generator (Libraries::Rand, a 64-bit
  LCG, another sequence): the roll was 10, Berethor 100, a tie the Nazgul won on list order. The console's state
  (0x2781e494) is three newlib calls from seed 1 (rolls 0, 3, 4: 99, 100, 100). runtime/tta_port.cpp now runs
  the game's own rand on its _rand_next (*(0x37cd14)+0x58): state, queue and opening turn match the console.
  New, visible from Berethor's menu: the hint box shows 'Hint:' without its text, the selection ring under the
  active character has a black square, a white shape at the screen's left (all three below). The host's per-second line now prints the game's frame
  (0x3b5720) beside its own: TTA_GLOW_DUMP and the other probes key on the game's (host 6800 = game 6755 in the
  intro with build/pad_intro.txt).
- **The battle menu: ring, hint, water, waterfall** (2026-11-11). (1) The selection ring under the active character (Cobj 17,
  a 4-vertex static batch, PRIM ABE, an opaque texture: black around white runes) carries no ALPHA_1 of its own; it
  blends with the one in force, DS+0x100 = 0x68 (Cs x FIX + Cd, FIX pulsing 0x1d..0x22): its black adds nothing.
  Static batches with ABE and no ALPHA of their own now take DS+0x100 (TTA_NO_DS_ALPHA=1 for the old alpha blend).
  (2) The hint body is the native variable 'ht' (menu +0x37c, crd_<id>_hlp from subT) the combat HUD reads through
  the binding table 0x395460 (getter 0x2d2190: 'ht' type 3, a char *). Its ActionScript reads it in Open (the menu's
  step queue at +0x5c8: ... Open, DELAY 10, ..., ActivateMenu0's step), which runs before the turn sets the hint,
  and in UpdateHelpText, sent only on input: a cursor move (0x1eb8a8 -> 0x1eb400 / 0x1eb610) or the stick bit 0x800
  (action 0x13, synthesised from an analog axis in 0x1bd0b0). Natively the hint appears on the first cursor move,
  as the game's logic says; the console capture with the hint at once was most likely taken after an input. Debug:
  TTA_STR_TRACE, TTA_APT_TRACE, TTA_BIND_TRACE (string lookups, C-to-ActionScript calls, bound-variable reads).
  (3) The white shape at the left was the type-19 water module's second layer (0x222fd0): an EE image (128 x 128 T8
  at 0x3fef00, a ripple simulation 0x21c420 fills from the scratchpad only while drops are pending, so all 0 here)
  with a cycling grey CLUT (0x3ba300), sent by 0x2221c8 into the UI's VRAM slot (TBP 0x3013, CBP 0x3003). Natively
  the place resolved to the UI texture last put there. 0x2221c8 now makes it a live texture at that place, like the
  text canvas (TTA_NO_WATER_LAYER=1 off). And the mist banks take their vertex colours from the animator's own
  output (0x2236a0 writes a VIF stream: V4-32 positions, V2-16 ST, V4-8 colours), matched to the native vertices
  by position: the second layer's alpha is 0 there (invisible without ripples) and the first layer's colours are
  about half the native estimate (TTA_MIST_OWN_COLOURS=1 for the estimate, TTA_MIST_PACKET=<frame> dumps the
  stream). (4) The waterfall is glow particles (types 9/20; the splash texture 0x33bb7793, spray 0x1c33bb7793,
  mist 0xabb46ddb, ALPHA 0x44): dim natively because each particle wrote Z and hid the ones behind it. The glow's own
  packet sets ZBUF_1/2 = 0x131000100 (ZMSK 1: tested, not written) and TEST 0x5000a, and its last block restores the
  scene's 0x31000100, which is what DS+0x120 holds when the render returns; a PCSX2 GS dump of the forest shows the
  glow draws (PRIM 0x254, TEX0 0x3623) with ZMSK 1. Natively the glows now take the ZBUF their packet sets (the first
  ZBUF_1 A+D among the glow's DMA blocks): the fall is bright and streaked as on the console, the selection ring gets
  its blue-white sparkle, and the forest's veils add up: the intro close-up (host 3751) is 69 / 68 / 60 against the
  console's 61 / 58 / 50 (63 / 62 / 54 before), the haze closer, a little bright.
  Where the 8 levels come from is open: without the screen filters the close-up is 38 / 36 / 33, without glows
  53 / 52 / 44; native glows there cover 17 screens (5 systems, alpha 6, colour 0x50: the console's values), and the
  only console GS dump with glows is a later shot (the sword close-up: 152 strips, 472 kicked triangles, 70.6
  screens), so the coverage at 3751 is not known; the systems follow the cutscene clock (above). TTA_GLOW_DUMP now
  prints each glow's on-screen coverage. The battle's first frame overlaid on the console capture lines up exactly
  (camera RC+0x1a0 / +0x260 identical in both RAMs); the attack turn (target hint, hit sparks, 'Immune', the Nazgul
  Scream's green veil) plays natively but has no console reference yet.
- **Past the first battle** (2026-11-12). build/pad_long.txt (the intro's three CROSS, then CROSS every 60 frames
  from 5960) plays the scripted loss (Wraith Thrust, Berethor at 0 HP), Idrial's light (the blue flower, white
  flashes), the healing cutscene, then free control in the forest (minimap, the "Scenes" tutorial box) and the first
  Epic Scene film (Minas Tirith) by host frame ~11800, all native and deterministic. Skin mode bit 0x80000000 (the
  Nazgul, scbm 70063 / 70388) changes nothing in the skin render (0x225fb0 tests 0x200 / 0x1000 / 0x2000 / 0x40000 /
  0x80000 / 0x20000 and 0xc0000 to skip); the second skinned layout is the shadow volumes, drawn (type 18).
  Driving the forest (build/pad_walk.txt): after the battle a "Scenes" tutorial box waits for CROSS, then "Quest
  Journal updated", then the prompt "You received an Epic Scene ... View it now?" appears a while after the last
  input (each press pushes it back), so it is answered with DOWN + CROSS pairs every 60 frames; START opens the
  pause menu (Scenes orbs, Menu, Stats screens: all native). The D-pad drives the left stick (BozoEngine's
  ScriptInput): Berethor walks, the minimap follows, a chest lies on the path with its "Chests" tutorial box.
- **Shadow volumes' length** (2026-11-12): long straight dark bands across the ground (reported at the orcs' legs, seen
  from Berethor in the first battle's attack shot) were the volumes swept 3x the body's size along the light. The
  console's (the GS dump of the battle, count-buffer strips into FBP 0x180) are about the size of the bodies that
  cast them. Set 5's program 0x178 builds the far cap as the transformed vertex + VU row 13 ((2, 2, 1, 0.033) in the
  first battle) times a per-vertex factor (0x07a0, 0x0918), in its own transformed space, so the native sweep now
  defaults to 0.3x the body's size (TTA_SHADOW_REACH=3 for the old length); porting that exact extrusion is open (done 2026-11-20: "Shadow volumes swept as on the console").
- **Save states** (2026-11-18, the user's request through JC2): BozoEngine's save states (JC2's b73692e, docs/
  save-states.md) merged into TTA's engine branch (tta/save-states, b6dc6cd: libmpeg's Nodata end became a Resume
  record keeping TTA's SDK callback; cdvdman keeps both sides' commands; libpad2's sockets, the IOP software
  registers and libmpeg's inputEnded added to the archive). TTA's section "tta": the AUDIOPF host
  (CompiledLibraries::serialize) and the driver commands waiting for the game, the resources the game loaded
  (tta_2d's g_resources) and where it placed its converted textures (g_placed; the live canvas and water textures
  are rebuilt when re-sent). The delivery end that was a lambda is a port Resume kind (kPortResumeKinds + 0).
  --save-state FILE --save-state-at N, --load-state FILE [--load-state-at N], --dump-ram FILE; in the window F5
  saves, F7 loads, F6 / Shift+F6 pick the slot, and the F2 window has a Save states tab. The frame report moved from
  F7 to F10. Checked headless: a save at 9000 (taken at 9094, after the Epic Scene film: a movie makes a save wait)
  and one mid-battle at 5800, each loaded in a new process and mid-run, gave the same EE RAM as the run that never
  stopped; the window build loading the battle state draws frame 5990 pixel for pixel as the uninterrupted run.
- **Named states for checks** (2026-11-18): start checks from a state instead of replaying from boot
  (--load-state FILE; frame numbers stay absolute, so --capture-frame and --frames count from the run's start).
  build/states (not in git; a state loads only into the build that wrote it, so make them again after a change to
  the state layout or TTA's section), all headless:
  - title_2400.state: tools/pads/pad_none.txt, frame 2400 (the title menu, Load Game selected when a card has a save).
  - outpost_3900.state: tools/pads/pad_load.txt with --card-dir build/cards_test (a copy of the user's save,
    Eregion Elven Outpost), frame 3900, standing at the save point.
  - battle1_5800.state: tools/pads/pad_attack.txt, frame 5800 (the first battle, Berethor's turn coming).
  - forest_9094.state: tools/pads/pad_explore.txt, asked at 9000, taken at 9094 after the Epic Scene film.
  - 2026-11-19: BozoEngine master with TMB's save states (#104) adds the 989SND driver's section "snd", so a state
    from before it is refused ("expected section snd, found timers"; the run goes on as it was). The scripted states
    were made again; user_battle (the user's sunny battle, F5 at 9329) and the window's slots need saving again.
- **Game time and stalls** (2026-11-13, the user saw the character speed up for a moment on the overworld, not
  reproduced): GetScaledElapsedTime (0x2674f8) returns 0x1dd4f8(1) = *(0x3bac4c) x *(0x3bac7c). In a real-time
  window run with a 90 ms stall every 300 frames (TTA_STALL_EVERY=300) the step stays 1/60 s through the stalls;
  over the forest walk two frames took 2/60. The pacer never catches up (a late frame restarts its schedule), and
  the game's frames a second never pass 60.04. TTA_TIME_TRACE=1 prints every step that is not the usual one.
  **Found and fixed (2026-11-20).** Not a stall but the EE clock's leaps: during streaming the engine models the
  disc's waits (that the PC does not have) and the EE clock jumps up to five fields in one frame; the game measures the
  frame's time from it (0x1c4018: *(0x3ba70c), the frame's seconds, times 0x1c4240's *(0x35fcd8), a global 1.0, to the
  time manager 0x1dd588), so in the user's forest clearing (build/states/user_slow_16590.state, a scripted walk in
  real time) runs of ~5 frames had steps of 0.033-0.083 s while each lasted 1/60 s: the party moved two to five times
  as fast. In real time, tta_port.cpp's hook on GetScaledElapsedTime (0x1dd4f8, called only through the table: 33
  callers; 0x1c4240 is called directly) scales each frame's elapsed times by the host's own fields since the last
  frame (at least one) over the measured seconds: 46 off steps on the walk -> 0. Scripted deterministic runs keep the
  console's count (the first battle's captures unchanged). The same leaps made the sound overflow (BozoEngine #111).
- **Stray shadow wedges** (2026-11-18, the user: "weird shadows from time to time"). Found from the user's state with
  an attack played (frames 9900-9940): a large straight-edged dark wedge of ground by a character close to the camera.
  The volumes were drawn at the type-18 pass, in the scene's traversal, and counted where the depth test passed
  (z-pass): closed prisms count 0 there, except where the near plane cuts one or the camera is inside one, which gave
  the wedges (and a whole-screen darkening when the camera was in Berethor's prism). The console draws them in layer
  4 after the scene (0x21a778 inverts Z, 0x21aa88 clears the count, the volumes go in with Z GREATER against the
  inverted Z, i.e. counting the faces behind the scene, and 0x21ae10 darkens). Natively the volumes are now queued
  and drawn at 0x21ae10 with z-fail (BozoEngine Material::stencilDepthFail), then one darkening at the strongest
  strength; TTA_SHADOW_ZPASS=1 / TTA_SHADOW_IMMEDIATE=1 for the old ways. The wedges are gone.
- **Debug: battles and warps** (2026-11-20, the user's request). runtime/tta_debug.cpp; how it works is in its header.
  Found: the Lua functions the level scripts call (StartEncounter, TeleportToPos, SpawnEnemy... registered through
  0x264498, StartEncounter's C side 0x268668 -> 0x17f040 -> 0x15b6c8(id)), the current map's entity table
  (*(0x3dff84), "manC", records {id, name[28]}; Eregion: 1675 records, ids 1000-2674, encounters Enc01_Nazgul 2109 ...
  Enc27_WatcherBattle 2133; the scripted first battle is 2109, started from 0x15b94c), the map list (14 paths at
  0x35bc88, the index at 0x35bd0c, 0x190130 sets it, chapter message 0 to handler 0x18f420 loads it; message
  categories are routed by range from the table at 0x35dec0: 0x7d0..0x833 -> 0x18f420), and the developers'
  command-line options (0x140670; -level never applies since the current-map buffer is always set). Checked from
  build/states/forest_14900.state (made with tools/pads/pad_explore.txt): every map index loads (Helm's Deep's gate by
  host frame 16600; e02c04 still black there), StartEncounter(2118) is a warg fight, StartEncounter(2109) during the
  forest film fights the Nazgul with the film still running, the random battles' _EP / _ET ids alone end at once on
  "Experience Earned" (a random battle pairs them elsewhere; not found yet). The first battle's capture is unchanged.
  TTA_ENCOUNTER_TRACE=1 prints every encounter start.
- **The draw work off the game thread** (2026-11-20, the user: a PS2 game should hold 60 with another game running).
  Everything ran on the game thread: in the user's forest clearing (build/states/user_slow_16590.state) ~12.5 ms of
  the 16.7, so any competition for that core dropped frames (38-45 in the user's session). Two changes:
  (1) BozoEngine ps2f (branch tta/softfloat-fast): addNearest and mul take the host's float add / multiply when the
  operands and the result are normal (exponent 1..254), where the exact result rounded to nearest even is IEEE's;
  zeros, denormals, exponent 255 and out-of-range results keep the exact path (BOZO_PS2F_EXACT_ONLY). The first
  battle and a forest walk end with the same RAM; the battle ~15% faster headless. (2) tta_3d.cpp's draw queue: the
  3D draws are recorded in order and their vertex work is a job (a static batch's colours, fog, triangles and
  clipping; a skinned batch's skinning and lighting, on its bones / W / lighting / fade copied at the draw), run by
  workers (TTA_DRAW_THREADS, default min(6, cores / 2)); tta::flushDraws runs the jobs and submits in order before any
  other use of the renderer (the 2D, the screen filters, the frame's begin and end). The shadow pass's skins and the
  skin debug switches stay on the game thread; shadow meshes outside the shadow pass are no longer skinned for
  nothing. Pixel-identical (15 frame pairs of the battle and the clearing against earlier builds and against
  TTA_DRAW_THREADS=0). Unpaced: the clearing ~80 -> ~125 frames/s, the first battle ~88 -> ~120. In real time, the
  clearing walk with two other copies of the game running flat out beside it held 59.4-60.2.
- **The sound's crackle** (2026-11-20, the user: "sometimes the sound crackles"). Measured with the window build in
  real time on SDL's dummy audio driver (SDL_AUDIO_DRIVER=dummy TTA_SCRIPTED_SOUND=1 TTA_AUDIO_STATS=1: the device
  drains at 48 kHz, nothing plays) on the user's forest clearing walk: 7 underruns and ~158k dropped samples in a
  minute on a quiet PC, 154 underruns with two other copies of the game running flat out. Two causes. (1) The drops:
  TTA_AUDIO_RATE=1 shows 801 samples a frame normally, but up to 4004 in stretches: the EE clock leaps (the engine
  models the disc's waits the PC does not have) and the mixer, brought up to each frame's end on the EE clock, gave
  the whole leap's sound in one host frame; the output queue (cap 64 ms) then dropped 0.2-0.6 s at a time. BozoEngine
  (branch tta/audio-clock-leaps): in real time the frames' sound is held to the host's time (a budget of samples
  growing at 48 kHz, spent by every output, stallSound's too; the surplus skipped where it was made). (2) The
  underruns: the game thread's dips under load. TTA's queue is now 64 ms (cap 160) instead of the engine's 32: a
  turn-based game loses nothing to it. After both: quiet, 0 underruns and 0 drops; under that load, 0 underruns and
  one ~0.2 s drop in a minute.
- **Debug: random battles and any enemy group** (2026-11-20). The map's objects carry the links: an encounter
  (type 0x1775 at +4: id at +0, its enemy group at +0x28) and an enemy group (type 0x1770, an "_ET" record: its
  placement, an "_EP" record, at +0x24; Eregion: e1c1_Rnd_Easy_01_ET -> e1c1_Rnd_2enemy_EP, Rnd_Med_01 -> the
  1-enemy layout, the story groups -> their own). StartEncounter takes an encounter only (an _EP or _ET id alone opens
  an empty battle that ends on "Experience Earned"). The random encounters are encounter objects too (Rnd01_OrcAmbush01
  2135, Rnd02_MoriaApproach 2136, Rnd01_05_Easy...), named Rnd.., which the name filter had missed: the list now comes
  from a RAM scan for {id, type} with the id one of the map's records (on each map load). Any group in a battle: a host
  encounter (the first Rnd.. one) gets its +0x28 pointed at the group, is started, and gets its own back 600 game
  frames later. Checked from forest_14900: Rnd_Easy_01 (two orc archers), Rnd_Hard_01 (two wargs), the Watcher's
  group (the Watcher as an ordinary fight), Rnd01_OrcAmbush01. How the game itself rolls them: danger zones. Entering
  one sends the encounter manager (0x3a4f98, 11 pools of 0x1c-byte headers) message 0xc8 with the zone's id
  (0x17bc98 -> 0x17a080(manager, 10, id, 0)), 0xc9 on leaving; 0x17afc0 then runs the active zone (0x178f38: game mode
  2, no film...) whose countdown (+0x50) falls with the scaled time, and at 0 0x178e88 draws an encounter from its
  list (+0x84, count +0x54, a shuffled deck: +0x5c left) and starts it (0x15b6c8). 0x178d18 fills a zone from its
  definition (+0x24: up to 4 ids) and the pool of kind 5. No zone was entered from any state here (forest, outpost, the
  user's clearing); TTA_ZONE_TRACE=1 prints zone messages and updates. The user's clearing state
  (build/states/user_slow_16590.state, F5 at 16590) held 59-60 frames/s in real time on a quiet bench (95 unpaced);
  the drops to 38-45 in the user's session did not reproduce.
- **Debug battles outside Eregion** (2026-11-20). Two faults stopped them. (1) The scan's name rule: Rohan (and the
  later maps) name their groups "..._Encounter_Template" / "...EncounterTemplate" and placements
  "..._Encounter_Placement" / "...EncounterPlacement", cut at the record's 27 characters ("sBridge02_Encounter_Templat"),
  where Eregion has "_ET" / "_EP": Rohan listed nothing. Templates and placements are now recognised by either form
  (Rohan: 83 encounters, 39 groups: EncounterInstance_Random1.., EI_Morwain_Random1.., Instance_Morwain, the
  bridge / tower / worm / Sharku fights). TTA_DEBUG_LIST=1 prints the ids and names on each scan. (2) StartEncounter
  was called from inside the main loop's hook 0x216118 (callGame): in Rohan the call switches the game's threads
  on the way and never comes back to the hook, which left the loop broken (the frame stopped at 0x216118, "did not
  return"). The hook now hands the loop's own call over to 0x17f040 (a0 the id, ra the loop's jal 0x216118, so the
  loop asks the hook again when it returns): one battle a tick, the game's own flow. And an id that is not one of the
  current map's encounters is refused instead of started. Checked: Rohan encounter 1172 from forest_14900 (warp at
  15000, --encounter 1172@16200): four orcs, Rage of Saruman, the party's loss, back to the forest.
- **Helm's Deep's debug battles: no ground, mounted party** (2026-11-21). Not a rendering fault: the battles were
  started inside the map's opening films. After the warp (forest_14900, warp at 15000, the START / CROSS script of the
  earlier audit) Helm's Deep plays in-engine films (the ride to the gate, Theoden) until about game frame 19140, and a
  battle asked for at 16200 began inside them. The battle itself was right (fought at its placement near x 2700:
  the placement object, type 0x1773, the group's +0x24, +8 .. +0x10 the place, +0x1c an angle; Deeping Wall 1126 at
  (2689, 10.5, 37), the breach 1129 at (2706, 3.9, 60)), but the film's five riders (Berethor, Idrial, Morwen, Elegost,
  Hadhod on the map's Horse_SG02_01..05, parked at (230, 18.7, 155)) and its camera stayed: about half the shots
  (TTA_3D_DUMP_FRAME=%20 now prints the camera's world place, solved from the clip matrix) were taken at (184, 21,
  134), the valley, where during the battle only four terrain pieces were drawn: mounted riders over black. Only the
  zones round the film's valley were in, so the battle had no ground either. The game's state word 0x3a5294 is 5 in
  a film (both films), 6 exploring (the forest, the Hornburg) and 3 in battle; the debug tool now holds a battle
  while it is 5, asking again every second, for two minutes at most (state 5 is also the game's prompts and the
  movies they play: the Gullies' "Epic Scene, view it now?", where a battle started well before). Started at 19140,
  1126, 1129 and 1133 are on foot on the wall, the breach and the court, with every shot at the battle. A first try
  (moving the party with the game's teleport 0x11a880(actor, position, 1), as Lua's TeleportToPos does through
  0x119888) brought the ground but not the film's shots, and Helm's Deep's 0x15c030 actor is the film's Berethor,
  not the explorer; it was taken out. The party list is 0x3a6e50 (0x3a5bd0 + 0x1280, as GetPartyMemberByName's
  0x1941f8): count at +8, member k's actor at +0xba4 + k * 0x20 (gonj, fewj, rofk, duwj, mdwj, eadj).
  TTA_DEBUG_OBJDUMP=1 prints the encounter's, group's and placement's words.
- **Starting without the disc, starting from Explorer** (2026-11-21). With no disc image at the --iso path (default:
  the repository root's No-Intro name, -DTTA_ISO) the game booted and stalled at its first read: a black window, no
  message. tta / tta-headless now check the ISO and the executable first and stop with where they looked and what to
  pass (code 2; a played window build also shows it in a message box, as it does host.setUp's errors). And tta.exe
  did not start at all without MSYS2 on the PATH (exit 127): everything is linked static except FFmpeg (avcodec-63,
  avutil-61), whose MSYS2 build pulls ~80 DLLs. A post-link step (cmake/copy_runtime_dlls.cmake, MinGW only:
  file(GET_RUNTIME_DEPENDENCIES) against the compiler's bin folder) copies them next to the executables, only when
  missing or changed; both run with a PATH without MSYS2. Then (same day) the lean way: scripts/build_ffmpeg.sh builds
  FFmpeg 9.0.2 (LGPL; the TMB build kit's release and configuration, swscale left out) with only the mpeg2video
  decoder and mpegvideo parser, static, into build/ffmpeg (1.9 MB of libraries; the tarball checked against its
  SHA-256; ~2 minutes; on Windows it runs itself again in MSYS2's UCRT64 shell, whose make cannot start the
  compiler from Git Bash, and needs pacman -S make). CMakeLists.txt hands it to BozoEngine as BOZO_FFMPEG_ROOT (the
  engine already took a static FFmpeg folder) and build_runtime.sh builds it when missing (TTA_SYSTEM_FFMPEG=1: the
  system's through pkg-config, the DLL copy then still there). tta.exe imports only Windows' DLLs; the 18
  regression captures (the boot films among them) are identical to the MSYS2 FFmpeg's.
- **Frame times in the later regions** (2026-11-21). PS2X_FRAME_STATS=1 (BozoEngine's pacer: the work of each frame,
  the wait for the cap left out), window build on the PC's clock, after a warp from forest_14900 with
  tools/pads/pad_warp_battle.txt (note: --frames counts from the boot, not from the state): Minas Tirith, Osgiliath
  and the Pelennor Fields run at 3-10 ms a frame (a few single frames over 16.7 ms around loads). Helm's Deep holds 60
  except from about game frame 18900 to 19550, the end of the opening films and the start of debug battle 1126 (the
  camera over the army on the wall): 16-19.5 ms on average there, 49-57 frames a second for ~10 s, then 60 through
  the battle. Reproduced unpaced from a state at 18896 (TTA_DETERMINISTIC=1, --encounter 1126@18900). TTA_HOST_PROFILE
  (its first report used to give every thread 0 cycles and list the wrong ones: a thread's CPU time now counts from
  when it is first seen) puts the game thread at a full core: ~25% the game's own VU0 macro code (vu0::madd / fmac /
  ps2f::addNearest / mulf, from game functions 0x1d5118 and 0x1d52c8, already on the host-float fast path for normal
  numbers), ~17% the vertex copies into Renderer::draw at the flush, ~10% drawShadowVolume's face sweep, then drawSkin's
  recording. The sweep now projects each of a prism's six corners once instead of once per face (it was each corner up
  to six times): its share halved, pixels unchanged (scripts/regress.sh), the frame time not measurably. Then
  BozoEngine#112: VU0 macro add / sub / mul / madd / msub take four fields at once through SSE when every enabled
  field is a normal operation or a zero rule (a zero factor: the signed zero; a zero addend: the other operand), else
  field by field as before; bit-exact with the flags (ctest vu0_simd, 2 million random cases). The stretch from 18896
  to 19700 unpaced: 7.7-8.6 s instead of 10.2-10.6 s; in real time 56-60 frames a second instead of 49-58 (the
  battle's own load is the 56). Captures pixel-identical. The vertex copies into Renderer::draw at the flush (~420K
  vertices, 13 MB, a frame there; ~12-15% of the game thread's busy samples) were tried in parallel (2026-11-21, not
  kept): a Renderer::drawSpace handing out each batch's place, the draw workers copying their outputs into it. The
  headless stretch to 20600: 15.35 s against 15.53 s, about 1%. The copy is bound by memory bandwidth, and the space
  must first be filled (std::vector::resize writes Vertex's defaults), which costs about what the copy did. Only
  jobs writing straight into the frame would save it, which needs each batch's vertex count before its clipping.
- **The attack cameras' black bars over the HUD** (2026-11-21, checked, left as is). In a battle's attack camera the
  letterbox (two opaque black sprites from 0x1c68a8 / 0x1c68c0, through the immediate primitive 0x218910) covers the
  top of the attack's name and portrait and the bottom of the HUD's box (Rohan 1172 at 16590). The game builds one
  linear chain at 0x3b5be8 (write pointer +4, appended by 0x2163c8): a temporary probe at Rohan's game frame 16400
  shows the frame's clear (0x211de4), then EA Apt's HUD (0x305330), then the screen filters (0x1cbafc, 0x1c1d34,
  0x1cde84, 0x1cd9dc ..) and the bars last, so the console draws the bars over the HUD too. Natively the HUD is the
  overlay, composited after the frame; the bars come through the same overlay after it, as on the console. The
  overlay does differ in one way: the screen filters, which the console sends after the HUD, are drawn into the
  frame under it, so the console's HUD may take their glow and tint where the native one does not. No console
  capture to compare with yet.
- **Visual regression check** (2026-11-21): scripts/regress.sh runs the window build (TTA_DETERMINISTIC=1) on five
  scenes from the local states (the first battle 5900 / 6300 / 6800 with tools/pads/pad_attack.txt, the outpost 4500 /
  5451, the forest 14940 / 14971, Rohan's debug battle 1172 at 16750 and Helm's Deep's 1126 at 19800 / 20400 with
  tools/pads/pad_warp_battle.txt) and compares each capture with build/regress/baseline (tools/tta_regress.py: mean
  difference and share of pixels moved by more than 8 levels; a sheet of the failing pairs). --update makes the
  baseline; scene names limit a run. ~1.5 minutes for all five (~3 with the lockstep below). Two things made runs differ: (1) unpaced, the window
  presented whenever it came round, so a capture landed a frame or two late by chance (frame_005452 for 5451, the
  outpost's pulse 10 levels off): the game thread now holds at the end of each capture frame until the window has
  captured it (WindowOptions::onCaptured), and every capture lands on its frame; (2) the flare occlusion read the depth
  of whichever frame the window had presented last (BozoEngine reads depth back asynchronously, by wall-clock tile
  requests): off the PC's clock (headless, TTA_DETERMINISTIC) every pixel of the flare's block now counts as clear, as
  headless already did. Two runs then match to 0.2 levels at most (the Scream at 6300, Rohan), the rest exactly.
  (3) Those two drifted further later (0.55, 0.9 at the Scream): the Scream's ghosting keeps the previous picture
  (the render targets and kPreviousFrame live across presents), and unpaced the window skipped some of the game's
  frames and showed others twice (its 1/30 s wait re-presents). The window build's TTA_DETERMINISTIC=1 runs now use
  BozoEngine#113's lockstep: the window presents each published frame once, after the frame has ended, and the game
  thread holds after each frame until it is presented (vsync off; TTA_NO_LOCKSTEP=1 for the old way). Every capture
  is now pixel-identical run to run (tta_regress.py prints "identical"); the baseline was remade, the Scream's frame
  changing (its ghosting now over every frame). A full check takes ~3 minutes.
  Three more scenes (same day): boot (no state: the films at 1500 and 2000, the title at 2380), pause (forest_14900,
  tools/pads/pad_pause.txt: START, the Menu tab at 15050, DOWN to Equip at 15140, CROSS to the party at 15260) and
  idrial (forest_9094: the in-engine film where Idrial heals Berethor, 9450 / 9750, her glow). The forest scene now
  runs with tools/pads/pad_none.txt like them: without a pad script a run is not "scripted", so its window showed and
  its sound opened. 18 captures in ~3.2 minutes, each pixel-identical over three runs. user_battle.state (the sunny
  battle) is from before the sound section of the save states and no longer loads (the loader puts the game back and
  it boots to the title): that battle needs saving again before it can be a scene.
  Three later regions (2026-11-21), after a survey of a battle in each of five (captures from 15800 to 19000; no
  defect found: Minas Tirith's and the Pelennor's near-black frames under the battle HUD are the battles' fade-in
  after their films): moria (warp e01c02, the Watcher 1000 at 16200: 16700 / 17300, the tentacles and the water's
  splash), osgiliath (1269, Gothmog's archers: 16700 / 17900) and minastirith (warp e03c02 alone: its opening films
  lead to the troll battle; 16080 in its fade-in, 16200). The warps use the map paths: regress.sh splits a scene's
  arguments on spaces. 24 captures in ~5 minutes; the new ones pixel-identical over two runs.
- **Scene drawing overhead** (2026-11-20). TTA_HOST_PROFILE on the first battle (battle1_5800 to 7500, the window
  build on the game's clock): the main thread spent ~20% in drawFor / drawNode (a copy of every batch's vertices into a
  new vector each frame, a texture lookup by id then by file name through two string maps per batch, the mesh lookup by
  a string tuple per node, fminf per fogged vertex), 7% in clipToDepthRange (every vertex tested against the near and
  far planes) and 7.5% in operator new. Now: reused buffers (drawNode, the clipper's output), each batch's texture
  and bounding box kept on it (a box wholly inside the depth range skips the clip test; the planes are linear, so its
  corners decide), a last-hit cache in front of meshFor, and comparisons instead of std::fmin / fmax in the fog and
  skin-lighting loops (C library calls on this toolchain, a dozen a skinned vertex). drawFor is 5.5%, allocation is
  gone from the profile, and the battle's 1700 frames run at ~105-109 frames/s unpaced (64-78 before, noisy with
  the other ports' work on the machine). Captures that land on the same frame are pixel-identical (battle 5900,
  outpost 5451, forest 14940 / 14971). What is left on the main thread: the game's own VU0 maths (madd / msub /
  fmac / mulf, ~25%), skinning (~20%), the renderer's batching (~8%).
- **The outpost's pulsing whiteout** (2026-11-20, worked around). From the outpost state (build/states/outpost_3900.state with
  tools/pads/pad_outpost.txt, a short walk; build/states/outpost_pulse_5440.state just before it), near the stone
  with the crystal the frame bleaches to cream every ~180 frames (mean 37 -> 235, Berethor and the HUD stay dark).
  It is the frame-alpha glow filter of the sunny battle, run three times a frame: 0x1cbafc fills the 256 x 256 target
  with ff59727f (cream), 0x1cf1fc multiplies it by the frame's alpha (ALPHA 0x89, the frame read as CT32 with TCC),
  0x1cf8a0 blurs it in two three-tap passes whose weights pulse (0x05/0x10/0x05 dark, 0x0d/0x29/0x0d bright), and
  0x1d0540 adds it back (0x48, 0x7f). The frame is read as a 24-bit image (alpha = the vertex's 0x80), so As = 1 on
  every pixel and the cream lands everywhere; the scene's own alpha is 1 natively as well (keeping the opaque draws'
  alpha, or reading the frame's alpha, changed nothing). Before the gsBlend change (2026-11-18) 0x89 fell back to
  (Cs - Cd) As + Cd, a copy of the frame, and the pulse was a milder brightening (62). On the console it must land
  only where the scene leaves alpha; what the scene writes there is the open question (a console capture at this
  spot settles the strength). The user chose the old look meanwhile: a 0x89 takes the exact form only when a 0x46
  darkening ran in the last two frames (the sunny battle's chain), else the old fallback; the outpost's peak is 63
  (TTA_GS_BLEND_89=1 for the exact form everywhere, TTA_NO_GS_BLEND_2D=1 for the fallback everywhere). Not
  rechecked in the sunny battle (its state needs saving again).
  Later (2026-11-20), toward emulating the frame's alpha: (a) the scene's static draws all run with FRAME_1 FBMSK
  0xff000000 (outpost and first battle): the console's scenery never writes alpha; (b) the game clears the frame's
  alpha itself just before the glow chain (0x1c1d34: a full-screen alpha-only draw, FBMSK 0x00ffffff, rgba 0);
  (c) BozoEngine keeps the frame's alpha as coverage for presenting and samples it as 1.0 (uTarget 1) whenever a draw
  reads the frame. Tried (reverted): FBMSK as the 3D draws' colour mask plus an engine Material::frameAlpha reading
  the frame's own alpha. The frame's alpha was then 0 at the chain's read (a debug view of it), yet with the exact
  0x89 the peak still reached 201 (skipping 0x89: 235, skipping the blur or the add-back: 26), and with the old
  fallback 220. Something else in the chain (the targets' alpha under the post draws' FBMSK 0xff000000, the GS's As
  past 0x80) is still not as on the console; without a console capture of this spot it stays open. PCSX2's GS dumps
  do not help here: with the hardware renderer their VRAM snapshot has the textures but not the frame buffers.
- **All regions reached through the warp** (2026-11-20): Western and Eastern Moria, the Gullies, Rohan, Helm's Deep,
  Osgiliath, Minas Tirith and the Pelennor load, play their films and battles (cave trolls, Uruk archers) with no new
  rendering faults seen (build/msv, a START / CROSS / walk script after each warp).
- **Character shadows on the ground** (2026-11-20). After the wedge fix no count was left anywhere: the depth was
  fine (the type-18 pass uses the same viewport and clip matrices as the character's own draw, and up faces alone,
  TTA_SHADOW_ZPASS=1 TTA_SHADOW_UP_ONLY=1, darken the whole volume), but the sweep was too short. A volume is the
  body swept along the light (0x377990 = (2, -2, 0): down and sideways at 45 degrees, y up) by TTA_SHADOW_REACH x the
  mesh's size, and at 0.3 it ended just under the feet. At 3 the shadows land on the ground beside each character
  (first battle, frame 5900: up to 12 levels darker at the game's strength 0x13, about 15%, a faint shadow in that
  night scene). TTA_SHADOW_SHOW_LATE=1 draws the queued faces in colour at the shadow layer, depth tested;
  TTA_SHADOW_ORDER=1 also prints each skin draw's viewport and clip Z.
- **The later regions' static scenery: shader type 5** (2026-11-20). A survey with TTA_3D_TRACE=3 after a warp to
  each region (Western Moria, the Gullies, Rohan, Helm's Deep, Osgiliath, Minas Tirith, the Pelennor; a START /
  CROSS / walk pad script) lists every static render the game runs: only type 5 (0x221760) was not drawn, in the
  Gullies, Rohan and the Pelennor, and no static node is left unconverted anywhere (the "not converted" counts are
  the skins). Type 5 is two objects on the whole disc, Cobj 33 (a 62-vertex sheet with a flowing-water texture) and
  Cobj 70000 (a 100 x 100 quad, an 8-frame ripple flipbook): the river's surface over its sky reflection. The
  render is type 0's (the instance's packet at inst+8) with microcode set 1 and DS mode 5 (ALPHA 0x44 for one
  node, 0x48 additive for the other), so the native draw comes after the game's render, which sets that state.
  Set 1's program 0x58 (read with the same throw-away VU disassembler) transforms like set 0 and passes the colour
  through, but reads a parameter qword P after each PRIM tag and adds floor(frac(P.xy x T) x P.z) x P.w to s and
  t, T = VU 130.x = DS+0x530, which the set-up 0x221630 writes before the render (the game's counter / 29970).
  tools/tta_meshes.py keeps P as two pseudo-registers (0xf0 = x, y; 0xf1 = z, w) and the runtime shifts the
  texture coordinates (Rohan's river: P = (29, 0, 10000, 1e-4), a slow drift in s). TTA_NO_TYPE5=1 and
  TTA_NO_TEXTURE_ANIMATION=1 turn them off. 0x2201a8, many calls a frame in the Pelennor, draws nothing: it is a
  set-up of the same kind (a doubled vector into DS+0x5cc..0x5fc, and the time: type 4's colour weights, below). The pause
  menu's black is the game's own: its Apt movie opens each frame with an opaque full-screen black quad, and the
  screen filters then blur that frame.
- **Shader type 4: the fire** (2026-11-21). The chain audit (TTA_CHAIN_AUDIT=1) run while exploring, not only in
  battles (a warp to each region from forest_14900, the films skipped, to 18000; build/chain/walk, local), found
  three callers no battle audit had seen, all in Eastern Moria: 0x22038c (set 0's MPG, 1004 qwords from 0x3492e0),
  0x2203f8 (MSCAL 0x767: program 0x3b38) and 0x220478 (the mesh), from shader type 4's render 0x220330 (type table
  0x35f690: set-up 0x2201a8 at +8, render at +0x2c). Type 4 is Cobj 19, 20 and 21 in every level: sheets of flame,
  drawn additively (ALPHA 0x48, Cs As + Cd) with Z GEQUAL. The native side only traced it. Program 0x3b38
  transforms as type 0 and shifts s / t as type 5 (the parameter qword after the PRIM tag, VU 130.x = DS+0x530 =
  the instance's time / 29970). It also reads a fourth vertex array (V4-8 through STROW 8388608.0): RGBA = colour +
  (delta - VU 130.y) x VU (4 + i).w, where VU 130.y = DS+0x534 = 0x4b000080 (8388608 + 128), i = the position's w
  (MTIR; 0 on the disc), and VU 4..7's w = DS+0x5cc + 0x10 k, which the set-up 0x2201a8 writes as twice the
  instance's four floats each frame (1.3-1.8 in the film, flickering). The meshes' own colour has alpha 0 (a
  neutral grey in the first batch, orange 0x43 0x16 0 with alpha 0x80 in the next, whose deltas lower it), so drawn
  as type 0 they added nothing. tools/tta_meshes.py now keeps the fourth array for these batches (colour mode 2:
  after the vertices, per vertex i16 delta[4] = the float less 8388608, u8 i, 3 pad). The runtime draws type 4
  after the game's render (which sets its ALPHA and the weights) with that colour (FTOI0, each field's low byte).
  In Eastern Moria's film (warp e01c04, frames ~16980-17400) the Balrog now arrives in fire: flames behind it, on
  its crown and over the screen in its blast. TTA_NO_TYPE4=1 turns it off. Other scenes unchanged (scripts/regress.sh,
  now with the film's frame 17250 as a twelfth scene, balrog).
- **Soak runs and the draw counters over every region** (2026-11-21). tta-headless from forest_14900, a warp to each
  of Western and Eastern Moria, the Gullies, Rohan, Helm's Deep, Osgiliath, Minas Tirith and the Pelennor at 15000,
  that region's debug battle at 16200 (Eastern Moria and Minas Tirith: the map's own start), CROSS every 45 frames
  to 21000 (tools/pads/pad_warp_battle.txt), run to 26000: all eight end at the frame limit, the game's frame
  counter moving in every 60-frame report, no unknown register access, no warning. With TTA_3D_TRACE=2 to 21000:
  no static node without a mesh, none not converted (besides the skins, which the skin path draws), no off-screen
  pass skipped. With the type-4 fire drawn, the chain audit's callers while exploring and in battle all have a
  native counterpart; what is left open needs a console reference (below, "Open").
- **Shadow volumes swept as on the console** (2026-11-20). Set 5's microcode (the overlay at 0x355780, read with a
  throw-away VU disassembler) answers the open extrusion: each vertex is stored twice, S v (object space; S = the
  blended bones, VU rows 4-7 set to identity by the set-up at 0x40) and C S v (clip, C = VU rows 0-3); the silhouette's
  far ends and the far cap are C S v + VU row 13, row 13 = C (x, y, z) times VU row 12 = the light in object space
  (block row 8), and the caps take a per-face factor from VU row 11 (.y = 0 near, .z = 1 far). In the first battle
  block row 8 is (1.99999, -2, 0.00623) and the character's W (rigid) has W^T (2, -2, 0) equal to it: the game carries
  the world light G+0x298 into object space, so in world space every vertex moves by the light vector itself,
  unnormalised (2.83 units for the default, ~1.5 x a character's height). The native sweep is now exactly that
  (TTA_SHADOW_REACH scales it, 1 by default) instead of 3 x the body's size; on flat ground the footprint is the same
  (the head's drop is under the sweep's 2), and where the ground falls away the shadow no longer runs on as a band.
  Block row 13 (VU row 14) is not the light: its .w (0.0327) is the skin's scale.
- **What the chain still carries: shader type 11, the lens flare** (2026-11-20). TTA_CHAIN_AUDIT=1 counts, every
  600 game frames, the callers of the chain append 0x2163c8 by return address and size: the game's producers of GS
  work, to be matched against the native hooks. Run on the first battle (battle1_5800 + build/pad_attack.txt), the
  forest and Rohan (build/chain/, local). Battle-only producers: the screen filters (0x1c9c08 .. 0x1d08b0, native),
  the movie (0x22b050) and the effect components (ears_rt_sfx; their type descriptors from 0x3782c0, 0x40 bytes:
  the name, then functions, the render at +0x38; with the audit on, each render is wrapped and the chain counted by
  component). Particle System (0x22efa0) is the Glows (types 9 / 20, native) and its modes 2 / 4 (+0xc3: 0x230768
  meshes through the type-0 render 0x2207a8, 0x2311f8 strips through the immediate primitive 0x218910, both native;
  their own chain blocks are state). Visual Object (0x23e4d8) and Screen (0x23fdb8) work through skins, textures,
  the screen filters and 0x218910 (native). LineTrail (0x23b988, the sword trails) sends its state and a matrix, then
  its strip through 0x218910 (native). 0x1d1a38 (from 0x1d19b8) sends only ALPHA / ZBUF / TEST for both contexts.
  Flare (0x232bd0) fills the type-11 list (below) and runs an occlusion test (0x1d2880, 0x2281e8, below). And 0x21e6b8 with
  0x21e270 under it: shader type 11's effect render (the type table 0x35f690, 0x44 bytes a type: type 11 is
  0x21e5e8 / 0x21e638 at +0, +4 and 0x21e6a8 / 0x21e6b0 / 0x21e6b8 / 0x21e800 at +0x34 .. +0x40, as the Glows'
  0x21f980 is at +0x3c of types 9 / 20), which nothing drew natively. 0x21e6b8 selects VU program 0x13 (DS+0x6a8,
  DS+0x730 |= 0x10000: the flush 0x2133d0 sends STCYCL, FLUSHA, a matrix to VU 4..7 and MSCAL 0) and calls
  0x21e270(list, instance + 8 == 0, ..). Microcode set 2 (the MPG at 0x350690; the game's seven sets start at
  0x3492e0, 0x34d1b0, 0x350690, 0x352100, 0x355780, 0x3578c0, 0x358a80): its program 0 loads vf1-vf8 from VU 0..7
  and stops; the strips continue at 0x48 (MSCNT). The list: up to 160 entries of 0x30 bytes, count at +0x1e00;
  +0 a world position, +0x10 / +0x14 two RGBA colours, +0x18 / +0x1c two radii, +0x20 an angle, +0x2c the layer (the
  call's second argument), +0x2d the shape. VU0 vf1-vf4 hold the camera's world -> view matrix; a centre behind the
  camera is skipped. Per entry: five qwords (the view-space centre, (cos, sin) and (-sin, cos), the radii, the
  colours as V4-8) and a precompiled strip from the table 0x403718 {address, qwords} (13 shapes, unit circles of 5
  to 32 segments: a GIF tag PRIM 0x4c, a Gouraud blended triangle strip, then (x, y, 0, flags) per vertex; flag bit 0
  takes radius / colour 0, else 1; bit 1, on the first two, draws nothing). The program places each vertex at
  centre + radius (x cos - y sin, x sin + y cos) in the view plane, projects it, and sends the colour with the
  scene's ALPHA_1 (0x48, Cs As + Cd) and TEST_1 (layer 1: Z ALWAYS; layer 0: GEQUAL). Shape bytes with bit 7 draw up
  to three rings (a disc 0.5 k r; a ring r .. 1.5 r on strip 3 + k; a thin ring at 0.4 k r, 1/64 wide each side). In
  the first battle it is the Nazgul Scream's lens flare: 39 discs strung from the light on the Ringwraith's blade
  through the screen's middle, alpha rising from 0 to ~14 over ~450 frames (host 6195 .. 6660), 2432 triangles.
  runtime/tta_3d.cpp's rings builds the same strips in view space, takes them back to the world with the inverse of
  vf1-vf4 and draws them through RC+0x1a0. The frame is 10-27 levels brighter on average while it plays (with the
  Scream's green filter over it). TTA_NO_RINGS=1 turns it off; TTA_SPRITE_PROBE=1 prints the lists. No console
  reference of the Scream yet. The flare's strength is its visibility: 0x1d3f28 puts the light's place on screen
  (0..1, R+0xc8 / +0xcc) and GS Z (+0xd0) in its record R = *(0x36b228) + i * 0x100 + 0x3c0; 0x2281e8 copies the Z
  buffer's 8 x 5 pixels around it (x - 4 .. x + 3, y - 2 .. y + 2, moved inside the screen; +0xa4 .. +0xb0 the part
  that is the light's window) to a small GS place; the frame end's 0x228118(i) reads them back (PSMZ32 StoreImage)
  into R +0 .. +0x9f; next frame 0x228040 sums the weights 0x397c78 of the pixels with Z & 0xffffff < the light's Z +
  16 (nothing in front). The readback did nothing natively, so every flare counted as fully visible. It now reads
  the window's depth (BozoEngine's depthAt, the frame presented last; TTA's GS Z is the matrices' z / w): in the
  Scream the block is read whole and sometimes covered (all 40 pixels in front), and the flare fades then (mean
  alpha 14 -> 9.6). Pixels not read back yet (headless) count as clear. TTA_FLARE_NO_DEPTH=1 for the old way;
  TTA_RENDER_TRACE=1 prints each test.
  The same audit over a battle in each later region (the debug warp from forest_14900 at 15000, an encounter at 16200,
  CROSS every 45 frames to 21000: Western Moria's Watcher 1000, the Gullies' 1132, Helm's Deep's breach 1129,
  Osgiliath's Gothmog 1269, Minas Tirith's court 1448, the Pelennor's 1252; Rohan's 1172 above) finds the effect
  components Particle System, Flare, LightBeam (0x236218: its strips through 0x218910), Visual Object and Screen,
  and only six callers not seen before, all GS state (A+D ALPHA / ZBUF / TEST / FRAME / TEX0) for draws made through
  native paths: 0x1cfbcc, 0x1ce02c (screen filters), 0x1d0c5c / 0x1d10b8 (0x1d0c40, Osgiliath, into the 256-wide
  target FBP 0x1e0), 0x2202b8 (type 4's set-up) and 0x23f2dc (0x23e7e0, Visual Object). TTA_CHAIN_DUMP=<ra hex>
  prints a caller's first blocks. Lightning, Glow (the component), Weather and Line Blur did not run in these.
  Captures of those battles look right except Helm's Deep (encounters 1129 Breech, 1126 Deeping Wall, 1133 Random 01,
  started from the valley's daytime start): mounted riders over black or a cloudy sky, no ground. Open, likely the
  debug start: the game itself draws only four type-0 nodes there (ter 104 pieces, its chain shows no terrain render),
  placed by RC+0x2e0 = viewport x RC+0x260 x the identity object matrix (TTA_3D_DUMP_FRAME now prints that check),
  mostly off-screen: the battle's ground is not among the nodes the game renders, as if the area the encounter places
  the battle in were not streamed in. The battle was started inside the map's opening films (below, "Helm's Deep's
  debug battles: no ground, mounted party").
- **Test runs show nothing** (2026-11-18): a scripted window run (--pad-script) now opens no window: BozoEngine's
  WindowOptions::hidden / captureScene present into an offscreen picture and capture from it (a covered or hidden
  window's back buffer read back black: captures depended on what was on top of the desktop). Same pixels as a
  visible window's capture, HUD included. TTA_SHOW_WINDOW=1 shows it.
- **The sunny battle's frame rate** (2026-11-18, the user: unstable frames and crackling sound). Not a regression:
  the build of 2026-11-13 (8f23a45, engine 725fa5d) takes 83-90 s headless from boot through the first battle, today's
  60 s. The user's session overlapped other ports' builds and tests on the same PC (the intro fell to 30 frames a
  second; alone it runs at 60), and the stalls emptied the sound queue (the crackles). The sunny battle itself is
  heavy: ~19-21 ms of game-thread work a frame headless (50-55 frames a second in the window): every character is
  ~8 skinned nodes (body, head, weapons), ~1470 batches skinned on the CPU a frame, all on screen (a cull test kept
  them all); the game's VU0 code (exact float rounding) is ~15%. A per-frame cache of skinned batches hit ~1% (the
  layer passes 02-04 mostly draw no batch) and was dropped; kept: drawSkin's buffers reused, its bone matrices in a
  flat table instead of a hash map, TTA_SKIN_NOLIGHT read once (~10% headless). TTA_HOST_PROFILE now lists 400
  addresses a thread.
- **The sunny battle's glow filter reads the frame's alpha** (2026-11-18, from the user's state, F5 in a battle:
  copied to build/states/user_battle.state, frame 9329; window states are in %APPDATA%\TLOTR_TTA_Recomp\states).
  0x1cf1fc and 0x1cfce8 read the frame (TBP0 0) as PSM CT32 with TCC 1, MODULATE: As is the frame's own alpha, and
  their targets (TBP0 0x3800 / 0x3c00) are CT32 TCC 1 too. The first battle's filters read the frame as CT24 (PSM 1),
  where the alpha is TEXA's: they do not depend on it. On the console the scene's draws leave the frame's alpha
  alone (FBMSK 0xff000000) and the highlight pass 0x80000 writes it alone (FBMSK 0x00ffffff), so this filter is a
  coloured glow around the highlighted and buffed characters (fill 0x6c797f x alpha, blurred, darkened inside by the
  alpha four times, added back). Natively the frame's alpha is not that mask (every draw writes alpha, the
  highlight is skipped) and the frame is read without its alpha: since the blend fix the filter adds almost nothing
  (no haze, no glow). Open: emulate the alpha through the chain (frame, targets, taps); no console reference yet.
  The post-draw trace now prints TEX0.
- **The white battle ground** (2026-11-13, F7 in the user's sunny battle): that battle runs another screen filter
  (0x1cbafc fills target 1, 0x1cf1fc multiplies it by the frame's alpha with ALPHA 0x89 = Cd As, 0x1cf8a0 blurs it
  through target 2 with five additive taps, 0x1cfce8 darkens it four times with ALPHA 0x46 = Cd (1 - As), alpha 0x54,
  and 0x1d0540 adds it back over the frame). 0x89 and 0x46 have no name in blendOf, which fell back to
  (Cs - Cd) As + Cd: the copy stayed as bright as the picture and the add-back doubled it. The 2D draws now take the
  GS's own (A - B) C + D for the forms without a name (TTA_NO_GS_BLEND_2D=1 for the old fallback); the first
  battle's capture is identical either way.
- **The white party in battle** (2026-11-12, reported after Company Might and on the selected character): in battle
  every character's 0x80000 pass (the flat FOGCOL copy, fade pulsing) runs with FRAME_1 FBMSK 0x00ffffff: colour
  masked, only the frame's alpha written, a mask for a later pass. Natively it was drawn in colour, and in the sunny
  forest FOGCOL is 0xffffff (ALPHA 0x48 = Cs As + Cd): white figures. The pass now takes the frame's write mask and is
  skipped when no colour channel is written (TTA_SKIN_SILHOUETTE_NO_FBMSK=1 for the old drawing). Found with F7 in
  the user's battle. TTA_RNG_TRACE=1 counts the game's generator (0x1ba9c8) and traces newlib rand by caller: while
  walking the forest only the particle systems (0x22bb..0x22d4) roll, so the random encounters roll elsewhere.
- **The shine layer and F7** (2026-11-12, open: Company Might's white party). Layer "02" (G+0x2b0 = 0x20000, 0x2265xx)
  draws only when inst+0x10 != 0: render target 0 with all channels (0x2132c8 flags 1: FBMSK 0; 2 = everything
  masked, 4 = alpha only, 8 = colour only, the scene's state), pass 1 with template 0x377b10 (ALPHA 0x2a = Cs, TEST
  ZTE GEQUAL, no alpha test: the character's own alpha into the frame's alpha), then the lights G+0xa0.. x 0x377a84 +
  0x377a88 x the colour 0x377a90 (10 terms, on the stack, not in G) and pass 2 with 0x377b80 (ALPHA 0x58 = Cs Ad + Cd:
  additive, weighted by that alpha; CLAMP 5) on the second texture *(inst+0x14), back to flags 8. The native skin
  draws that layer as a plain copy. In the first battle it is empty (no sections, G+0xa0 zero, scales 1 and 0).
  F7 in the window build (TTA_REPORT_AT=<game frame> from a script) writes the RAM to build/ram_key_<frame>.bin and
  logs one frame of [tta:report] lines: every character draw's target, FBMSK, mode, fade, sections, lights and the
  shine inputs.
- **Saving** (2026-11-12): a save at a save point failed. TTA's libmc (0x27c838..0x27d8c8) has no mkdir RPC (its
  commands: 0xfe init, 1 getinfo, 2 open, 3 close, 4 seek, 5 read, 6 write, 0xa flush, 0xc chdir, 0xd getdir, 0xf
  delete, 0x10 format): its sceMcMkdir (0x27cb58, unnamed in the map) is sceMcOpen(port, slot, name, 0x40) and, on 0,
  sets the pending command 0x37a610 to 0xb for sceMcSync. BozoEngine answers sceMcOpen natively, where 0x40 is no
  create flag: a missing entry, -4, and the save folder was never made. 0x27cb58 is now named sceMcMkdir
  (recomp/tta_symbols_reviewed.txt), answered by the native mkdir. The cards persist only with --card-dir DIR
  (without it they are fresh formatted cards in memory); BOZO_TRACE_MC=1 traces every card call.
  A played game (the window build, no pad script) now defaults to %APPDATA%\TheBozos\The Third Age\cards
  (SDL_GetPrefPath): a save launched without --card-dir had lived only in memory and was gone at exit. Headless and
  scripted runs keep in-memory cards; TTA_MEMORY_CARDS=1 does the same for a played game.
- **Battle frame dips** (2026-11-11). Alone on the bench (scripts/bench_run.sh --alone), the window build ran the
  battle's turn and the Nazgul Scream at 20-26 ms a picture in places, smooth with TTA_NO_GLOWS. TTA_HOST_PROFILE
  (resolve offsets with C:/msys64/ucrt64/bin/nm.exe -C on build/bozo/tta.exe, image base 0x140000000) put the game
  thread's time under uploadCanvas (a fresh 512 x 512 RGBA texture each time the game re-sends the UI canvas, mostly
  unchanged) and in tta::env (a std::string built per call on the draw paths). The canvas now keeps its texture when
  its pixels are the same, and env keys its cache by the literal's address: the worst second is 18 ms (was 26),
  the scream 16.8 ms, pictures identical (deterministic captures compared pixel for pixel).
- **The screen filters' bloom, at the console's strength** (2026-11-11). With the mist fixed, the intro close-up was
  darker and flatter than the console's (mean 47 / 46 / 40 vs 61 / 58 / 50; the earlier "match" came from the grey
  canvas veils). The console's chain in this scene (PCSX2 GS dump): clear the 256 x 256 target FBP 0x1c0; the frame
  added into it five times at alpha 5-7 with a small ST jitter (a blur, (Cs - 0) As + Cd); downsampled into its own
  corner at 128 then 64; the 64 x 64 image added over the frame full screen at vertex alpha 0xff (As ~ 2); the
  previous frame blended on top with the scene's tint (e.g. 72 / a5 / 7f at 0x3a). Three things were lost natively:
  (1) the GPU stops alpha at 1, so the bloom came back at half strength: an additive post draw past 0x80 is drawn
  again with the rest (TTA_NO_DOUBLE_ALPHA); (2) the GS blends in integers and truncates, floor(floor(Ct Cv / 128) As /
  128), so in the faint passes a dark texel adds nothing: before the target is read back its passes' loss (half a level
  each) is taken off at the target's first read-back (its downsample into its own corner) by the shader's levels
  (rgb - low, clamped at 0; TTA_BLOOM_FLOOR=n levels, TTA_NO_BLOOM_FLOOR off): shadows 23 -> 17 against the
  console's 8-14. A separate (Cd - Cs) draw before it did the same but about one capture in twelve came out all
  black (0 in 6 with a per-frame glReadPixels probe syncing the GPU, so a timing interaction in the renderer, not a
  wrong draw list; one GL context, the depth PBOs unbound, TTA's batches unkeyed); the levels version adds no draw
  and gave 0 black in 8. (3) the GS samples a
  pixel at its integer position, the GPU at its centre: the filter draws move by half a GS pixel (TTA_NO_HALF_PIXEL),
  else the chain drifts up-left and the bloom lands ~7 pixels off (a halo up-left of faces). TTA_2D_TRACE=2 prints
  each post draw's rectangle and ST.
- **The skins' vertex colours** (2026-11-11). The skin program 0x378 (set 8df1e059) lights each vertex's colour, not
  a constant: at 0x5c0..0x668 it loads the vertex's third qword (lq -4(vi2), one vertex behind the transform), ITOF0,
  multiplies by the order-2 light, clamps to 255, FTOI0. tools/tta_meshes.py had taken that qword for VU scratch and
  written 0x80808080; it holds baked shading (450 distinct values in E01C01: greys 0x45 / 0x33, greens 0x4a5a38,
  ...). The converter now writes it. The intro's Black Riders shot: shadows 17 -> 13 (console 8). The remaining gap
  there is large-scale (console std 28 against our 14 at 16-px scale; fine detail matches): a bright background, darker
  characters. Fog is not it: FOGCOL 524842 and K (2.55e6 in the scene, 31875 from the front end 0x1c3158 once a
  frame) match, our scenery never gets past w ~2700 (F = 255), and on the console the canopy's vertices are F 255
  too: its F = 0 vertices all have Q < 0 (behind the camera) and ADC set, so no drawn strip triangle uses them (a
  GS dump reader that carries F over between draws makes them look fogged). The static program 0x2418 computes
  F = min(K / clip.w, 255) by overwriting clip.w with K after the DIV has read it (addx.w vf23, vf30, vf0x).
  TTA_SKIN_STATS=<game frame> prints each skin batch's mean lit / vertex colour.
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

On BozoEngine (reviewed 2026-11-21; the PS2Recomp-era items, the GS rasterizer, the VU1 AOT, the fork branches,
sceGsExecLoadImage, the garbled loading text, have no counterpart there: the loading screen is drawn natively, and
BozoEngine's libmpeg keeps one stream callback a type, so the sceMpeg callback leak JC2 found cannot happen):
- The screen filter's frame alpha (above, "The glow filter"): the console's scene leaves the frame's alpha alone and
  the highlight pass writes it alone; natively every draw writes alpha, so the coloured glow around highlighted and
  buffed characters is missing. No console capture of it yet.
- The shine layer "02" (above, "The shine layer and F7"): no case seen to check it against.
- Helm's Deep's battle start still dips to ~56 frames a second for two seconds (above, "Frame times in the later
  regions"; was ~10 s at 49-57 before BozoEngine#112).
- The Linux and macOS builds (WSL here has no compiler; installing one needs the user's sudo).
