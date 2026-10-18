HOME Menu art for the AM2R build, picked up by cmake/n3ds.cmake: `icon.png` (48x48, AM2R's icon: `AM2RIcon.png` from the
[AM2R Launcher](https://github.com/AM2R-Community-Developers/AM2RLauncher), scaled to a third with nearest-neighbour) and
`banner.png` (256x128, the AM2R logo) with `banner.wav` (half a second of silence; a banner needs a sound).

`banner.png` carries the Chinese title: it is cut out of the game's own Chinese logo
(`sd/3ds/am2r/lang/titles/[simplified chinese] ... .png`, 270x101) and rebuilt to the original banner's proportions.

> The source logo is **not centred in its own canvas** - its content sits at x 55..264 of 270, with 55 columns of
> empty space on the left and 5 on the right - so scaling the whole image puts the text off to the right and leaves
> white above and below. Crop to the content first, then fit:
>
> ```
> ffmpeg -i title.png -vf "crop=210:68:55:30,scale=223:-1:flags=lanczos,pad=256:128:16:28:color=black@0" \
>        -pix_fmt rgba banner.png
> ```
>
> That gives content 223x72 with 16 columns and 28 rows of margin, against the original banner's 223x62, 18/15 and
> 33/33.
>
> **The padding must be transparent, not white.** The HOME Menu draws the banner over its own background, and the
> original banner is transparent everywhere outside the logo (its corners are `0,0,0,0`). A white pad shows up as a
> white frame on all four sides.
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
