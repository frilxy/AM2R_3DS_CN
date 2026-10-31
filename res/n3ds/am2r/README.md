HOME Menu art for the AM2R build, picked up by cmake/n3ds.cmake: `icon.png` (48x48, AM2R's icon: `AM2RIcon.png` from the
[AM2R Launcher](https://github.com/AM2R-Community-Developers/AM2RLauncher), scaled to a third with nearest-neighbour) and
`banner.png` (256x128, the AM2R logo) with `banner.wav` (half a second of silence; a banner needs a sound).
Without them the title gets libctru's default homebrew icon and no banner (bannertool is needed for the banner).

`sd/` holds files installed on the SD card by `tools/n3ds/make_sd.sh` only when the card doesn't have them yet:
`config.ini` is AM2R's control config with a 3DS layout (Samus Returns style). The game's own Joypad menu still
rebinds them.

| 3DS | AM2R |
|---|---|
| B | Jump (menus: back) |
| Y | Fire |
| A | Morph (menus: OK) |
| L / ZL | Aim / Aim down |
| R | Missiles |
| ZR | Aim lock |
| Select | Weapon select |
| Start | Pause |
| D-pad / Circle Pad | Move |

AM2R's legacy DirectInput joystick bindings are off (`EnableJoystick="0"`): they would read the same buttons again
under another numbering.
