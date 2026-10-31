// Reads the CJK font pack (cjk-font.bin) and installs one font's glyphs out of it.
//
// The pack is baked offline by tools/n3ds/fontgen/make_cjk_font.py: a language file's characters, rasterized from the
// pixel fonts' own 1-bit bitmap strikes, laid out as rectangles the runner can draw (line top to baseline) into one
// 1024x1024 page. This file therefore does no font parsing, no packing and no scaling: it reads the page, uploads it
// once as a runtime texture page, and copies the rectangle table of the requested font into the game's font.
//
// Why it is baked and not built at boot: the port used to rasterize the TTFs on the 3DS (1.3 s of boot, plus a font
// cache to make the next boot faster). Doing it offline removes both, and the art stays inspectable: the generator
// also writes cjk-font.png, the same page as a picture.

#include "n3ds_cjk_font.h"

#include "n3ds_renderer.h"

#include "../log.h"
#include "../utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N3DS_CJK_PACK_NAME "cjk-font.bin"
#define N3DS_CJK_PACK_MAGIC 0x464B4A43u // "CJKF", little-endian
#define N3DS_CJK_PACK_VERSION 1u
#define N3DS_CJK_STRIP_SIZE 44u // the on-disk strip record: name, size, y, height, glyphs, offset
#define N3DS_CJK_GLYPH_SIZE 14u // the on-disk glyph record

// One entry of the pack's strip table: a (font file, pixel size) pair, and where its glyph rectangles are.
typedef struct {
    uint16_t size;
    uint16_t yOffset;
    uint16_t maxGlyphHeight;
    uint16_t glyphCount;
    uint32_t glyphOffset;
} N3DSCjkStrip;

static uint8_t* gPack = NULL;
static uint32_t gPackSize = 0;
static uint32_t gStripsOffset = 0;
static uint32_t gStripCount = 0;
static uint32_t gTextureWidth = 0;
static uint32_t gTextureHeight = 0;
static uint32_t gUsedRows = 0;
static uint32_t gBitmapOffset = 0;
static bool gPackMissing = false;
static int32_t gTexturePage = -1;

static uint16_t N3DSCjk_readU16(const uint8_t* p) {
    return (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
}

static uint32_t N3DSCjk_readU32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static int16_t N3DSCjk_readS16(const uint8_t* p) {
    return (int16_t) N3DSCjk_readU16(p);
}

// The 8x8 Morton index the 3DS samples textures with (the same order the atlas pages are stored in).
static uint32_t N3DSCjk_morton(uint32_t x, uint32_t y) {
    return (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2) | ((x & 4u) << 2) | ((y & 4u) << 3);
}

// "lang/fonts/04b09.ttf" -> "lang/fonts/cjk-font.bin" (caller frees). The pack travels with the fonts it was baked
// from, so a language folder copied to the card keeps working without any configuration.
static char* N3DSCjk_packPath(const char* ttfPath) {
    if (ttfPath == NULL) return NULL;
    const char* slash = strrchr(ttfPath, '/');
    size_t directory = slash != NULL ? (size_t) (slash - ttfPath) + 1u : 0u;
    size_t nameLength = strlen(N3DS_CJK_PACK_NAME);
    char* path = (char*) safeMalloc(directory + nameLength + 1u);
    if (directory > 0u) memcpy(path, ttfPath, directory);
    memcpy(path + directory, N3DS_CJK_PACK_NAME, nameLength + 1u);
    return path;
}

static bool N3DSCjk_loadPack(Runner* runner, const char* ttfPath) {
    if (gPack != NULL) return true;
    if (runner == NULL || runner->fileSystem == NULL) return false;

    char* path = N3DSCjk_packPath(ttfPath);
    if (path == NULL) return false;
    uint8_t* data = NULL;
    int32_t size = 0;
    bool read = runner->fileSystem->vtable->readFileBinary(runner->fileSystem, path, &data, &size);
    if (!read || data == NULL || size < 32) {
        free(data);
        // Logged once: the game calls font_replace for every font at boot and the answer would be the same each time.
        if (!gPackMissing) logInfo("AM2R: no CJK font pack at %s, keeping the game's fonts\n", path);
        gPackMissing = true;
        free(path);
        return false;
    }
    free(path);

    if (N3DSCjk_readU32(data) != N3DS_CJK_PACK_MAGIC || N3DSCjk_readU32(data + 4) != N3DS_CJK_PACK_VERSION) {
        logError("AM2R: %s is not a version %u CJK font pack\n", N3DS_CJK_PACK_NAME, N3DS_CJK_PACK_VERSION);
        free(data);
        return false;
    }
    uint32_t width = N3DSCjk_readU16(data + 8);
    uint32_t height = N3DSCjk_readU16(data + 10);
    uint32_t usedRows = N3DSCjk_readU16(data + 12);
    uint32_t strips = N3DSCjk_readU16(data + 14);
    uint32_t stripsAt = N3DSCjk_readU32(data + 16);
    uint32_t bitmapAt = N3DSCjk_readU32(data + 20);
    uint32_t bitmapBytes = N3DSCjk_readU32(data + 24);
    // citro3d only takes power-of-two textures up to 1024, and a packed row is whole bytes.
    if (width < 8u || width > 1024u || (width & (width - 1u)) != 0u || (width % 8u) != 0u ||
        height < 8u || height > 1024u || (height & (height - 1u)) != 0u ||
        usedRows == 0u || usedRows > height ||
        bitmapBytes < ((width / 8u) * usedRows) ||
        bitmapAt < 32u || bitmapAt > (uint32_t) size || bitmapBytes > (uint32_t) size - bitmapAt ||
        stripsAt < 32u || stripsAt > (uint32_t) size || strips * N3DS_CJK_STRIP_SIZE > (uint32_t) size - stripsAt) {
        logError("AM2R: %s has an invalid header\n", N3DS_CJK_PACK_NAME);
        free(data);
        return false;
    }

    gPack = data;
    gPackSize = (uint32_t) size;
    gTextureWidth = width;
    gTextureHeight = height;
    gUsedRows = usedRows;
    gStripsOffset = stripsAt;
    gStripCount = strips;
    gBitmapOffset = bitmapAt;
    logInfo("AM2R: CJK font pack: %ux%u page, %u rows, %u strips\n",
        (unsigned) width, (unsigned) height, (unsigned) usedRows, (unsigned) strips);
    return true;
}

// Uploads the pack's page as a texture the draw calls can sample. Done once: every font shares the page, and a font's
// glyphs are just rectangles inside it (which is why one page has to be big enough for every strip).
static bool N3DSCjk_createTexturePage(Runner* runner) {
    if (gTexturePage >= 0) return true;
    uint16_t* pixels = NULL;
    int32_t tpag = N3DSRenderer_allocImagePage(runner->renderer, gTextureWidth, gTextureHeight, &pixels);
    if (tpag < 0 || pixels == NULL) {
        logError("AM2R: could not allocate the CJK font texture page\n");
        return false;
    }

    // 1-bit rows -> RGBA5551 white-on-transparent, written in 8x8 Morton tiles. Only set bits are touched: the page was
    // zero-filled by the allocator and 0x0000 is transparent black.
    const uint8_t* bitmap = gPack + gBitmapOffset;
    uint32_t stride = gTextureWidth / 8u;
    for (uint32_t y = 0; y < gUsedRows; y++) {
        const uint8_t* row = bitmap + (size_t) y * stride;
        for (uint32_t x = 0; x < gTextureWidth; x++) {
            if ((row[x >> 3] & (0x80u >> (x & 7u))) == 0u) continue;
            uint32_t tile = ((y >> 3) * (gTextureWidth >> 3) + (x >> 3)) * 64u + N3DSCjk_morton(x & 7u, y & 7u);
            pixels[tile] = 0xFFFFu; // R=31 G=31 B=31 A=1: the draw colour tints it, so white is the usable ink
        }
    }
    N3DSRenderer_flushImagePage(runner->renderer, tpag);
    gTexturePage = tpag;
    logInfo("AM2R: CJK font texture page is tpag %d (%ux%u)\n", (int) tpag, (unsigned) gTextureWidth, (unsigned) gTextureHeight);
    return true;
}

// The pack's strip whose font file is baseName at that pixel size, or NULL. Sizes are matched exactly: the glyph
// rectangles were laid out against the size the game asked for at bake time, so a different size would sit wrong.
static const N3DSCjkStrip* N3DSCjk_findStrip(const char* baseName, uint16_t size, N3DSCjkStrip* out) {
    size_t baseLength = strlen(baseName);
    for (uint32_t i = 0; i < gStripCount; i++) {
        const uint8_t* entry = gPack + gStripsOffset + (size_t) i * N3DS_CJK_STRIP_SIZE;
        const char* name = (const char*) entry;
        // The on-disk name is a 32-byte field: it only matches when it ends inside it.
        size_t nameLength = 0;
        while (nameLength < 32u && name[nameLength] != '\0') nameLength++;
        if (nameLength >= 32u || nameLength != baseLength || memcmp(name, baseName, nameLength) != 0) continue;
        if (N3DSCjk_readU16(entry + 32) != size) continue;
        out->size = size;
        out->yOffset = N3DSCjk_readU16(entry + 34);
        out->maxGlyphHeight = N3DSCjk_readU16(entry + 36);
        out->glyphCount = N3DSCjk_readU16(entry + 38);
        out->glyphOffset = N3DSCjk_readU32(entry + 40);
        if (out->glyphCount == 0u) return NULL;
        if (out->glyphOffset > gPackSize || (uint32_t) out->glyphCount * N3DS_CJK_GLYPH_SIZE > gPackSize - out->glyphOffset) return NULL;
        return out;
    }
    return NULL;
}

// The pixel font sits a little low in a line the game already laid out: item names inside a block crowded the heading
// above them. ascenderOffset is subtracted from the cursor, so a negative value moves the text up.
static void N3DSCjk_applyVerticalNudge(Font* font) {
    if (font == NULL || font->name == NULL) return;
    if (strcmp(font->name, "fontMenuTiny") != 0 && strcmp(font->name, "fontMenuTinyDefault") != 0) return;
    font->ascenderOffset = -2;
}

bool N3DSCjkFont_apply(Runner* runner, int32_t fontIndex, const char* ttfPath, float emSize) {
    if (runner == NULL || runner->dataWin == NULL || ttfPath == NULL) return false;
    DataWin* dw = runner->dataWin;
    if (fontIndex < 0 || (uint32_t) fontIndex >= dw->font.count) return false;
    if (!N3DSCjk_loadPack(runner, ttfPath)) return false;

    const char* baseName = strrchr(ttfPath, '/');
    baseName = baseName != NULL ? baseName + 1 : ttfPath;
    uint16_t requested = (uint16_t) (emSize > 0.0f ? emSize + 0.5f : 0u);
    N3DSCjkStrip strip;
    if (!N3DSCjk_findStrip(baseName, requested, &strip)) {
        logInfo("AM2R: the CJK font pack has no %s at %upx\n", baseName, (unsigned) requested);
        return false;
    }
    if (!N3DSCjk_createTexturePage(runner)) return false;

    Font* font = &dw->font.fonts[fontIndex];
    // The glyph array belongs to the data.win parser, and its teardown frees exactly this shape (see data_win.c): the
    // kerning arrays and then the array itself.
    for (uint32_t g = 0; g < font->glyphCount; g++) free(font->glyphs[g].kerning);
    free(font->glyphs);

    font->glyphs = (FontGlyph*) safeCalloc(strip.glyphCount, sizeof(FontGlyph));
    font->glyphCount = strip.glyphCount;
    for (uint32_t i = 0; i < strip.glyphCount; i++) {
        const uint8_t* record = gPack + strip.glyphOffset + (size_t) i * N3DS_CJK_GLYPH_SIZE;
        FontGlyph* glyph = &font->glyphs[i];
        glyph->character = N3DSCjk_readU16(record + 0);
        glyph->sourceX = N3DSCjk_readU16(record + 2);
        glyph->sourceY = N3DSCjk_readU16(record + 4);
        glyph->sourceWidth = N3DSCjk_readU16(record + 6);
        glyph->sourceHeight = N3DSCjk_readU16(record + 8);
        glyph->shift = N3DSCjk_readS16(record + 10);
        glyph->offset = N3DSCjk_readS16(record + 12);
        glyph->kerningCount = 0;
        glyph->kerning = NULL;
    }
    Font_buildGlyphLUT(font);

    // Lines advance by the tallest rectangle. The game's own fonts space their lines more generously, and where they
    // already cover the new glyphs the original stride is kept so the layout the game was designed around does not
    // move.
    if (strip.maxGlyphHeight > font->maxGlyphHeight) font->maxGlyphHeight = strip.maxGlyphHeight;
    font->tpagIndex = gTexturePage;
    N3DSCjk_applyVerticalNudge(font);
    N3DSRenderer_invalidateTextLayoutCache(runner->renderer);
    logInfo("AM2R: %s -> %s at %upx, %u glyphs (tpag %d)\n",
        font->name != NULL ? font->name : "font", baseName, (unsigned) requested, (unsigned) font->glyphCount,
        (int) gTexturePage);
    return true;
}
