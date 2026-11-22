<img src="docs/branding/tta_banner.png" alt="The Lord of the Rings: The Third Age, native PC port by The Bozos" width="100%">

<div align="center">

# TLOTR_TTA_Recomp

**A native Windows, Linux and macOS port of *The Lord of the Rings: The Third Age* (NTSC-U, `SLUS-21027`), made by statically recompiling the game's PlayStation 2 code to C++.**<br>
Built on [BozoEngine](https://github.com/TheBozos/BozoEngine), The Bozos' own toolchain: the game's EE code recompiled to C++, every library and kernel call it makes answered by native code. No PS2 hardware model at any level: no GS, VU, VIF/GIF, DMA, IOP or SPU2.

![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-1b8cff)
![Compiler](https://img.shields.io/badge/compiler-GCC%20%7C%20Clang-1b8cff)
![Based on](https://img.shields.io/badge/based%20on-BozoEngine-17a35b)
![Status](https://img.shields.io/badge/status-first%20battle%20native%20at%2060%20fps-17a35b)

</div>

> [!IMPORTANT]
> No game files are included: no ELF, IRX modules, data archives, or code generated from them.
> You need **your own copy** of the North American release, PS2 ID **`SLUS-21027`**. The port is built against that disc's executable:
>
> | File | SHA-1 |
> |---|---|
> | `SLUS_210.27` | `d88ded370592114169fa18f0ff73b9f610a94bd0` |

## Status

**On BozoEngine (since 2026-11-09): the game plays natively in a window.** On the PS2Recomp fork the game
played from the boot to the first battle (the older screenshots below), but its graphics went through a PS2 emulation
layer (VU1 sandbox, GS state machine, GS memory). BozoEngine has none of it, so the picture comes back as native draw
hooks are written from the game's own data: the frame and the movies first (2026-11-10), then the 2D, the text and
the 3D. Windows comes first, then Linux and macOS.

| Milestone on BozoEngine | |
|---|:---:|
| Symbol map of the stripped ELF (BozoEngine's matcher against reference executables, then names read from the code) | ✅ |
| Recompile with `bozo-recomp`, a headless runner on the engine's free host | ✅ |
| SDK init, SIF, CD/DVD, memory card, IOP reboot and module loads answered natively | ✅ |
| EA's sound driver (AUDIOPF.IRX) and LIBSD statically compiled from the disc, its SIF handshake with the game | ✅ |
| Pads through a native libpad2 (BozoEngine PR #81) | ✅ |
| VU0 microprograms translated to C++ from the ELF; movies through a native libmpeg (BozoEngine PR #82) | ✅ |
| Native frame: the chain built but never sent, the swap publishing to the engine's renderer; the movies drawn | ✅ |
| Textures converted offline from the disc (tools/tta_textures.py); the 2D of LLPrims drawn natively: the loading screen | ✅ |
| The UI (EA Apt: title, menus, battle HUD) and its text drawn natively; the boot reaches the first battle | ✅ |
| 3D: terrain, trees, objects and animated characters from meshes converted offline, the game's matrices and skeletons | ✅ |
| Effects, water, render targets (blur, glows), the battle menu's details | 🔧 |
| Window, sound output, pad from the PC, save states (F5 / F7) | ✅ |
| The game playable again: the first battle, the forest, Eregion and on (played in the window, saves on disk) | ✅ |
| Linux and macOS builds | ⏳ |

The engine changes TTA needs are merged into BozoEngine's `master`, which the submodule follows.

✅ works · 🔧 in progress · ⏳ not reached yet

[docs/RECOMP_NOTES.md](docs/RECOMP_NOTES.md) records how the port is brought up, every fix and how it was found, and
what remains open.

## Screenshots

**2026-11-22.** Berethor's sword slash in the first battle, now drawn in the 3D scene: depth-tested at its place and
taken by the screen filters' glow, as the console sends it, where it used to lie flat over the finished picture with
the HUD.

<p align="center">
  <img src="docs/screenshots/2026-11-22-bozo-sword-slash.png" width="400" alt="Berethor's sword slash, a bright blue streak across the screen, beside a Nazgul in the first battle on BozoEngine">
</p>

**2026-11-21.** Widescreen (`--widescreen`): the forest after the first battle at 16:9, the trees and the path a
third wider on each side than the console shows, the minimap keeping its shape. The game's own matrices are squeezed
after their projection, so nothing of the game is patched; the game draws what lies just outside its 4:3 view.

<p align="center">
  <img src="docs/screenshots/2026-11-21-bozo-widescreen-forest.png" width="560" alt="Berethor in the forest in 16:9 widescreen on BozoEngine, trees on both sides and the minimap at the top right">
</p>

**2026-11-21.** The Balrog in Eastern Moria's in-engine film, its fire drawn at last: shader type 4, a sheet of flames
(Cobj 19-21) whose colour the game animates through a fourth vertex array and a flickering weight per frame. The
port drew it with no alpha before, so the Balrog arrived without fire. Found by surveying what the game puts in its GS
chain while exploring each region.

<p align="center">
  <img src="docs/screenshots/2026-11-21-bozo-balrog-fire.png" width="400" alt="The Balrog bursting out of the rock in Eastern Moria on BozoEngine, flames behind it">
</p>

**2026-11-21.** Helm's Deep's Deeping Wall battle, started from the debug tool: Berethor, Legolas, Idrial and Morwen
on foot on the wall's walkway, among the stakes. Asked for during the map's opening films, these battles used to start
inside them: the film's riders on their horses, its camera and its valley in half the shots, no ground under the
rest. The debug tool now waits for the films to end.

<p align="center">
  <img src="docs/screenshots/2026-11-21-bozo-deeping-wall.png" width="400" alt="A battle on the Deeping Wall at night on BozoEngine, the party facing Uruk-hai berserkers on the stone walkway">
</p>

**2026-11-20.** The first battle's Nazgul Scream with its lens flare: the discs strung from the light on the
Ringwraith's blade through the middle of the screen (shader type 11, an effect render found missing by a survey of what
the game puts in its GS chain), built from the same unit-circle strips as set 2's VU1 program.

<p align="center">
  <img src="docs/screenshots/2026-11-20-bozo-scream-flare.png" width="400" alt="The Nazgul Scream in the first battle on BozoEngine, with the lens flare's discs over the green veil">
</p>

**2026-11-20.** Rohan, reached through the debug warp: the river's flowing surface (shader type 5, the only static
scenery that was still missing in the later regions) drawn over its sky reflection, its texture drifting with the
current as set 1's VU1 program animates it.

<p align="center">
  <img src="docs/screenshots/2026-11-20-bozo-rohan-river.png" width="400" alt="A Rider of Rohan on horseback beside the river in Rohan on BozoEngine">
</p>

**2026-11-12, early.** Past the first battle, all native: Berethor falls to the Black Riders as scripted, Idrial's
light drives them off, she heals him, and the game hands over the controls in the forest (minimap, tutorial box)
before Gandalf's first Epic Scene film. Boot to here with nothing but CROSS presses.

<p align="center">
  <img src="docs/screenshots/2026-11-11-bozo-idrial-light.png" width="400" alt="Idrial's light flower over the battle on BozoEngine">
  <img src="docs/screenshots/2026-11-11-bozo-after-battle-explore.png" width="400" alt="Berethor exploring the forest after the first battle, with the minimap and a tutorial box">
</p>

**2026-11-11, late night.** The battle menu as on the console: the selection ring glows (it blends with the
game's additive mode in force, not over a black square), the white sheet at the left is gone (a water layer had
picked up a UI texture from the shared VRAM slot), the waterfall falls bright again (its particles had been hiding
each other: the game draws them without writing depth), and the hint reads "Attack a single target." once the
cursor moves, which is when the game sends it.

<p align="center">
  <img src="docs/screenshots/2026-11-11-bozo-battle-menu.png" width="400" alt="Berethor's battle menu on BozoEngine with the glowing selection ring and the attack hint">
</p>

**2026-11-11, night.** The first battle opens on Berethor's turn again, as on the PS2: the game's initiative jitter
draws on its own C library rand, which the engine had been answering with another generator.

<p align="center">
  <img src="docs/screenshots/2026-11-11-bozo-berethor-first.png" width="400" alt="The first battle on BozoEngine with Berethor's command menu and the turn queue">
</p>

**2026-11-11, evening.** The screen filters' bloom at the console's strength. The game adds its blurred frame back
at GS alpha 0xff (twice the image), which a GPU stops at 1, so ours was half as bright; the blur chain also drifted
half a pixel per pass and left a halo up-left of faces. The Berethor close-up now averages 63 / 62 / 54 against the
console's 61 / 58 / 50.

<p align="center">
  <img src="docs/screenshots/2026-11-11-bozo-bloom-closeup.png" width="400" alt="Berethor in the forest on BozoEngine with the console's soft bloom">
  <img src="docs/screenshots/2026-11-11-bozo-bloom-forest.png" width="400" alt="A Black Rider raising his sword in the green, misty forest on BozoEngine">
</p>

**2026-11-11, later.** The mist's real textures: the glows had been drawn with the UI's text canvas (it shares the
mist texture's place in GS memory), which washed the intro out in green. Each effect now takes its texture from its
own record, so the forest's mist is soft and wispy.

<p align="center">
  <img src="docs/screenshots/2026-11-11-bozo-forest-mist.png" width="400" alt="A Black Rider in the forest on BozoEngine, the trees fading into green mist behind him">
</p>

**2026-11-11.** TTA's Glows, native: the effects' particle billboards (VU1 microcode 0x620 rewritten in C++). They
are the console's mist veils in the forest and the haze and light shafts over the battle.

<p align="center">
  <img src="docs/screenshots/2026-11-11-bozo-glow-mist.png" width="400" alt="Berethor in the forest on BozoEngine, veiled in the soft mist of TTA's glow particles">
  <img src="docs/screenshots/2026-11-11-bozo-battle-haze.png" width="400" alt="The first battle on BozoEngine with the glow particles' haze and a light shaft between the Black Riders">
</p>

**2026-11-10, evening.** The console's look: the screen filters (the frame blurred through an off-screen target and
graded back) and the forest's mist, native; shadows at the game's strength.

<p align="center">
  <img src="docs/screenshots/2026-11-10-bozo-forest-haze.png" width="400" alt="Berethor riding through the misty green forest on BozoEngine, with the game's screen filters drawn natively">
</p>

**2026-11-10, later.** The battle's ground: the bridge and the forest floor under the Black Riders (scene depth writes
and near-plane clipping fixed).

<p align="center">
  <img src="docs/screenshots/2026-11-10-bozo-battle-ground.png" width="400" alt="The first battle on BozoEngine: the two Black Riders standing on the stone bridge, Berethor down after a critical hit">
</p>

<p align="center">
  <img src="docs/screenshots/2026-11-10-bozo-exploration.png" width="400" alt="Exploring after the first battle on BozoEngine: Berethor in the forest, the round minimap and the Epic Scene dialog">
</p>

**2026-11-10.** Past the first battle: exploring the forest, the HUD's minimap and dialogs (EA Apt's depth masks
done natively).

<p align="center">
  <img src="docs/screenshots/2026-11-10-bozo-nazgul-scream.png" width="400" alt="The first battle on BozoEngine: a Black Rider casting Nazgul Scream, the battle HUD over it">
  <img src="docs/screenshots/2026-11-10-bozo-first-battle.png" width="400" alt="The first battle on BozoEngine: a critical hit, Berethor down and the two Black Riders over him">
</p>

**2026-11-10.** The first battle again, all native, at 60 frames/s: the Black Riders' Nazgul Scream and Wraith
attacks, Berethor falling to the scripted critical hit.

<p align="center">
  <img src="docs/screenshots/2026-11-10-bozo-berethor-riding.png" width="400" alt="Berethor riding his horse through the dead forest, drawn natively on BozoEngine">
  <img src="docs/screenshots/2026-11-10-bozo-black-riders.png" width="400" alt="Two Black Riders confronting Berethor in the forest, drawn natively on BozoEngine">
</p>

**2026-11-10.** The intro cinematic in 3D again, with no PS2 hardware model: the forest, the sky and the characters
from meshes converted offline from the disc, skinned on the CPU from the game's own animation, with its camera.

<p align="center">
  <img src="docs/screenshots/2026-11-10-bozo-title-screen.png" width="400" alt="The title screen in the BozoEngine window: Minas Tirith, the logo and New Game / Load Game / More, drawn natively">
  <img src="docs/screenshots/2026-11-10-bozo-first-battle-hud.png" width="400" alt="The first battle's HUD in the BozoEngine window: Berethor 177 HP, 65 AP, and the Black Rider's Wraith Thrust; the 3D is not drawn yet">
</p>

**2026-11-10.** The title screen (EA Apt's background, logo and menu, its text rasterised by the game's own fonts)
and the first battle reached again: its HUD is native, the 3D (forest, Berethor, the Black Riders) comes next.

<p align="center">
  <img src="docs/screenshots/2026-11-10-bozo-loading-screen.png" width="400" alt="The loading screen in the BozoEngine window: the One Ring's inscription and an Elvish line, drawn natively">
</p>

**2026-11-10.** The loading screen drawn natively: the game's own primitive calls and draw state, with its
textures converted once from the disc and found by the game's texture records. No GS packet is read.

<p align="center">
  <img src="docs/screenshots/2026-11-10-bozo-ea-games-movie.png" width="400" alt="The EA GAMES logo movie in the BozoEngine window">
  <img src="docs/screenshots/2026-11-10-bozo-thx-movie.png" width="400" alt="The THX Certified Game logo movie in the BozoEngine window">
</p>

**2026-11-10.** The first pictures on BozoEngine: the intro movies, decoded by the engine's native libmpeg and shown
by its renderer, at 60 frames/s. The game builds its frame's DMA chain as before; nothing of it is sent anywhere.

From the PS2Recomp build, until 2026-11-09.

<p align="center">
  <img src="docs/screenshots/2026-10-29-native-vu1-black-riders.png" width="400" alt="Berethor facing three Black Riders in the forest cinematic, drawn without the emulated VU1">
</p>

**2026-10-29.** The Black Riders' cinematic with the VU1 sandbox (now the default): no VU1 program runs on the
emulated VU1 unit, the native adapter runs each one in a sandbox and hands what it draws to the OpenGL renderer in GS
order, replaying each packet's GS register writes. That is still emulation (VU1 microcode, GS state machine), see
above. The first battle, its scene unlock and the film clip after it match the VU1 path, at the same
frame rates (19.5 in battle).

<p align="center">
  <img src="docs/screenshots/2026-10-29-black-rider.png" width="400" alt="A Black Rider raising its sword in a misty forest, in-engine">
  <img src="docs/screenshots/2026-10-29-scene-unlocked.png" width="400" alt="The Scenes menu after the first battle: Nazgul Agenda, scene 2 of 109, unlocked">
</p>

**2026-10-29.** The first battle played to its end with the Cross button alone (a pad script): the Black Riders'
cinematic, the scene it unlocks (Nazgul Agenda, 2/109) and the film clip that follows, in one 11-minute run.

<p align="center">
  <img src="docs/screenshots/2026-10-29-first-battle.png" width="400" alt="The first battle: Berethor facing two Black Riders, with the Attack / Taunt Foe / Skip menu, turn portraits and HP">
</p>

**2026-10-29.** The first battle (Berethor against two Black Riders), reached with the defaults only: the native
OpenGL renderer, the IOP modules recompiled, the game on the PC's clock. What crashed the intro cinematic in most runs
was CD reads completing at once; they now take drive-like time.

<p align="center">
  <img src="docs/screenshots/2026-10-29-in-engine-forest.png" width="400" alt="An in-engine scene: a dark horse in a misty forest, drawn by the native OpenGL renderer">
</p>

**2026-10-29.** After New Game: the in-engine intro cinematic, the game's own 3D drawn by the native OpenGL renderer.

<p align="center">
  <img src="docs/screenshots/2026-10-28-title-screen.png" width="400" alt="The title screen: The Lord of the Rings: The Third Age logo over a dark blue background, with New Game, Load Game and More">
  <img src="docs/screenshots/2026-10-28-attract-movie.png" width="400" alt="The attract movie: two characters on a forest path at dusk">
</p>

**2026-10-28.** The title screen, after the EA GAMES and New Line Cinema movies; the attract movie plays and returns
to it. The movies end the way the game signals it (its no-data callback returns 0), fork `tlotr/mpeg-nodata-end`.

<p align="center">
  <img src="docs/screenshots/2026-10-27-loading-screen.png" width="400" alt="The loading screen: the One Ring with its glowing inscription and an Elvish-style line of text">
  <img src="docs/screenshots/2026-10-27-memory-card-check.png" width="400" alt="A message box: Checking memory card (PS2) in MEMORY CARD slot 1">
  <img src="docs/screenshots/2026-10-27-legal-screen.png" width="400" alt="The Lord of the Rings: The Third Age logo above the copyright notice">
  <img src="docs/screenshots/2026-10-27-ea-games-movie.png" width="400" alt="The EA GAMES Challenge Everything boot movie">
</p>

**2026-10-27.** The loading screen, the memory card check, the legal screen and the first boot movie, drawn by the
recompiled code. The disc is read
through the game's own IOP sound and streaming module, and the VU1 microprograms run AOT-compiled (with exact
MAC/status flags for now). The white band on the right is outside the 516-pixel-wide area the game draws in this
build.

<details>
<summary>2026-10-26: first frames</summary>

<p align="center">
  <img src="docs/screenshots/2026-10-26-loading-screen.png" width="400" alt="The loading screen with a garbled line of text">
</p>

The ring of the loading screen turned, but its text was garbled and the game never read the disc.
</details>

## Playing

`build/bozo/tta.exe` (after the build below; it needs your disc image, see "Build and run").

| PS2 pad | Keyboard (default) |
|---|---|
| D-pad / left stick | Arrow keys or W A S D |
| Right stick | I J K L |
| ✕ (confirm) | X or Space |
| ○ (back) | C or Escape |
| □ / △ | Z / V |
| L1 / R1 | Q / E |
| L2 / R2 | Left Shift / Right Shift |
| L3 / R3 | F / G |
| START / SELECT | Enter / Tab |

A gamepad (Xbox, PlayStation, Switch Pro ...) works as a PS2 pad when plugged in. Keys and buttons are changed in the
Controls tab of the settings window. The mouse pointer hides over the game after two seconds without moving
(`PS2X_KEEP_CURSOR=1` keeps it).

| Key | |
|---|---|
| F2 | The settings window: Video (window size, full screen, render scale, texture upscaling and smoothing, frame rate), Game (widescreen, skippable logos, volume), Controls, Save states, Debug (warps and battles) |
| F3 | Frame rate counter |
| F11 or Alt+Enter | Window / full screen |
| F5 / F7 | Save / load a save state; F6 and Shift+F6 pick the slot (0-9) |
| F8 | Capture a sequence of pictures |
| F10 | A bug report of this moment (the RAM and one frame of draw state) |

The game's own saves (its two memory cards) are in `%APPDATA%\TheBozos\The Third Age\cards`; the settings
(`video.ini`, `input.ini`, `tta.ini`) in `%APPDATA%\TLOTR_TTA_Recomp`.

## Goal

A fully playable *The Third Age* as a native executable on Windows, Linux and macOS, from the intro to the end of the
campaign. Accuracy first; once it is playable, widescreen and higher internal resolution.

## How it works

```
disc (your ISO) ──► scripts/extract_iso.sh ──► game/ (SLUS_210.27, MODULES/*.IRX)
                                   │
 recomp/tta_symbols.txt (names for the stripped ELF: scripts/symbols.sh) + runtime/ overrides
                                   │
        bozo-externals ──► the externals list   bozo-recomp ──► C++ (build/bozo/tta_gen/)
        tools/tta_bozo_audio.py ──► AUDIOPF.IRX and LIBSD.IRX as C++ (BozoEngine tools/native_irx)
                                   │
              CMakeLists.txt ──► BozoEngine runtime + generated code + runtime/ ──► tta-headless
```

- The retail executable is stripped. `recomp/tta_symbols.txt` names its library and kernel functions:
  BozoEngine's `bozo_symbolmap.py` matches them against executables that keep their symbols, and
  `recomp/tta_symbols_reviewed.txt` adds the ones read from the code, each with its evidence
  (`tools/tta_rpc_names.py` pairs SIF RPC clients with their server and function number to help).
  Every named function BozoEngine knows is answered natively.
- `runtime/tta_port.cpp` registers the port's overrides by address; `runtime/tta_audio.cpp` runs EA's sound driver
  in the engine's compiled-library host and connects it to the game (SIF commands, software registers).
- Nothing derived from the disc is committed: the generated code and the compiled IRX are in `build/`.

## Requirements

- Windows 10/11 with [MSYS2](https://www.msys2.org/) (UCRT64), Linux, or macOS
- CMake 3.21+, Ninja, GCC 13+ or Clang with C++20
- GNU make and curl (the movies' minimal FFmpeg is built from its source once: scripts/build_ffmpeg.sh); or FFmpeg
  development libraries (`libavcodec`, `libavutil`) through `pkg-config` with `TTA_SYSTEM_FFMPEG=1`
- `bsdtar` (libarchive) to extract the disc, Python 3
- To regenerate the symbol map: reference executables that keep their symbols (scripts/symbols.sh)
- A CPU with AVX2 (the runtime is built for `x86-64-v3`)
- Your own copy of `SLUS-21027` (check the hash above)

## Build and run

```bash
git clone --recurse-submodules https://github.com/TheBozos/TLOTR_TTA_Recomp.git
cd TLOTR_TTA_Recomp
scripts/extract_iso.sh "/path/to/Lord of the Rings, The - The Third Age (USA).iso"   # disc files -> game/
scripts/build_runtime.sh                       # recompile + sound driver + runtime -> build/bozo
scripts/run.sh --frames 600                    # tta-headless: runs and reports what is not native yet
```

`build/bozo/tta.exe` is the playable window build. It reads the disc image at the repository's root under its
No-Intro name (`Lord of the Rings, The - The Third Age (USA).iso`); give another place with `--iso FILE` or configure
with `-DTTA_ISO=FILE`. Without it the game stops at once and says where it looked. The movies' decoder is a minimal
FFmpeg (9.0.2, LGPL, only its MPEG-2 video decoder) that `scripts/build_runtime.sh` downloads (checked against its
SHA-256) and builds once into `build/ffmpeg` (`scripts/build_ffmpeg.sh`, ~2 minutes), linked static: the executables
need no DLL beyond Windows' own, and `tta.exe` starts from Explorer. `TTA_SYSTEM_FFMPEG=1` uses the system's FFmpeg
through pkg-config instead (on Windows its DLLs are then copied next to the executables). Its saves (the two memory cards) live in `%APPDATA%\TheBozos\The Third Age\cards`; `--card-dir DIR` puts them
elsewhere.

**Widescreen:** the Game tab of the settings window (F2), kept for the next game (`tta.ini` in the same folder as
`video.ini`), or `tta.exe --widescreen` (or `TTA_WIDESCREEN=1`) for one run, shows a 16:9 picture: the 3D scene a third wider than
the console's 4:3 view, the HUD, the menus and the movies keeping their shape in the middle, and full-screen effects
(tints, fades) across the whole width.

**Skippable logos:** the EA, THX and New Line logos at boot (one movie, about 28 seconds) cannot be skipped on
the console; the Game tab's "Skippable logos" (kept in `tta.ini`, or `TTA_SKIP_LOGOS=1` for one run) lets START end
them, as the developers' own option `skipallmovies` did.

**Volume:** the Game tab's slider (kept in `tta.ini`, or `TTA_VOLUME=0..100` for one run) sets the sound's volume,
music, voices and effects together; the console left it to the TV.

**Video settings:** the Video tab of the settings window (F2) sets the window's size, windowed / full screen /
borderless (F11 or Alt+Enter switch), the 3D's render scale, texture upscaling, texture smoothing (off as the console,
trilinear, or anisotropic up to 16x: distant ground without the shimmer) and the frame rate, saved in
`%APPDATA%\TLOTR_TTA_Recomp\video.ini`. `--fullscreen` starts borderless full screen. Scripted and `TTA_DETERMINISTIC`
runs ignore the file, so their captures stay the same. Above the game's 60 frames a second (120, 144, the monitor's
refresh), the pictures between the game's frames are drawn interpolated: the scenery and the characters move between
their two frames' places, the HUD and effects show the newest frame. `TTA_FRAME_RATE=<n>` sets it for any run.
"Pause in the background" (off by default; `pause_in_background` in `video.ini`, `PS2X_PAUSE_IN_BACKGROUND=on`) holds
the game while another window has the focus or this one is minimised.

`scripts/regress.sh` is the visual regression check: fifteen scenes (the boot films and title, the logos skipped with START, the first battle and
the same in widescreen, the outpost, the forest, the pause menu, Idrial's film, the Balrog's film, and battles in
Western Moria, Rohan, Helm's Deep, Osgiliath and Minas Tirith) captured from local save states and compared with a local baseline (`--update` makes it), ~5 minutes. Its runs are reproducible to the pixel.

Save states: F5 saves, F7 loads, F6 / Shift+F6 pick the slot (0-9), and the F2 window has a Save states tab. From the
command line: `--save-state FILE --save-state-at N` and `--load-state FILE [--load-state-at N]`. A save waits while a
movie plays. F10 writes a bug report of the moment (the RAM and one frame of draw state) for the developers.

Debug tools (the F2 window's **Debug** tab, built on the game's own level-script functions):

- **Warp**: loads any map from its start (its films, then its start position): Eregion, Western Moria, Eastern Moria,
  Eastemnet Gullies, Rohan, Helm's Deep, Osgiliath, Minas Tirith, Pelennor Fields (and e02c04).
- **Encounters**: every battle the current map defines, its story battles (Eregion: the Nazgul, the orc ambushes and
  warbands, the warg camps, the Watcher...) and its random ones (`Rnd..`), started on the spot.
- **Enemy groups**: any enemy group of the current map in a battle: the random battles' groups (`Rnd_Easy / Med /
  Hard`, each with its 1-, 2- or 3-enemy layout) or a story battle's (the Watcher as an ordinary fight).

Start them while walking freely, not during another battle; a battle asked for during a film or a prompt waits for it
to end (two minutes at most). Enemies of another region: warp there first.

From the command line: `--warp MAP[@FRAME]` (a region such as `"helms deep"`, a path such as `e02c05`, or an index)
`--encounter ID[@FRAME]` and `--group ID[@FRAME]` (frames are the game's), for example from a saved state:
`tta --load-state build/states/forest_14900.state --encounter 2118@14940` starts Eregion's first warg camp.
`--game-option NAME[=VALUE]` passes the developers' own options to the game (`encounters_only`).

### Windows

The same scripts run in an MSYS2 **UCRT64** shell, with MSYS2's GCC (MSVC is not supported for this port).

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,libarchive,python} make git
```

## Sharing the machine with other ports

The Bozos' ports share a two-slot bench lock (BozoEngine's `tools/bench_lock.sh`): a game run takes one slot, a build
or a timed run both, in arrival order.

```bash
scripts/bench_run.sh 5 "headless boot" scripts/run.sh --frames 600
scripts/bench_run.sh --alone 30 "full rebuild" scripts/build_runtime.sh
```

## Legal

This project contains no code or data from the game. You must own the game to use it. *The Lord of the Rings: The
Third Age* is a trademark of its respective owners; this project is not affiliated with or endorsed by them.

The executables link FFmpeg (LGPL-2.1-or-later), built from its unmodified source by `scripts/build_ffmpeg.sh`; its
licence is copied to `build/ffmpeg/COPYING.LGPLv2.1`.
