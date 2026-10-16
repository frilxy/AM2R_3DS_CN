# Localization (Chinese, with a config.ini language switch)

AM2R's `data.win` carries **no CJK glyphs at all**. The language packs make Chinese appear by swapping whole fonts at
runtime through GameMaker's `font_replace`, which was never implemented in this port — so `lang/schinese.ini` loaded,
the language name was read correctly, and every string still drew blank or in English. This is that implementation,
plus `background_replace` (the packs use it for the title logo) and three smaller fixes that turned out to be needed
before the Chinese title screen appeared.

## What the game asks for

`oControl`'s Create event does this (the strings are in `data.win`; the GML is quoted in the localization notes that
came with this work):

```gml
ini_open("lang/fonts/sizes.ini");
if (file_exists("lang/fonts/04b09.ttf")) {
    font_replace(fontGUI,    "lang/fonts/04b09.ttf",           ini_read_real('Sizes','04b09_(large)',16), false,false,32,255);
    font_replace(fontSubScr, "lang/fonts/04b09.ttf",           ini_read_real('Sizes','04b09_(small)', 8), false,false,32,255);
}
if (file_exists("lang/fonts/Acknowledge_TT_BRK.ttf"))
    font_replace(fontGUI2,      "lang/fonts/Acknowledge_TT_BRK.ttf", ini_read_real('Sizes','Acknowledge_TT_BRK',13), false,false,32,255);
if (file_exists("lang/fonts/uni_05_53.ttf"))
    font_replace(fontMenuSmall, "lang/fonts/uni_05_53.ttf",          ini_read_real('Sizes','uni_05_53',8),          false,false,32,255);
if (file_exists("lang/fonts/04b24.ttf"))
    font_replace(fontMenuTiny,  "lang/fonts/04b24.ttf",              ini_read_real('Sizes','04b24',16),             false,false,32,255);
if (file_exists("lang/fonts/Glasstown_NBP.ttf"))
    font_replace(fontMenuSmall2,"lang/fonts/Glasstown_NBP.ttf",      ini_read_real('Sizes','Glasstown_NBP',16),     false,false,32,255);
ini_close();
```

Two consequences that are easy to miss:

* **`sizes.ini` is keyed by font FILENAME**, not by GameMaker font name. The eight GameMaker font names
  (`fontGUIDefault`, `fontSubScrDefault`, …) appear nowhere in `data.win`.
* `font_replace` is only called **if the file exists**, so the five filenames are load-bearing and this port ships its
  own copies of all five (`res/n3ds/am2r/overlay/`).

## Why the fonts ship with the port

The language pack that came with AM2R points those five names at a 16 MB outline CJK face with 12–16px strikes, which
is the wrong kind of font for this renderer *and* the wrong sizes. The port replaces them with
**fusion-pixel-font** 2026.09.25 (MIT), the pixel font the layout was designed around. See
`res/n3ds/am2r/overlay/README.md` for the name → size → md5 table.

The sizes are forced by the game's layout, not chosen for looks:

| font | constraint | size |
|---|---|---|
| `fontMenuTiny` (item names) | drawn on a **9px stride** (`y + 15/24/33/…`) | ink must fit in **8px** |
| `fontMenuSmall` (option hints) | drawn at `y = 228` on a 240px screen | **10px** fills it exactly |
| the rest | body text, dialogue, menus | **12px** |

## Reading the fonts: 1-bit strikes, not outlines

A pixel font rasterized from its outlines comes out grey and too bold, so the bitmaps are taken from the TTF's own
**EBLC/EBDT** tables. The formats that matter, and the three things that are easy to get wrong:

| indexFormat | layout |
|---|---|
| 1 / 3 | one offset per glyph **plus a final one**; a glyph's size is the gap to the next offset |
| 2 | fixed size, with the big metrics **in the subtable header** |

| imageFormat | metrics | rows |
|---|---|---|
| 1, 2, 8, 9 | `smallGlyphMetrics`, 5 bytes, in the glyph entry | 1/2 byte-aligned, 8/9 packed |
| 5 | `bigGlyphMetrics` in the **EBLC subtable** | packed |
| 6 | `bigGlyphMetrics` in the glyph entry | byte-aligned |
| 7 | `bigGlyphMetrics` in the glyph entry, after **8 bytes** | packed |

The parser in `src/n3ds/n3ds_font_replace.c` was verified against an independent raw reader: **5103 glyphs are
pixel-exact**, including the CJK, across all three sizes.

## Glyph rectangles

The runner draws a glyph's rectangle at the line top with `cursorY` (which starts at `-ascenderOffset`, 0 for AM2R)
and advances by `maxGlyphHeight`. Measuring the game's own fonts with a dumper settled the convention:

* a rectangle is the **ink extended upward to the line's top**, so its bottom edge is the baseline;
* its width is the **ink width** (plus any negative left side bearing, which is nudged in by `offset`);
* the ink is contiguous from the rectangle's bottom.

So a period is a 12-row rectangle with 8 blank rows then 4 inked ones, and `g` gets a 16-row rectangle for its
descender. The packer reproduces exactly that shape.

## One texture for every size

All sizes share a single 1024×1024 texture, each stacked below the last by the rows it actually used. This is a hard
requirement, not an optimization: with two or more runtime atlases, particular strings stopped drawing while the
atlas contents, page numbers, glyph counts and memory use all looked correct, and the cause was never found.

Using the rows actually used matters — the sizes rounded up to powers of two are `512+512+256+256 = 1536`, which does
not fit in 1024. With the two languages the atlas needs about **379 of 1024 rows**.

The texture is uploaded **Morton-tiled**, 8×8, first row at the top, with the byte order the screenshot code reads
back (R in the top byte). Glyph strips skip transparent pixels; whole images (`background_replace`) must write every
pixel, because `C3D_TexInit` does not clear the memory and the gaps show up as garbage.

`font_replace` is called once per font, and three of the five files are byte-identical copies, so a strip is cached by
the file's contents and the requested size: without that, the same bitmaps filled the texture five times over.

## Language switching

`config.ini`'s `[Screen] Language` is the game's own setting (it reads it into `global.currentlanguage`), and
`lang/languages.txt` gives it the list — the line number is the value. The port ships:

```
english.ini    # Language="0"
schinese.ini   # Language="1"
```

Every language in that file has its characters put into the texture (the atlas is built from the language files
themselves, so it never has to cover all of CJK), which is what makes switching possible without a rebuild.

## The 3DS front end's own strings

The port replaces the game's Display page, and those rows are in no language file, so their characters would be
missing. They are handed to the loader before `oControl` builds the atlas
(`N3DSAm2r_registerPortGlyphs`), and the strings follow the selected language through `N3DSAm2r_text`.

## The other three fixes

Without all three the title screen stayed English:

1. **`src/ini.c`** — the language files start with a UTF-8 BOM, which sat in front of the opening `[` of the first
   section, so `[Header]` was never recognized and its `Language` line was dropped.
2. **`program_directory`** (`src/vm_builtins.c`/`.h`) — the built-in variable the game builds `lang/titles/…` paths
   from. The enum value is appended so no existing id changes, and the name went into the table at its sorted place
   (the table is bsearch'd).
3. **`src/overlay_file_system.c`** — `isAbsolute` only accepted a leading `/`, so a devoptab path like
   `sdmc:/3ds/am2r/lang/titles` was treated as relative and had the bundle path prepended, giving
   `sdmc:/3ds/am2r/sdmc:/3ds/am2r/…`.

## Verifying a change

The parser and packer are plain C with no 3DS dependency beyond the renderer's upload calls, so they can be checked on
a workstation against `fontTools` or a raw reader without a toolchain — that is how the 5103-glyph result above was
produced, and it is worth doing after any change to the formats, the packing or the rectangle convention.
