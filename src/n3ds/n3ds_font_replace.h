#pragma once

// font_replace(font, ttf, size, bold, italic) on the 3DS: the game's data.win has no CJK glyphs at all, so the
// language packs swap whole fonts at runtime with this built-in. Nothing else implements it, so the loaded font is
// rasterized here from the TTF's own 1-bit bitmap strikes (the packs ship pixel fonts) instead of from outlines.

#include "../runner.h"

#include <stdbool.h>
#include <stdint.h>

// Replaces fontIndex's glyphs with the ones rasterized out of the TTF at ttfPath. Returns false (leaving the font as
// it was) when the file cannot be read or holds no usable bitmap strike.
bool N3DSFontReplace_apply(Runner* runner, int32_t fontIndex, const char* ttfPath, float emSize, bool bold, bool italic);

// Characters the 3DS front end draws itself (its Display and Cheats pages). They are in no language file, so they
// would otherwise be missing from the atlas the loader builds out of the language files. Call before the game's
// oControl runs its first font_replace.
void N3DSFontReplace_addExtraText(const char* text);

// As addExtraText, for a string that is built at startup and kept alive by the caller.
void N3DSFontReplace_resetExtraText(void);

// Turns a path the game hands a built-in ("lang/fonts/04b09.ttf", "lang/titles/<file>.png") into one that opens on
// the 3DS. The game builds these from program_directory, which this port resolves through its FileSystem, but the
// built-ins that take a file path (font_replace, background_replace) read the file themselves - and a bare relative
// path does not resolve there: the 3DS runs with a current directory that is not the game's folder, so fopen() on
// "lang/fonts/04b09.ttf" fails and every font slot is left as it was. An absolute path (one with a drive and a leading
// slash, or a leading '/') is returned unchanged; anything else is put under N3DS_SD_DIR. The caller frees the result.
char* N3DSFontReplace_resolvePath(const char* path);
