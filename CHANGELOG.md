# Changelog

All notable changes for players. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/).

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
