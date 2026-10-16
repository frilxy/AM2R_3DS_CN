# AM2R for New 3DS

Play **AM2R** (Another Metroid 2 Remake) on your **New 3DS / New 2DS XL**.

![Samus at the landing site](docs/screenshots/landing-site.png)
![Samus over lava](docs/screenshots/lava.png)

> [!IMPORTANT]
> You need your own copy of AM2R. The game itself is not included.
>
> This branch is the **top-screen** build: the game and its pause menu both run on the top screen, and the bottom
> screen stays black. The touch screen is not used; the pause menu takes the game's own buttons.

## You need

- A New 3DS, New 3DS XL or New 2DS XL with custom firmware and **FBI** ([3ds.hacks.guide](https://3ds.hacks.guide)).
- Your AM2R 1.1 zip (`AM2R_11.zip`).
- A computer with a desktop web browser (Chrome, Edge or Firefox).

## Install

1. Open the **[AM2R 3DS SD Card Maker](https://nattensmadrigal.codeberg.page/AM2R_3DS/)**, drop in your `AM2R_11.zip` and wait a minute or two. It updates
   the game to the Community Updates 1.5.5 and converts it for the 3DS, all in your browser (nothing is uploaded).
   Extract the zip it gives you to the root of your SD card.
2. Install `am2r.cia` with FBI: Remote Install → Scan QR Code, and scan the code the page shows.

That's it: AM2R is on your HOME Menu.

Save the page (Ctrl+S) to keep a copy that works offline. Already updated your game with the AM2R Launcher? The
`am2r-sd.bat` (Windows) / `am2r-sd.sh` (Linux) scripts in [Releases](../../releases) still convert that copy.

## Controls

| Button | Action |
|---|---|
| B | Jump |
| Y | Shoot |
| A | Morph Ball |
| L / ZL | Aim |
| R | Missiles |
| ZR | Aim lock |
| Select | Switch weapon |
| Start | Pause |

The pause menu is the game's own (`rm_subscreen`) and is driven with the buttons above: the D-pad moves the
selection, **A** confirms and **B** goes back. The whole game, pause menu included, is on the top screen; the bottom
screen is black. AM2R's own widescreen picture is fitted to the top screen at 400x240 with square pixels.

## Problems?

Open an issue here and tell us what happened.

---

AM2R by DoctorM64 and team, with updates by the AM2R Community Developers. Not affiliated with Nintendo.
For developers: [.github/README.md](.github/README.md).
