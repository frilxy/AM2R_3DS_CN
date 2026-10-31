# AM2R for 3DS — Chinese Edition

Play **AM2R** (Another Metroid 2 Remake) on a **New 3DS / New 2DS XL**, fully translated into Simplified Chinese, with a live map on the bottom screen.

[中文](README.md) · [Development notes (Chinese)](开发文档.md) · [Releases](https://github.com/frilxy/AM2R_3DS_CN/releases)

![Landing site](docs/screenshots/landing-site.png)

> [!IMPORTANT]
> **You must supply your own AM2R game data; the game itself is not included.**
> Build the SD folder with `tools/n3ds/make_sd.sh` from your own AM2R 1.1 or Community Updates 1.5.5 copy.

## Requirements

- A hacked (CFW) New 3DS / New 3DS XL / New 2DS XL with **FBI**
- Your own AM2R `data.win`

## Install

1. Run `tools/n3ds/make_sd.sh` against your AM2R folder and copy the resulting `3ds` folder to the SD card root (`sdmc:/3ds/am2r/`, about 565 MB).
2. Install `am2r.cia` with FBI, or run `am2r.3dsx` from the Homebrew Launcher.
3. `sdmc:/3ds/am2r/config.ini` selects the language: `[Screen] Language` = `1` for Simplified Chinese (default), `0` for English.

Upgrading from an older build is just a CIA reinstall; saves and SD data stay.

## Controls

| Input | Action |
|---|---|
| A / B / X / Y / L / R / ZL / ZR | In-game actions (as configured in the game) |
| **START** | Pause → map page; press again to resume |
| **SELECT** | Tap: the game's own (area description after a scan); **hold**: pause → items page, press again to resume |
| Bottom screen touch | Drag the map; tap to place/remove a marker |
| START + SELECT | Debug overlay |
| Pause menu → Display | Screen mode (point / wide / stretch), status bar, frameskip, fast scripts, FPS counter, cheats |

## What this build adds

- Full Simplified Chinese translation (`font_replace` / `background_replace` implemented, bitmap fonts, line-wrap bug fixed)
- Live bottom-screen map: status counters, zoom levels, drag to pan, tap to mark; artwork on non-gameplay screens
- Screen modes: point (native 320×240, default), wide, stretch
- Fixes for the save-select stutter and a long-session crash (atlas page OOM); fonts are cached on the SD card so the second boot is about a second faster

## Two things you can replace

| File | Purpose |
|---|---|
| `sdmc:/3ds/am2r/gfx/bottom.png` | Bottom-screen artwork (320×240 PNG); swap it without reinstalling |
| `sdmc:/3ds/am2r/fontcache.bin` | Font cache, created automatically; delete it to force a rebuild |

## Building

Built by GitHub Actions only (`.github/workflows/build-n3ds.yml`): every push produces a pre-release with `am2r.cia` / `am2r.3dsx`, and a tag publishes a full release. No local devkitARM is needed.

## Credits

- **AM2R** by DoctorM64; **AM2R Community Updates** by the community
- 3DS port based on [nattensmadrigal/AM2R_3DS](https://github.com/nattensmadrigal/AM2R_3DS) (Butterscotch / devkitARM / libctru)
- Translation, bottom-screen map, screen modes and performance work by this project

See [开发文档.md](开发文档.md) for implementation details and known issues.
