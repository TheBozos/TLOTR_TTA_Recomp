# Changelog

All notable changes for players. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.2] - 2026-11-24

### Fixed
- The game feels smooth at 60 frames a second on a 60 Hz monitor: a picture no longer stays twice now and then while the next is skipped, and the controls answer a few frames sooner.
- The first movie after the title screen no longer loses about a second of its sound as it starts (the sound of a load came out all at once and overflowed).

## [0.1.1] - 2026-11-24

### Added
- Cheats: a "Win this battle" button.
- A Special thanks tab in the settings window (F2), for the players who test the port and report what they find.

### Fixed
- The soft glow around highlighted and buffed characters in some battles is drawn.
- The palantir that shows in the top-left corner when an encounter is near is no longer hidden behind trees (#3).
- Crowded scenes with many shadows, such as the start of the battle on Helm's Deep's wall, run about 9% faster, and their shadows cost less again since.
- Characters fading in at the start of a battle no longer show the inside of their heads (the face drawn over the back of the head, as if turned around) (#4).
- The forest ground no longer flickers at the camera's feet on slopes (a second ground layer showed through for single frames as the camera moved) (#5).
- A character's sword no longer throws a long dark strip of shadow for single frames.
- The palantir that warns of a coming encounter fades in again instead of showing at full strength at once (#6); other glows fade by the game's strength too.
- Light rays and lens flares from a light hidden behind walls no longer flash on while the camera turns (#7).
- The lake at the end of Eregion has its water surface again (#8).
- Spells that darken the screen (Aura of the Valar, the Nazgul's Scream, Rage of Saruman and others) no longer cover their own caster and light: the screen no longer turns almost solid blue during Aura of the Valar, and the spells' glows shine as on the console (#9).
- In widescreen, objects at the sides of the picture (such as a save point in Moria) no longer vanish until they reach the middle (#10).
- Cutscenes with depth of field (such as the one after the Watcher in the Water) are no longer blurred all over: only the background is soft, as on the console.
- An in-game save made after loading a save state (F7) is written to the memory card again; it used to be lost when the game closed.
- Save states made with 0.1.0 keep loading with the updated engine (two engine changes had made them refused).

## [0.1.0] - 2026-11-23

The first release: The Lord of the Rings: The Third Age (USA, SLUS-21027) as a native Windows game on BozoEngine, built on your PC from your own disc by the launcher.

### Added
- The TTA Launcher, the same as TMB's and Dark Cloud's: it checks your disc image, prepares the game from it (about half an hour, once; it can be stopped and resumed), then plays it. Display settings, updates, and a Maintenance tab for the saves and logs. English and French.
- The whole game natively: no PS2 emulation at any level. The movies, the menus and HUD, the 3D (terrain, characters, effects, water, glows, shadows) and EA's sound driver, compiled from the disc.
- PC options: any window size or full screen, render scale, widescreen (16:9), anti-aliasing (MSAA up to 8x), texture upscaling and smoothing, frames between the game's on high-refresh monitors, and pausing in the background.
- Save states (F5 / F7, ten slots with pictures), screenshots (F12), fast forward (hold the key left of 1, or a gamepad's touchpad), skippable boot logos and a volume setting.
- Keyboard, mouse and gamepads, all rebindable, with rumble; the settings window can be driven from a gamepad.
- Cheats (F2, Cheats): infinite HP and AP, one-hit kills, an experience multiplier, and any item added to the bag.
- Debug tools (F2, Debug): warp to any region, start any battle.

### Known limits
- Windows only for now; Linux and macOS are planned.
- A full playthrough from the intro to the end has not been made yet. Every region loads and its battles play, and the last battle and the ending were reached through the debug tools.
- A few effects still differ from the console: the coloured glow around highlighted characters is missing.
