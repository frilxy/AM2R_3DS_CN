# Language assets this port replaces

Everything under `overlay/` is copied over the game folder **after** the game's own files (`tools/n3ds/make_sd.sh`),
because AM2R's shipped language pack targets outline fonts while this port draws from the pixel fonts' 1-bit bitmap
strikes, baked offline into `lang/fonts/cjk-font.bin` (see `docs/font-plan.md`).

## `lang/fonts/*.ttf`

The five filenames are the ones `data.win` itself asks for in `oControl`'s Create event — `font_replace` is only
called when the file exists, so the names have to match. The mapping from name to pixel size is the game's own, from
`lang/fonts/sizes.ini`:

| file | strike in the file | size asked for | used by |
|---|---|---|---|
| `04b09.ttf` | 12px | 12 (large) / 10 (small) | `fontGUI`, `fontSubScr` |
| `Acknowledge_TT_BRK.ttf` | 12px | 12 | `fontGUI2` |
| `Glasstown_NBP.ttf` | 12px | 12 | `fontMenuSmall2` |
| `uni_05_53.ttf` | 10px | 10 | `fontMenuSmall` |
| `04b24.ttf` | 8px | 10 | `fontMenuTiny` |

The two columns are different on purpose: the size the game asks for is the **ascent** it laid the line out with
(a glyph's rectangle runs from the line top down to its ink), while the strike decides how large the ink is drawn.
Nothing is scaled: a pixel font rasterized bigger comes out grey and bold. So `04b24.ttf` holds the 8px face under a
10px request, because `fontMenuTiny` draws item names on a 9px stride and its ink has to fit in 8px, and `04b09.ttf`
holds one face for two slots - `fontGUI`'s 12px and `fontSubScr`'s 10px request share it, the smaller one just crops
two blank rows off each rectangle (see `docs/font-plan.md`).

The files are **fusion-pixel-font** 2026.09.25, `proportional` / `zh_hans` / `.ms`, from
<https://github.com/TakWolf/fusion-pixel-font> (MIT). Each carries exactly one 1-bit `EBLC`/`EBDT` strike:

| placed as | source file | bytes | md5 |
|---|---|---|---|
| `04b09.ttf`, `Acknowledge_TT_BRK.ttf`, `Glasstown_NBP.ttf` | `fusion-pixel-12px-proportional-zh_hans.ms.bitmap.ttf` | 1682340 | `c9d542b217310ac7921cd1cfd77de7bf` |
| `uni_05_53.ttf` | `fusion-pixel-10px-proportional-zh_hans.ms.bitmap.ttf` | 1112952 | `6a733808377bf34cc14e0c4cca49549c` |
| `04b24.ttf` | `fusion-pixel-8px-proportional-zh_hans.ms.bitmap.ttf` | 1177576 | `d21138035b6d1186e6a8ebf6c1a37414` |

The sizes are forced by the game's layout, not chosen for looks: `fontMenuTiny` draws item names on a 9px stride, so
its ink has to fit in the 8px strike, and `fontMenuSmall` draws at `y = 228` on a 240px screen, which the 10px strike
fills exactly.

## `lang/fonts/cjk-font.bin`

The glyphs the port's `font_replace` loads: every character the language files use, the port's own front-end strings and
ASCII, rasterized from the same five files above and packed into one 1024x512 page by
`tools/n3ds/fontgen/make_cjk_font.py`. Regenerate it whenever the translation, `sizes.ini` or the fonts change;
The page as a picture is `docs/cjk-font-atlas.png` in the repository (review only -- it is not shipped).
`tools/n3ds/fontgen/verify_cjk_font.py` re-reads
the pack like the 3DS does and draws a sample string out of it.

## `lang/languages.txt`

One file per line; the line number is what `config.ini`'s `[Screen] Language` selects, and every file listed has its
characters put into the font texture, so switching language needs no rebuild. Only the two the port supports:

```
english.ini    # Language="0"
schinese.ini   # Language="1"
```

## `lang/fonts/sizes.ini`

Read by the game itself. The keys are font **filenames**, not GameMaker font names — that is what `data.win` looks
up. `[Offsets]` is consumed too (`04b24_offset` shifts the item names up by 6px).
