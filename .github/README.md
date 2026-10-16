# AM2R 3DS (developer README)

> [!NOTE]
> Players: downloads and the install guide are on Codeberg, [codeberg.org/NattensMadrigal/AM2R_3DS](https://codeberg.org/NattensMadrigal/AM2R_3DS). This page is for building and working on the port.

AM2R (Another Metroid 2 Remake) on the New Nintendo 3DS: the 1.5.5 Community Updates (recommended) or the original 1.1.

This is a game-specific fork of [Butterscotch](https://github.com/ButterscotchRunner/Butterscotch), an open source
re-implementation of the GameMaker: Studio runner in C. Butterscotch runs the game's own bytecode (`data.win`), so the
3DS build runs AM2R's real game code; this repository adds the 3DS platform layer and the changes AM2R needs on it. The
3DS rendering, audio and preprocessing code started from
[Cinnamon](https://github.com/Project-Sunshine-Native/cinnamon)'s Undertale 3DS port.

> [!IMPORTANT]
> You need your own copy of AM2R. This repository does not include any game files.

## Which version

- **AM2R 1.5.5 (Community Updates)**, made from your AM2R 1.1 with the
  [AM2R Launcher](https://github.com/AM2R-Community-Developers/AM2RLauncher): the version this port is tuned for. Its
  own 16:9 widescreen picture (426x240) suits the 400x240 top screen, and the "Fast scripts" option (built-in versions of
  its busiest scripts) only applies to it.
- **AM2R 1.1**: still runs (4:3, stretched or 1:1).

Saves don't carry over between the two: 1.5 saves as `save1`..`save3`, 1.1 as `sav1`..`sav3`.

## Install

1. **SD card files.** Open the [SD card maker](https://nattensmadrigal.codeberg.page/AM2R_3DS/) (`web/`), drop in the AM2R 1.1 zip and extract the zip it
   makes to the root of the SD card: it applies the Community Updates 1.5.5 patch and runs the preprocessor in the
   browser. Or download `am2r-sd.bat` (Windows) or `am2r-sd.sh` (Linux) from the
   [latest release](https://github.com/pegacha/Butterscotch/releases/latest) and run it. Pick the game's `data.win` (for 1.5.5: the one in the
   launcher's `Profiles/Community Updates (Latest)` folder, with its music files next to it) and a folder; it writes
   `<folder>/3ds/am2r/`, which you copy to the root of the SD card, and downloads `am2r.cia` next to it.
   - On Windows it runs in WSL, and offers to install WSL (administrator rights and a restart) if it isn't there.
   - The first run downloads the tools it needs (the preprocessor) into its own folder.
   - Developers can also run `tools/n3ds/make_sd.sh <game folder> <out>` from a checkout (a C compiler and CMake).

   The folder only needs rebuilding when a release says it needs a newer SD data revision; the game tells you at
   startup if the card's folder is out of date.

2. **The program.** Open the [latest release](https://github.com/pegacha/Butterscotch/releases/latest) and scan its QR code with FBI
   (Remote Install → Scan QR Code), or download `am2r.cia` and install it with FBI from the SD card.
   Every passing build is published as a release; builds from branches other than `main` are marked pre-release
   (`claude/*` work-in-progress branches only get a build artifact).

Saves, `cheats.ini` (the 3DS options) and `log.txt` go in `sdmc:/3ds/am2r/`.

## Playing

- **Buttons:** B jump, Y fire, A morph, L/ZL aim, R missiles, ZR aim lock, Select weapon, Start pause; in menus A is OK
  and B is back. The full layout is in [res/n3ds/am2r/README.md](../res/n3ds/am2r/README.md); the game's own Joypad menu
  can rebind it.
- **One screen:** the game runs on the **top** screen, and so does its pause menu (`rm_subscreen`), driven with the
  game's own buttons (D-pad, A = OK, B = back). The bottom screen stays black and the touch screen is not used.
- **Options → Display** is a 3DS page:
  - *Screen:* AM2R's own widescreen picture (426x240) is wider than the top screen (400x240), so this picks what gives:
    - **Wide** (default) — the picture 1:1 and centred: square pixels, no scaling, no bars, with the 26 columns past
      400 given up 13 at each edge.
    - **Stretch** — the whole 426 columns squeezed into 400, so nothing is lost but the picture is 6% narrower.
    - **1x** — the whole picture fitted and centred (400x225, 7px bars above and 8 below), slightly soft.
    - **2x** — 2x, centred on the middle of the picture.
  - *HUD:* on by default. The game's own switch for the energy, ammo and map display during play (its Options page had
    the row; this page carries it now). Off draws no HUD at all.
  - *Frameskip:* a steady 30 fps at full game speed.
  - *Fast scripts:* on by default. Runs the game's busiest scripts (HUD, minimap, collision checks, the Area 6 light
    map, ...) as built-in code: same result, much faster. "N/A" for game versions it wasn't written for.
  - *Cheats:* unlimited health and ammo, stronger weapons.
- **Start+Select** shows a performance monitor.

## Performance

Measured in the Azahar emulator at CPU clock 100 (close to a real New 3DS) with the 1.5.5 game files, release build.
Most rooms run at the full 60 fps; the busiest ones need 12–15 ms of the 16.7 ms frame budget.

| room | before (2026-10-06) | now |
|---|---|---|
| `rm_a3h04` (730 objects) | 28 fps | 51 fps |
| `rm_a6b14` (Area 6, light map) | 29 fps | 49 fps |
| `rm_a5h04` (lava) | 26 fps | 51 fps |
| `rm_a2h02` | 47 fps | 52 fps |

(Each figure includes a screenshot stall in the test; real hardware numbers welcome, via `log.txt`.)

## Building

```bash
tools/n3ds/build.sh ../build-n3ds    # -> am2r.cia + am2r.3dsx (needs devkitARM, and makerom for the .cia)
```

[.github/workflows/build-n3ds.yml](workflows/build-n3ds.yml) is the reference build: the `devkitpro/devkitarm`
container, with makerom built from [3DSGuy/Project_CTR](https://github.com/3DSGuy/Project_CTR).

[PORTING_NOTES.md](../PORTING_NOTES.md) covers how the port works: the SD layout, the emulator test loop (Azahar) and its
harness, the renderer, audio and file system, the pause screen, the built-in scripts, profiling, performance work and
known issues.

## Credits

* AM2R by DoctorM64 and team; the Community Updates by the AM2R Community Developers.
* [Butterscotch](https://github.com/ButterscotchRunner/Butterscotch) by MrPowerGamerBR and contributors.
* [Cinnamon](https://github.com/Project-Sunshine-Native/cinnamon) (3DS, Wii and Wii U ports of Butterscotch) by
  @casrielasriel and @grayforz24682.

## License

GNU Affero General Public License v3.0, as Butterscotch: see [LICENSE](../LICENSE).

## Disclaimer

This project has no association, endorsement, or any connection whatsoever with Nintendo, the AM2R team, the AM2R
Community Developers, YoYo Games or any of the software it runs, and does not provide any of it. You need to provide
your own game files.
