// font_replace(font, ttf, size, bold, italic) for the 3DS front end.
//
// AM2R's data.win carries no CJK glyphs at all: the language packs make Chinese appear by swapping whole fonts at
// runtime, from a TTF, through this GameMaker built-in. Butterscotch never implemented it, so every Chinese string
// came out blank. This file implements it by rasterizing the TTF's own 1-bit bitmap strikes -- the packs ship pixel
// fonts, and outline rasterization would come out grey and too bold -- into one texture shared by every size.
//
// Why one shared 1024x1024 texture and not one per size: with two or more runtime atlases, particular strings stopped
// drawing while the atlas contents, page numbers, glyph counts and memory use all looked right. Every size therefore
// keeps its own glyph array and coordinates but shares the texture, its glyphs shifted down by its shelf's offset.

#include "n3ds_font_replace.h"

#include "n3ds_renderer.h"

#include "../log.h"
#include "../utils.h"

#include "n3ds_platform_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// citro3d only takes power-of-two textures between 8 and 1024, so the shared texture is 1024x1024. The sizes stack by
// the rows they actually used: rounding each up to a power of two does not fit (512+512+256+256 > 1024).
#define N3DS_FONT_SHARED_ATLAS_W 1024u
#define N3DS_FONT_SHARED_ATLAS_H 1024u

// A rasterized glyph: its 1-bit bitmap and where it sits relative to the pen. bearingY counts up from the baseline.
typedef struct {
    uint16_t codepoint;
    uint16_t width;
    uint16_t height;
    int16_t bearingX;
    int16_t bearingY;
    int16_t advance;
    uint8_t* bits; // width bits per row, no row padding (imageFormat 7 layout)
} N3DSFontGlyphBitmap;

// How a glyph is packed into the shared texture. The game stores each glyph as the ink extended up to the line's top
// -- the runner draws a glyph's rect with its bottom on the baseline (see the note on N3DSFontReplace_pack in
// docs/localization.md) -- so the packer reproduces exactly that shape.
typedef struct {
    uint16_t sourceX;
    uint16_t sourceY;
    uint16_t sourceWidth;  // the ink width: the game's rects are ink-tight horizontally
    uint16_t sourceHeight; // ink height + the blank rows above it, up to the line top
    int16_t shift;
    int16_t offset;
} N3DSFontPackedGlyph;

typedef struct {
    uint16_t* items;
    uint32_t count;
    uint32_t capacity;
    bool* seen;
} N3DSGlyphSet;

static char* gExtraText = NULL;

// The game asks for the same font file at the same size more than once: 04b09.ttf is requested for fontGUI and
// fontSubScr, and three of the five files are byte-identical copies of the 12px face. Rasterizing each request
// separately filled the shared texture (a strip is ~175 rows, so only five fitted). A strip is therefore keyed by the
// font file's contents and the requested size, and a repeat uses the shelf already built -- its glyph rectangles are
// copied, with the new font's own name and line stride.
typedef struct {
    uint32_t fileHash;
    uint32_t fileSize;
    int32_t emSize;
    int32_t shelfY;
    uint32_t stripHeight;
    uint32_t maxGlyphHeight;
    uint32_t glyphCount;
    FontGlyph* glyphs; // offsets within the strip; sourceY is rewritten per font from shelfY
} N3DSFontStrip;

#define N3DS_FONT_MAX_STRIPS 16
static N3DSFontStrip gStrips[N3DS_FONT_MAX_STRIPS];
static uint32_t gStripCount = 0;

// FNV-1a over the whole file: the strip cache must not hand a font the wrong glyphs, and these files are up to 1.6 MB
// (hashing one costs a few ms once, against re-rasterizing ~1300 glyphs).
static uint32_t N3DSFontReplace_hashFile(const uint8_t* d, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; i++) {
        hash ^= d[i];
        hash *= 16777619u;
    }
    return hash;
}

static const N3DSFontStrip* N3DSFontReplace_findStrip(uint32_t fileHash, uint32_t fileSize, int32_t emSize) {
    for (uint32_t i = 0; i < gStripCount; i++) {
        const N3DSFontStrip* strip = &gStrips[i];
        if (strip->fileHash == fileHash && strip->fileSize == fileSize && strip->emSize == emSize) return strip;
    }
    return NULL;
}

// ===[ Byte helpers ]===

static uint16_t N3DSFontReplace_be16(const uint8_t* p) {
    return (uint16_t) (((uint16_t) p[0] << 8) | (uint16_t) p[1]);
}

static uint32_t N3DSFontReplace_be32(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static int16_t N3DSFontReplace_s8(const uint8_t* p) {
    return (int16_t) (int8_t) p[0];
}

// Turns a path the game hands a built-in into one that opens on the 3DS. The game builds them from program_directory,
// which this port resolves through its FileSystem - but font_replace and background_replace read the file themselves,
// and the 3DS runs with a current directory that is not the game's folder, so a bare "lang/fonts/04b09.ttf" fails to
// open and every font slot stays as it was (no text anywhere). Anything already absolute is left alone.
char* N3DSFontReplace_resolvePath(const char* path) {
    if (path == NULL) return NULL;
    // Absolute: a leading '/', or a devoptab path such as "sdmc:/3ds/am2r/lang/...".
    const char* colon = strchr(path, ':');
    bool absolute = path[0] == '/' || (colon != NULL && colon != path && colon[1] == '/');
    if (absolute) {
        size_t len = strlen(path);
        char* copy = (char*) safeMalloc(len + 1u);
        memcpy(copy, path, len + 1u);
        return copy;
    }
    // Backslashes are not path separators here, but a language pack written on Windows may use them.
    size_t len = strlen(N3DS_SD_DIR) + strlen(path) + 1u;
    char* full = (char*) safeMalloc(len);
    size_t at = (size_t) snprintf(full, len, "%s%s", N3DS_SD_DIR, path);
    for (size_t i = strlen(N3DS_SD_DIR); i < at; i++) {
        if (full[i] == '\\') full[i] = '/';
    }
    return full;
}

static uint8_t* N3DSFontReplace_readFile(const char* path, size_t* outSize) {
    char* full = N3DSFontReplace_resolvePath(path);
    FILE* f = full != NULL ? fopen(full, "rb") : NULL;
    free(full);
    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t* data = (uint8_t*) safeMalloc((size_t) size + 1u);
    size_t got = fread(data, 1, (size_t) size, f);
    fclose(f);
    if (got != (size_t) size) {
        free(data);
        return NULL;
    }
    data[size] = '\0';
    *outSize = (size_t) size;
    return data;
}

// ===[ Glyph set ]===

static void N3DSFontReplace_glyphSetInit(N3DSGlyphSet* set) {
    memset(set, 0, sizeof(*set));
    set->seen = (bool*) safeCalloc(0x10000u, sizeof(bool));
}

static void N3DSFontReplace_addGlyph(N3DSGlyphSet* set, uint16_t codepoint) {
    if (codepoint == 0u || set->seen[codepoint]) return;
    set->seen[codepoint] = true;
    if (set->count >= set->capacity) {
        set->capacity = set->capacity > 0u ? set->capacity * 2u : 256u;
        set->items = (uint16_t*) safeRealloc(set->items, (size_t) set->capacity * sizeof(uint16_t));
    }
    set->items[set->count++] = codepoint;
}

static void N3DSFontReplace_addUtf8(N3DSGlyphSet* set, const char* text) {
    if (text == NULL) return;
    const uint8_t* p = (const uint8_t*) text;
    while (*p != '\0') {
        uint32_t cp;
        if (*p < 0x80u) {
            cp = *p++;
        } else if ((*p & 0xE0u) == 0xC0u && p[1] != '\0') {
            cp = ((uint32_t) (p[0] & 0x1Fu) << 6) | (uint32_t) (p[1] & 0x3Fu);
            p += 2;
        } else if ((*p & 0xF0u) == 0xE0u && p[1] != '\0' && p[2] != '\0') {
            cp = ((uint32_t) (p[0] & 0x0Fu) << 12) | ((uint32_t) (p[1] & 0x3Fu) << 6) | (uint32_t) (p[2] & 0x3Fu);
            p += 3;
        } else if ((*p & 0xF8u) == 0xF0u && p[1] != '\0' && p[2] != '\0' && p[3] != '\0') {
            p += 4; // outside the BMP; the runner's glyph characters are uint16_t
            continue;
        } else {
            p++;
            continue;
        }
        if (cp != 0u && cp < 0x10000u) N3DSFontReplace_addGlyph(set, (uint16_t) cp);
    }
}

void N3DSFontReplace_addExtraText(const char* text) {
    if (text == NULL) return;
    size_t add = strlen(text);
    size_t have = gExtraText != NULL ? strlen(gExtraText) : 0u;
    gExtraText = (char*) safeRealloc(gExtraText, have + add + 2u);
    memcpy(gExtraText + have, text, add);
    gExtraText[have + add] = '\n';
    gExtraText[have + add + 1u] = '\0';
}

void N3DSFontReplace_resetExtraText(void) {
    free(gExtraText);
    gExtraText = NULL;
}

// Only the characters a language file actually uses go into the atlas: rasterizing all of CJK would need far more
// than the 1024x1024 the shared texture has. GameMaker strings are the double-quoted runs, and the files carry a BOM.
static void N3DSFontReplace_addFileChars(N3DSGlyphSet* set, const char* path) {
    size_t size = 0;
    uint8_t* data = N3DSFontReplace_readFile(path, &size);
    if (data == NULL) return;
    size_t start = 0;
    if (size >= 3u && data[0] == 0xEFu && data[1] == 0xBBu && data[2] == 0xBFu) start = 3u;
    const char* p = (const char*) data + start;
    while (*p != '\0') {
        const char* open = strchr(p, '"');
        if (open == NULL) break;
        const char* close = strchr(open + 1, '"');
        if (close == NULL) break;
        size_t len = (size_t) (close - (open + 1));
        char* value = (char*) safeMalloc(len + 1u);
        memcpy(value, open + 1, len);
        value[len] = '\0';
        N3DSFontReplace_addUtf8(set, value);
        free(value);
        p = close + 1;
    }
    free(data);
}

// Every language lang/languages.txt lists, not just the one config.ini selects: switching the language then finds its
// characters already in the atlas, which is what makes the switch possible at all.
static void N3DSFontReplace_buildGlyphSet(N3DSGlyphSet* set) {
    for (uint16_t c = 32u; c < 127u; c++) N3DSFontReplace_addGlyph(set, c);
    if (gExtraText != NULL) N3DSFontReplace_addUtf8(set, gExtraText);

    FILE* f = fopen(N3DS_SD_DIR "lang/languages.txt", "r");
    if (f != NULL) {
        char line[256];
        while (fgets(line, sizeof(line), f) != NULL) {
            char* name = line;
            while (*name == ' ' || *name == '\t') name++;
            char* end = name + strlen(name);
            while (end > name && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
            *end = '\0';
            if (*name == '\0') continue;
            char path[512];
            snprintf(path, sizeof(path), N3DS_SD_DIR "lang/%s", name);
            N3DSFontReplace_addFileChars(set, path);
        }
        fclose(f);
    }
    logInfo("AM2R: font atlas covers %u characters\n", set->count);
}

// ===[ TTF: table lookup and cmap ]===

static bool N3DSFontReplace_findTable(const uint8_t* d, size_t size, const char* tag, uint32_t* outOffset, uint32_t* outLength) {
    if (size < 12u) return false;
    uint16_t count = N3DSFontReplace_be16(d + 4);
    for (uint16_t i = 0; i < count; i++) {
        size_t rec = 12u + (size_t) i * 16u;
        if (rec + 16u > size) return false;
        if (memcmp(d + rec, tag, 4) != 0) continue;
        uint32_t offset = N3DSFontReplace_be32(d + rec + 8);
        uint32_t length = N3DSFontReplace_be32(d + rec + 12);
        if ((size_t) offset + (size_t) length > size) return false;
        *outOffset = offset;
        *outLength = length;
        return true;
    }
    return false;
}

typedef struct {
    const uint8_t* segEnd;
    const uint8_t* segStart;
    const uint8_t* idDelta;
    const uint8_t* idRangeOffsetBase;
    uint16_t segCount;
} N3DSFontCmap4;

static bool N3DSFontReplace_cmap4(const uint8_t* d, size_t size, N3DSFontCmap4* out) {
    uint32_t cmapOff = 0, cmapLen = 0;
    if (!N3DSFontReplace_findTable(d, size, "cmap", &cmapOff, &cmapLen)) return false;
    uint16_t tables = N3DSFontReplace_be16(d + cmapOff + 2);
    for (uint16_t i = 0; i < tables; i++) {
        size_t rec = (size_t) cmapOff + 4u + (size_t) i * 8u;
        if (rec + 8u > size) return false;
        uint16_t platform = N3DSFontReplace_be16(d + rec);
        uint16_t encoding = N3DSFontReplace_be16(d + rec + 2);
        uint32_t subOff = cmapOff + N3DSFontReplace_be32(d + rec + 4);
        if (platform != 3u || encoding != 1u) continue;
        if ((size_t) subOff + 16u > size) return false;
        if (N3DSFontReplace_be16(d + subOff) != 4u) continue;
        uint16_t segX2 = N3DSFontReplace_be16(d + subOff + 6);
        if (segX2 == 0u) return false;
        uint16_t seg = (uint16_t) (segX2 / 2u);
        out->segCount = seg;
        out->segEnd = d + subOff + 14u;
        out->segStart = out->segEnd + segX2 + 2u;
        out->idDelta = out->segStart + segX2;
        out->idRangeOffsetBase = out->idDelta + segX2;
        return true;
    }
    return false;
}

static uint16_t N3DSFontReplace_glyphForCodepoint(const N3DSFontCmap4* cmap, uint16_t codepoint) {
    for (uint16_t i = 0; i < cmap->segCount; i++) {
        uint16_t end = N3DSFontReplace_be16(cmap->segEnd + (size_t) i * 2u);
        if (codepoint > end) continue;
        uint16_t start = N3DSFontReplace_be16(cmap->segStart + (size_t) i * 2u);
        if (codepoint < start) return 0u;
        uint16_t delta = N3DSFontReplace_be16(cmap->idDelta + (size_t) i * 2u);
        const uint8_t* rangeOffsetAt = cmap->idRangeOffsetBase + (size_t) i * 2u;
        uint16_t rangeOffset = N3DSFontReplace_be16(rangeOffsetAt);
        if (rangeOffset == 0u) return (uint16_t) (codepoint + delta);
        uint16_t glyph = N3DSFontReplace_be16(rangeOffsetAt + rangeOffset + (size_t) (codepoint - start) * 2u);
        if (glyph == 0u) return 0u;
        return (uint16_t) (glyph + delta);
    }
    return 0u;
}

// ===[ TTF: the bitmap strike ]===

// The chosen strike: where EBLC and EBDT are, and which bitmapSizeTable was picked.
typedef struct {
    uint32_t eblc;
    uint32_t ebdt;
    uint32_t sizeTable; // bitmapSizeTable of the chosen strike
    uint16_t ppem;
} N3DSFontStrike;

// The glyph entries a strike's index subtables describe, in a form the per-glyph lookup can use. indexFormat 1 and 3
// keep an offset and a size per glyph after the subtable header; indexFormat 2 keeps one size in the header and its
// metrics in the header too.
typedef struct {
    uint16_t firstGlyph;
    uint16_t lastGlyph;
    uint16_t indexFormat;
    uint16_t imageFormat;
    const uint8_t* table; // the subtable header
    const uint8_t* metrics; // bigGlyphMetrics for imageFormat 5 (inside the subtable), else NULL
    uint32_t imageDataOffset;
    uint32_t imageSize; // indexFormat 2: the fixed image size
} N3DSFontSubtable;

static bool N3DSFontReplace_findStrike(const uint8_t* d, size_t size, float emSize, N3DSFontStrike* out) {
    uint32_t eblcOff = 0, eblcLen = 0, ebdtOff = 0, ebdtLen = 0;
    if (!N3DSFontReplace_findTable(d, size, "EBLC", &eblcOff, &eblcLen)) return false;
    if (!N3DSFontReplace_findTable(d, size, "EBDT", &ebdtOff, &ebdtLen)) return false;
    uint32_t numSizes = N3DSFontReplace_be32(d + eblcOff + 4);
    if (numSizes == 0u) return false;

    // The closest strike, so a sizes.ini that drifts from the font still renders rather than failing.
    int32_t best = -1;
    uint32_t bestDistance = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < numSizes; i++) {
        size_t entry = (size_t) eblcOff + 8u + (size_t) i * 48u;
        if (entry + 48u > size) break;
        uint16_t ppemX = d[entry + 44u];
        uint32_t distance = ppemX > emSize ? (uint32_t) (ppemX - emSize) : (uint32_t) (emSize - ppemX);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = (int32_t) i;
        }
    }
    if (best < 0) return false;

    out->eblc = eblcOff;
    out->ebdt = ebdtOff;
    out->sizeTable = eblcOff + 8u + (uint32_t) best * 48u;
    out->ppem = d[out->sizeTable + 44u];
    return true;
}

// Fills out with the subtable covering glyphId. Each indexSubTableArray entry is 8 bytes (first, last, additional
// offset) and its additional offset is measured from the start of the array, not from the entry.
static bool N3DSFontReplace_findSubtable(const uint8_t* d, size_t size, const N3DSFontStrike* strike, uint32_t glyphId,
        N3DSFontSubtable* out) {
    uint32_t arrayOffset = N3DSFontReplace_be32(d + strike->sizeTable);
    uint32_t count = N3DSFontReplace_be32(d + strike->sizeTable + 8u);
    size_t array = (size_t) strike->eblc + arrayOffset;
    for (uint32_t i = 0; i < count; i++) {
        size_t entry = array + (size_t) i * 8u;
        if (entry + 8u > size) return false;
        uint16_t first = N3DSFontReplace_be16(d + entry);
        uint16_t last = N3DSFontReplace_be16(d + entry + 2);
        if (glyphId < first || glyphId > last) continue;
        uint32_t additional = N3DSFontReplace_be32(d + entry + 4);
        size_t header = array + additional;
        if (header + 8u > size) return false;
        memset(out, 0, sizeof(*out));
        out->firstGlyph = first;
        out->lastGlyph = last;
        out->indexFormat = N3DSFontReplace_be16(d + header);
        out->imageFormat = N3DSFontReplace_be16(d + header + 2);
        out->table = d + header;
        if (out->indexFormat == 1u || out->indexFormat == 3u) {
            out->imageDataOffset = N3DSFontReplace_be32(d + header + 4);
        } else if (out->indexFormat == 2u) {
            // Format 2 keeps the image size and the (big) metrics in the subtable header, right after the 8-byte
            // indexSubHeader: imageSize, then the metrics. The glyph data itself starts at imageDataOffset.
            out->imageSize = N3DSFontReplace_be32(d + header + 8u);
            out->imageDataOffset = N3DSFontReplace_be32(d + header + 4);
            if (out->imageFormat == 5u) out->metrics = d + header + 12u;
        } else {
            return false;
        }
        // A format with big metrics in the EBLC keeps them right after the indexSubHeader (so where the per-glyph
        // offset array would start for formats 1 and 3).
        if (out->imageFormat == 5u && out->metrics == NULL) out->metrics = d + header + 8u;
        return true;
    }
    return false;
}

// Reads one glyph's bitmap. imageFormat decides where the metrics are and whether rows are padded to bytes:
//   1, 2, 8, 9  smallGlyphMetrics (5 bytes) in the glyph entry; 1/2 pad rows, 8/9 do not
//   5           bigGlyphMetrics (8 bytes) in the EBLC subtable; rows packed
//   6           bigGlyphMetrics in the glyph entry, rows padded to bytes
//   7           bigGlyphMetrics in the glyph entry, rows packed, after 8 bytes of header
static bool N3DSFontReplace_readGlyph(const uint8_t* d, size_t size, const N3DSFontStrike* strike,
        const N3DSFontSubtable* sub, uint32_t glyphId, uint16_t codepoint, N3DSFontGlyphBitmap* out) {
    uint32_t imageFormat = sub->imageFormat;
    const uint8_t* entry;
    size_t entrySize;

    if (sub->indexFormat == 1u || sub->indexFormat == 3u) {
        // Formats 1 and 3 keep one offset per glyph PLUS a final one that ends the last glyph, and a glyph's size is
        // the gap to the next offset. Reading it as a separate offset/size pair shifts every glyph after the first.
        uint32_t n = (uint32_t) (sub->lastGlyph - sub->firstGlyph) + 1u;
        size_t offsets = (size_t) (sub->table - d) + 8u;
        uint32_t slot = glyphId - sub->firstGlyph;
        if (offsets + ((size_t) n + 1u) * 4u > size) return false;
        uint32_t offset = N3DSFontReplace_be32(d + offsets + (size_t) slot * 4u);
        uint32_t next = N3DSFontReplace_be32(d + offsets + ((size_t) slot + 1u) * 4u);
        if (next < offset) return false;
        size_t at = (size_t) strike->ebdt + sub->imageDataOffset + offset;
        size_t length = (size_t) (next - offset);
        if (at + length > size) return false;
        entry = d + at;
        entrySize = length;
    } else {
        // Format 2: every glyph's image is the same size, one after another.
        if (sub->imageSize == 0u) return false;
        size_t at = (size_t) strike->ebdt + sub->imageDataOffset + (size_t) sub->imageSize * (size_t) (glyphId - sub->firstGlyph);
        if (at + sub->imageSize > size) return false;
        entry = d + at;
        entrySize = sub->imageSize;
    }

    uint16_t height, width, advance;
    int16_t bearingX, bearingY;
    size_t dataOffset;
    bool byteAligned;

    if (imageFormat == 1u || imageFormat == 2u || imageFormat == 8u || imageFormat == 9u) {
        if (entrySize < 5u) return false;
        height = entry[0];
        width = entry[1];
        bearingX = N3DSFontReplace_s8(entry + 2);
        bearingY = N3DSFontReplace_s8(entry + 3);
        advance = entry[4];
        dataOffset = 5u;
        byteAligned = (imageFormat == 1u || imageFormat == 2u);
    } else if (imageFormat == 5u) {
        if (sub->metrics == NULL) return false;
        height = sub->metrics[0];
        width = sub->metrics[1];
        bearingX = N3DSFontReplace_s8(sub->metrics + 2);
        bearingY = N3DSFontReplace_s8(sub->metrics + 3);
        advance = sub->metrics[4];
        dataOffset = 0u;
        byteAligned = false;
    } else if (imageFormat == 6u || imageFormat == 7u) {
        if (entrySize < 8u) return false;
        height = entry[0];
        width = entry[1];
        bearingX = N3DSFontReplace_s8(entry + 2);
        bearingY = N3DSFontReplace_s8(entry + 3);
        advance = entry[4];
        dataOffset = 8u;
        byteAligned = (imageFormat == 6u);
    } else {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->codepoint = codepoint;
    out->width = width;
    out->height = height;
    out->bearingX = bearingX;
    out->bearingY = bearingY;
    out->advance = (int16_t) advance;
    if (width == 0u || height == 0u) return true; // a blank glyph, e.g. a space

    if (dataOffset >= entrySize) return false;
    size_t available = entrySize - dataOffset;
    // imageFormat 6 pads every row to a byte; 5 and 7 are one continuous bit stream (a row is width bits, not
    // width rounded up to a byte), which is why the expected size is ceil(width*height/8) and not stride*height.
    size_t strideBytes = (((size_t) width + 7u) / 8u);
    size_t needed = byteAligned ? strideBytes * (size_t) height : (((size_t) width * (size_t) height) + 7u) / 8u;
    if (needed > available) return false; // a truncated glyph: the rest of the file cannot be trusted

    // Unpack into one bit per pixel: the game samples glyph rects pixel by pixel, so a padded stride would leak the
    // padding into the neighbouring pixel.
    size_t outBytes = (((size_t) width * (size_t) height) + 7u) / 8u;
    out->bits = (uint8_t*) safeCalloc(outBytes, 1u);

    const uint8_t* src = entry + dataOffset;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t bit;
            if (byteAligned) {
                bit = (uint8_t) ((src[(size_t) y * strideBytes + (x >> 3)] >> (7u - (x & 7u))) & 1u);
            } else {
                size_t index = (size_t) y * (size_t) width + (size_t) x;
                bit = (uint8_t) ((src[index >> 3] >> (7u - (index & 7u))) & 1u);
            }
            if (bit != 0u) out->bits[((size_t) y * (size_t) width + (size_t) x) >> 3] |= (uint8_t) (0x80u >> (((size_t) y * (size_t) width + (size_t) x) & 7u));
        }
    }
    return true;
}

// ===[ Font building ]===

// Vertical nudge, in pixels, for the fonts where the pixel face sits a little high or low in a line the game already
// laid out. ascenderOffset is subtracted from the cursor, so a negative value moves the text down. The field is free
// here: it is only read from data.win at WAD 17 and this game is WAD 15, so it was always 0.
//
// This has to run on both paths out of buildAndInstall - the one that rasterizes a size and the one that reuses a
// strip another font already put in the texture - or a font that happens to load second silently keeps the old
// position.
static void N3DSFontReplace_applyVerticalNudge(Font* font) {
    if (font == NULL || font->name == NULL) return;
    static const struct { const char* fontName; int32_t pixels; } kVertical[] = {
        { "fontMenuTiny", -2 }, // item names crowded the block heading above them (5 was too far)
    };
    for (size_t i = 0; i < sizeof(kVertical) / sizeof(kVertical[0]); i++) {
        size_t length = strlen(kVertical[i].fontName);
        if (strncmp(font->name, kVertical[i].fontName, length) != 0) continue;
        const char* rest = font->name + length;
        if (*rest != '\0' && strcmp(rest, "Default") != 0) continue;
        font->ascenderOffset = kVertical[i].pixels;
        logInfo("AM2R: %s shifted %d px vertically\n", font->name, (int) kVertical[i].pixels);
        return;
    }
}

// Packs the glyphs of one size into a strip of the shared texture and writes the font's glyph array. Every glyph's
// rect is the ink plus the blank rows above it up to the line top, matching how the game's own fonts are packed:
// the runner draws a glyph at cursorY, which is the line top, so a rect that starts at the line top puts the ink
// exactly on the baseline.
static bool N3DSFontReplace_buildAndInstall(Runner* runner, int32_t fontIndex, const uint8_t* d, size_t size,
        const N3DSFontCmap4* cmap, const N3DSFontStrike* strike, const N3DSGlyphSet* set, float emSize, const char* ttfPath) {
    DataWin* dw = runner->dataWin;
    Font* font = &dw->font.fonts[fontIndex];
    int16_t requested = (int16_t) (emSize > 0.0f ? emSize + 0.5f : strike->ppem);
    uint32_t fileHash = N3DSFontReplace_hashFile(d, size);

    // This exact file at this exact size has already been rasterized into the shared texture: hand the font that
    // strip's glyphs instead of filling the atlas with a second copy of the same bitmaps.
    const N3DSFontStrip* cached = N3DSFontReplace_findStrip(fileHash, (uint32_t) size, requested);
    if (cached != NULL) {
        for (uint32_t g = 0; g < font->glyphCount; g++) free(font->glyphs[g].kerning);
        free(font->glyphs);
        font->glyphs = (FontGlyph*) safeCalloc(cached->glyphCount, sizeof(FontGlyph));
        font->glyphCount = cached->glyphCount;
        for (uint32_t i = 0; i < cached->glyphCount; i++) {
            font->glyphs[i] = cached->glyphs[i];
            font->glyphs[i].kerning = NULL;
            font->glyphs[i].kerningCount = 0;
            font->glyphs[i].sourceY = (uint16_t) (cached->glyphs[i].sourceY + (uint16_t) cached->shelfY);
        }
        Font_buildGlyphLUT(font);
        uint32_t originalStride = font->maxGlyphHeight;
        font->maxGlyphHeight = originalStride > cached->maxGlyphHeight ? originalStride : cached->maxGlyphHeight;
        font->tpagIndex = N3DSRenderer_sharedFontAtlasTPAG(runner->renderer);
        N3DSFontReplace_applyVerticalNudge(font);
        N3DSRenderer_invalidateTextLayoutCache(runner->renderer);
        logInfo("AM2R: %s -> %u glyphs at %dpx, reusing the strip at y=%d, stride %u\n",
            font->name != NULL ? font->name : "font", font->glyphCount, (int) requested, (int) cached->shelfY,
            font->maxGlyphHeight);
        return true;
    }

    // Rasterize every character the font has a bitmap for. The line's ascent is the size the pack was asked for: the
    // game's own fonts put a glyph's rectangle at the line top and the ink on the baseline, and the request is the
    // only thing that says where the baseline is (the TTF's own strike metrics describe the strike, not the layout).
    N3DSFontGlyphBitmap* bitmaps = NULL;
    uint32_t bitmapCount = 0;
    uint32_t bitmapCapacity = 0;
    int32_t ascent = requested;
    uint32_t missing = 0;

    for (uint32_t i = 0; i < set->count; i++) {
        uint16_t codepoint = set->items[i];
        uint16_t glyphId = N3DSFontReplace_glyphForCodepoint(cmap, codepoint);
        if (glyphId == 0u) {
            missing++;
            continue;
        }
        N3DSFontSubtable sub;
        if (!N3DSFontReplace_findSubtable(d, size, strike, glyphId, &sub)) {
            missing++;
            continue;
        }
        N3DSFontGlyphBitmap glyph;
        if (!N3DSFontReplace_readGlyph(d, size, strike, &sub, glyphId, codepoint, &glyph)) {
            missing++;
            continue;
        }
        // Ink taller than the requested line would be cut off at the top; the line has to cover it instead.
        if (glyph.height > 0u && (int32_t) glyph.bearingY > ascent) ascent = glyph.bearingY;
        if (bitmapCount >= bitmapCapacity) {
            bitmapCapacity = bitmapCapacity > 0u ? bitmapCapacity * 2u : 256u;
            bitmaps = (N3DSFontGlyphBitmap*) safeRealloc(bitmaps, (size_t) bitmapCapacity * sizeof(N3DSFontGlyphBitmap));
        }
        bitmaps[bitmapCount++] = glyph;
    }

    if (bitmapCount == 0u) {
        logError("AM2R: %s: no usable bitmaps for the language files (%u characters)\n", ttfPath, set->count);
        free(bitmaps);
        return false;
    }

    // The rects to pack, and the tallest one is what the line stride has to clear. The game advances lines by
    // maxGlyphHeight, so it must cover the deepest descender as well as the ascent.
    uint16_t* rectW = (uint16_t*) safeMalloc((size_t) bitmapCount * sizeof(uint16_t));
    uint16_t* rectH = (uint16_t*) safeMalloc((size_t) bitmapCount * sizeof(uint16_t));
    uint32_t* order = (uint32_t*) safeMalloc((size_t) bitmapCount * sizeof(uint32_t));
    uint32_t maxGlyphHeight = 0;
    for (uint32_t i = 0; i < bitmapCount; i++) {
        N3DSFontGlyphBitmap* g = &bitmaps[i];
        // The ink's left edge inside its rect. A negative bearing hangs to the left of the origin, so the rect grows
        // to the left; a positive one starts inside it. Ignoring the positive case put the ink hard against the rect's
        // left edge, which left its bearing as a gap on the right - the stray space after 「《」 and before 「》」.
        int32_t left = g->bearingX < 0 ? -(int32_t) g->bearingX : (int32_t) g->bearingX;
        rectW[i] = (uint16_t) (left + (int32_t) g->width);
        int32_t above = (int32_t) ascent - (int32_t) g->bearingY;
        rectH[i] = (uint16_t) (above + (int32_t) g->height);
        if (rectH[i] > maxGlyphHeight) maxGlyphHeight = rectH[i];
        order[i] = i;
    }

    // Shelf packing, tallest first, into 1024 columns. Sorting by height keeps the strips tight.
    for (uint32_t i = 1; i < bitmapCount; i++) {
        uint32_t key = order[i];
        uint32_t j = i;
        while (j > 0u && rectH[order[j - 1u]] < rectH[key]) {
            order[j] = order[j - 1u];
            j--;
        }
        order[j] = key;
    }

    uint32_t* placedX = (uint32_t*) safeCalloc(bitmapCount, sizeof(uint32_t));
    uint32_t* placedY = (uint32_t*) safeCalloc(bitmapCount, sizeof(uint32_t));
    uint32_t cursorX = 0, cursorY = 0, rowHeight = 0;
    for (uint32_t i = 0; i < bitmapCount; i++) {
        uint32_t slot = order[i];
        uint32_t w = rectW[slot], h = rectH[slot];
        if (w > N3DS_FONT_SHARED_ATLAS_W) {
            logError("AM2R: a glyph is %u px wide, wider than the atlas\n", w);
            free(rectW); free(rectH); free(order); free(placedX); free(placedY);
            for (uint32_t k = 0; k < bitmapCount; k++) free(bitmaps[k].bits);
            free(bitmaps);
            return false;
        }
        if (cursorX + w > N3DS_FONT_SHARED_ATLAS_W) {
            cursorX = 0;
            cursorY += rowHeight;
            rowHeight = 0;
        }
        placedX[slot] = cursorX;
        placedY[slot] = cursorY;
        cursorX += w;
        if (h > rowHeight) rowHeight = h;
    }
    uint32_t stripHeight = cursorY + rowHeight;

    // Where this size's shelf starts in the shared texture. The rows each size actually used are what gets stacked:
    // rounding each up to a power of two would not fit (512+512+256+256 > 1024).
    Renderer* renderer = runner->renderer;
    if (N3DSRenderer_sharedFontAtlasIndex(renderer) < 0 && N3DSRenderer_allocSharedFontAtlas(renderer) < 0) {
        logError("AM2R: no room for the font texture\n");
        free(rectW); free(rectH); free(order); free(placedX); free(placedY);
        for (uint32_t k = 0; k < bitmapCount; k++) free(bitmaps[k].bits);
        free(bitmaps);
        return false;
    }
    uint32_t nextY = N3DSRenderer_sharedFontAtlasNextY(renderer);
    if (nextY + stripHeight > N3DS_FONT_SHARED_ATLAS_H) {
        // Filling the shared texture would mean a second one, and a second one is what makes particular strings
        // vanish, so this size is dropped rather than silently breaking the sizes already in the texture.
        logError("AM2R: the font texture is full (%u of %u rows used, %u more wanted)\n",
            nextY, (unsigned) N3DS_FONT_SHARED_ATLAS_H, stripHeight);
        free(rectW); free(rectH); free(order); free(placedX); free(placedY);
        for (uint32_t k = 0; k < bitmapCount; k++) free(bitmaps[k].bits);
        free(bitmaps);
        return false;
    }
    int32_t shelfY = (int32_t) nextY;
    N3DSRenderer_setSharedFontAtlasNextY(renderer, nextY + stripHeight);

    // Compose this size's strip: a glyph's ink sits at the bottom of its rect, the blank rows above it.
    uint8_t* rgba = (uint8_t*) safeCalloc((size_t) N3DS_FONT_SHARED_ATLAS_W * (size_t) stripHeight * 4u, 1u);
    N3DSFontPackedGlyph* packed = (N3DSFontPackedGlyph*) safeCalloc(bitmapCount, sizeof(N3DSFontPackedGlyph));
    for (uint32_t i = 0; i < bitmapCount; i++) {
        N3DSFontGlyphBitmap* g = &bitmaps[i];
        N3DSFontPackedGlyph* p = &packed[i];
        // Same left edge the rect was sized with above, so the ink lands inside its own rect.
        int32_t left = g->bearingX < 0 ? -(int32_t) g->bearingX : (int32_t) g->bearingX;
        uint32_t above = (uint32_t) ((int32_t) ascent - (int32_t) g->bearingY);
        p->sourceX = (uint16_t) placedX[i];
        p->sourceY = (uint16_t) placedY[i];
        p->sourceWidth = rectW[i];
        p->sourceHeight = rectH[i];
        p->shift = g->advance;
        // The game's own fonts nudge a glyph right by one (offset=1 in the dumped data), which is where the pixel
        // font's left side bearing lands. The rect already covers the bearing, so that nudge is all that is left to
        // account for - a negative bearing grows the rect instead of shifting the ink.
        p->offset = (int16_t) (1 - (g->bearingX < 0 ? -(int32_t) g->bearingX : 0));
        if (g->width == 0u || g->height == 0u) continue;
        // The rect is line-top aligned, so the ink starts `above` rows down from the rect's own top, and the rect sits
        // at placedY: both offsets are needed, or every glyph lands on its shelf's first row.
        uint32_t inkTop = placedY[i] + above;
        for (uint32_t y = 0; y < g->height; y++) {
            uint32_t dstY = inkTop + y;
            if (dstY >= stripHeight) break;
            for (uint32_t x = 0; x < g->width; x++) {
                size_t bitIndex = (size_t) y * (size_t) g->width + (size_t) x;
                if (((g->bits[bitIndex >> 3] >> (7u - (bitIndex & 7u))) & 1u) == 0u) continue;
                uint32_t dstX = placedX[i] + (uint32_t) left + x;
                if (dstX >= N3DS_FONT_SHARED_ATLAS_W) continue;
                size_t at = ((size_t) dstY * (size_t) N3DS_FONT_SHARED_ATLAS_W + (size_t) dstX) * 4u;
                rgba[at + 0u] = 0xFFu; // glyphs are drawn with the current draw colour, so white is the usable ink
                rgba[at + 1u] = 0xFFu;
                rgba[at + 2u] = 0xFFu;
                rgba[at + 3u] = 0xFFu;
            }
        }
    }

    bool uploaded = N3DSRenderer_uploadFontAtlasRegion(renderer, rgba, 0u, (uint32_t) shelfY,
        N3DS_FONT_SHARED_ATLAS_W, stripHeight);
    free(rgba);
    free(rectW);
    free(rectH);
    free(order);
    free(placedX);
    free(placedY);
    for (uint32_t k = 0; k < bitmapCount; k++) free(bitmaps[k].bits);
    if (!uploaded) {
        logError("AM2R: could not upload the font texture\n");
        free(bitmaps);
        free(packed);
        return false;
    }

    // Hand the font the new glyphs. The kerning arrays and the glyph arrays themselves are the parser's to free, and
    // it frees exactly this shape (data_win.c), so replacing them here keeps the teardown correct.
    for (uint32_t g = 0; g < font->glyphCount; g++) free(font->glyphs[g].kerning);
    free(font->glyphs);
    font->glyphs = (FontGlyph*) safeCalloc(bitmapCount, sizeof(FontGlyph));
    font->glyphCount = bitmapCount;
    for (uint32_t i = 0; i < bitmapCount; i++) {
        FontGlyph* dst = &font->glyphs[i];
        const N3DSFontPackedGlyph* p = &packed[i];
        dst->character = bitmaps[i].codepoint;
        dst->sourceX = p->sourceX;
        // The size's shelf is transparent to the font: every rect is shifted down by it.
        dst->sourceY = (uint16_t) (p->sourceY + (uint16_t) shelfY);
        dst->sourceWidth = p->sourceWidth;
        dst->sourceHeight = p->sourceHeight;
        dst->shift = p->shift;
        dst->offset = p->offset;
        dst->kerningCount = 0;
        dst->kerning = NULL;
    }
    Font_buildGlyphLUT(font);
    // The line stride must clear the deepest rectangle or the ink would overlap the next line. The game's own fonts
    // space their lines generously, so wherever the original already covers the new glyphs it is kept and the layout
    // the game was designed around does not move.
    uint32_t originalStride = font->maxGlyphHeight;
    font->maxGlyphHeight = originalStride > maxGlyphHeight ? originalStride : maxGlyphHeight;
    // The bitmaps are drawn one pixel per pixel. The game asks for the tier it wants through sizes.ini and the closest
    // strike is used as it is: scaling a pixel font makes its strokes grey (and only 0.5 is even tolerable).
    font->tpagIndex = N3DSRenderer_sharedFontAtlasTPAG(renderer);
    N3DSFontReplace_applyVerticalNudge(font);

    // Remember the strip (in strip-local coordinates, so a later font can shift it by this shelf itself) so that the
    // same file at the same size does not get rasterized into the texture a second time.
    if (gStripCount < N3DS_FONT_MAX_STRIPS) {
        N3DSFontStrip* strip = &gStrips[gStripCount++];
        strip->fileHash = fileHash;
        strip->fileSize = (uint32_t) size;
        strip->emSize = requested;
        strip->shelfY = shelfY;
        strip->stripHeight = stripHeight;
        strip->maxGlyphHeight = maxGlyphHeight;
        strip->glyphCount = bitmapCount;
        strip->glyphs = (FontGlyph*) safeCalloc(bitmapCount, sizeof(FontGlyph));
        for (uint32_t i = 0; i < bitmapCount; i++) {
            strip->glyphs[i] = font->glyphs[i];
            strip->glyphs[i].sourceY = packed[i].sourceY; // strip-local, not the shelf-shifted value on the font
            strip->glyphs[i].kerning = NULL;
            strip->glyphs[i].kerningCount = 0;
        }
    }

    free(packed);
    free(bitmaps);
    logInfo("AM2R: %s -> %u glyphs at %dpx, strip y=%d+%u of %u, stride %u, %u missing\n",
        font->name != NULL ? font->name : "font", bitmapCount, (int) requested, (int) shelfY, stripHeight,
        (unsigned) N3DS_FONT_SHARED_ATLAS_H, font->maxGlyphHeight, missing);
    return true;
}


// Defined below, next to the entry point: the cache has to hash the same character set the normal path builds from.
static const N3DSGlyphSet* N3DSFontReplace_glyphSet(void);

// ===[ On-card cache of the built font atlas ]===
//
// Building the strips costs about 1.3 s of every boot: three bitmap strikes are decoded and packed out of ~3.9 MB of
// TTF, 1288 glyphs each. The result is deterministic for a given font file and size, and the game asks for the same six
// slots every time, so it is written to the card once and read back (about 1.5 MB, 0.15 s) on later boots.
//
// Version 2 adds the glyph's codepoint (FontGlyph.character): without it the rebuilt font cannot resolve any character,
// which is what made every shared-atlas string vanish in the first build that had this cache.
//
// The cache is keyed per strip by the font file's hash, its size and the requested pixel size, the same key
// findStrip uses, so a font that changed on the card simply misses and is rasterized as before - it is appended after
// the cached shelves and nothing already in the texture moves. A cache whose header or length does not match is
// ignored wholesale, which leaves the normal path untouched.
#define N3DS_FONT_CACHE_PATH N3DS_SD_DIR "fontcache.bin"
#define N3DS_FONT_CACHE_MAGIC 0x31434641u // "AFC1"
#define N3DS_FONT_CACHE_VERSION 2u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t usedRows;
    uint32_t paddedRows;
    uint32_t stripCount;
    uint32_t pixelBytes;
    // Which characters the strips were rasterized for. The set comes from the language files on the card, so adding or
    // changing one changes it; a cache built for another set would be missing glyphs.
    uint32_t glyphSetHash;
} N3DSFontCacheHeader;

static uint32_t N3DSFontReplace_glyphSetHash(const N3DSGlyphSet* set) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < set->count; i++) {
        hash = (hash ^ (uint32_t) set->items[i]) * 16777619u;
    }
    return hash;
}

typedef struct {
    uint32_t fileHash;
    uint32_t fileSize;
    int32_t emSize;
    int32_t shelfY;
    uint32_t stripHeight;
    uint32_t maxGlyphHeight;
    uint32_t glyphCount;
} N3DSFontCacheStrip;

// Set by loadCache: a boot that hit the cache only needs to write it again when a font it did not cover was built.
static bool gCacheLoaded = false;
static uint32_t gCacheLoadedStripCount = 0;

static bool writeBytes(FILE* f, const void* data, size_t size) {
    return size == 0u || fwrite(data, 1, size, f) == size;
}

bool N3DSFontReplace_saveCache(Runner* runner) {
    if (runner == NULL || runner->renderer == NULL || gStripCount == 0u) return false;
    if (gCacheLoaded && gStripCount == gCacheLoadedStripCount) return true; // the card already holds exactly this

    uint32_t usedRows = 0;
    const uint8_t* pixels = N3DSRenderer_sharedFontAtlasPixels(runner->renderer, &usedRows);
    if (pixels == NULL || usedRows == 0u) return false;
    uint32_t paddedRows = (usedRows + 7u) & ~7u;
    if (paddedRows > N3DS_FONT_SHARED_ATLAS_H) paddedRows = N3DS_FONT_SHARED_ATLAS_H;
    uint32_t pixelBytes = paddedRows * N3DS_FONT_SHARED_ATLAS_W * 4u;

    char tempPath[128];
    snprintf(tempPath, sizeof(tempPath), "%s.tmp", N3DS_FONT_CACHE_PATH);
    FILE* f = fopen(tempPath, "wb");
    if (f == NULL) return false;

    N3DSFontCacheHeader header = {
        .magic = N3DS_FONT_CACHE_MAGIC,
        .version = N3DS_FONT_CACHE_VERSION,
        .usedRows = usedRows,
        .paddedRows = paddedRows,
        .stripCount = gStripCount,
        .pixelBytes = pixelBytes,
        .glyphSetHash = N3DSFontReplace_glyphSetHash(N3DSFontReplace_glyphSet()),
    };
    bool ok = writeBytes(f, &header, sizeof(header));
    for (uint32_t i = 0; ok && i < gStripCount; i++) {
        const N3DSFontStrip* strip = &gStrips[i];
        N3DSFontCacheStrip record = {
            .fileHash = strip->fileHash,
            .fileSize = strip->fileSize,
            .emSize = strip->emSize,
            .shelfY = strip->shelfY,
            .stripHeight = strip->stripHeight,
            .maxGlyphHeight = strip->maxGlyphHeight,
            .glyphCount = strip->glyphCount,
        };
        ok = writeBytes(f, &record, sizeof(record));
        // The metrics only: kerning is never built for these fonts, and a pointer would be meaningless on the card.
        for (uint32_t g = 0; ok && g < strip->glyphCount; g++) {
            const FontGlyph* glyph = &strip->glyphs[g];
            uint16_t fields[7] = {
                glyph->character, glyph->sourceX, glyph->sourceY, glyph->sourceWidth, glyph->sourceHeight,
                (uint16_t) glyph->shift, (uint16_t) glyph->offset,
            };
            ok = writeBytes(f, fields, sizeof(fields));
        }
    }
    if (ok) ok = writeBytes(f, pixels, pixelBytes);
    if (fclose(f) != 0) ok = false;

    if (!ok) {
        remove(tempPath);
        logWarn("AM2R: could not write the font cache\n");
        return false;
    }
    remove(N3DS_FONT_CACHE_PATH);
    if (rename(tempPath, N3DS_FONT_CACHE_PATH) != 0) {
        remove(tempPath);
        return false;
    }
    logInfo("AM2R: font cache saved (%u strips, %u of %u rows, %u KB)\n",
        (unsigned) gStripCount, (unsigned) usedRows, (unsigned) N3DS_FONT_SHARED_ATLAS_H,
        (unsigned) (pixelBytes / 1024u));
    return true;
}

static bool readBytes(FILE* f, void* data, size_t size) {
    return size == 0u || fread(data, 1, size, f) == size;
}

bool N3DSFontReplace_loadCache(Runner* runner) {
    if (runner == NULL || runner->renderer == NULL) return false;
    FILE* f = fopen(N3DS_FONT_CACHE_PATH, "rb");
    if (f == NULL) return false;

    N3DSFontCacheHeader header;
    bool ok = readBytes(f, &header, sizeof(header)) &&
        header.magic == N3DS_FONT_CACHE_MAGIC && header.version == N3DS_FONT_CACHE_VERSION &&
        header.stripCount > 0u && header.stripCount <= N3DS_FONT_MAX_STRIPS &&
        header.usedRows > 0u && header.usedRows <= N3DS_FONT_SHARED_ATLAS_H &&
        header.paddedRows >= header.usedRows && header.paddedRows <= N3DS_FONT_SHARED_ATLAS_H &&
        header.pixelBytes == header.paddedRows * N3DS_FONT_SHARED_ATLAS_W * 4u &&
        header.glyphSetHash == N3DSFontReplace_glyphSetHash(N3DSFontReplace_glyphSet());

    N3DSFontStrip loaded[N3DS_FONT_MAX_STRIPS];
    memset(loaded, 0, sizeof(loaded));
    uint32_t loadedCount = 0;
    for (uint32_t i = 0; ok && i < header.stripCount; i++) {
        N3DSFontCacheStrip record;
        if (!readBytes(f, &record, sizeof(record))) { ok = false; break; }
        if (record.glyphCount == 0u || record.glyphCount > 0x4000u) { ok = false; break; }
        FontGlyph* glyphs = (FontGlyph*) safeCalloc(record.glyphCount, sizeof(FontGlyph));
        for (uint32_t g = 0; ok && g < record.glyphCount; g++) {
            uint16_t fields[7];
            if (!readBytes(f, fields, sizeof(fields))) { ok = false; break; }
            glyphs[g].character = fields[0];
            glyphs[g].sourceX = fields[1];
            glyphs[g].sourceY = fields[2];
            glyphs[g].sourceWidth = fields[3];
            glyphs[g].sourceHeight = fields[4];
            glyphs[g].shift = (int16_t) fields[5];
            glyphs[g].offset = (int16_t) fields[6];
        }
        if (!ok) { free(glyphs); break; }
        loaded[loadedCount].fileHash = record.fileHash;
        loaded[loadedCount].fileSize = record.fileSize;
        loaded[loadedCount].emSize = record.emSize;
        loaded[loadedCount].shelfY = record.shelfY;
        loaded[loadedCount].stripHeight = record.stripHeight;
        loaded[loadedCount].maxGlyphHeight = record.maxGlyphHeight;
        loaded[loadedCount].glyphCount = record.glyphCount;
        loaded[loadedCount].glyphs = glyphs;
        loadedCount++;
    }

    // Nothing in the file is trusted: a strip whose glyphs came back without their codepoints leaves every string
    // unable to resolve a character, which shows up as all of the shared-atlas text disappearing. Require a sane ASCII
    // set before the cache is allowed to replace the atlas.
    if (ok) {
        uint32_t withCharacter = 0;
        bool hasUpperA = false;
        for (uint32_t i = 0; i < loadedCount; i++) {
            for (uint32_t g = 0; g < loaded[i].glyphCount; g++) {
                uint16_t c = loaded[i].glyphs[g].character;
                if (c != 0u) withCharacter++;
                if (c == (uint16_t) 'A') hasUpperA = true;
            }
        }
        if (withCharacter < 64u || !hasUpperA) {
            logWarn("AM2R: font cache has no usable codepoints (%u glyphs); ignoring it\n", (unsigned) withCharacter);
            ok = false;
        }
    }

    uint8_t* pixels = NULL;
    if (ok) {
        pixels = (uint8_t*) safeMalloc(header.pixelBytes);
        ok = readBytes(f, pixels, header.pixelBytes);
    }
    fclose(f);

    if (!ok || !N3DSRenderer_restoreSharedFontAtlasPixels(runner->renderer, pixels, header.usedRows)) {
        for (uint32_t i = 0; i < loadedCount; i++) free(loaded[i].glyphs);
        free(pixels);
        return false;
    }
    free(pixels);

    for (uint32_t i = 0; i < loadedCount; i++) gStrips[i] = loaded[i];
    gStripCount = loadedCount;
    gCacheLoaded = true;
    gCacheLoadedStripCount = loadedCount;
    logInfo("AM2R: font cache hit (%u strips, %u of %u rows, saved %u KB of rasterizing)\n",
        (unsigned) loadedCount, (unsigned) header.usedRows, (unsigned) N3DS_FONT_SHARED_ATLAS_H,
        (unsigned) (header.pixelBytes / 1024u));
    return true;
}

// ===[ Entry point ]===


// The character set is the same for every call, and reading the language files costs a few hundred KB of I/O each
// time (the game calls this six times in a row), so it is built once and kept.
static N3DSGlyphSet gGlyphSet;
static bool gGlyphSetReady = false;

static const N3DSGlyphSet* N3DSFontReplace_glyphSet(void) {
    if (!gGlyphSetReady) {
        N3DSFontReplace_glyphSetInit(&gGlyphSet);
        N3DSFontReplace_buildGlyphSet(&gGlyphSet);
        gGlyphSetReady = true;
    }
    return &gGlyphSet;
}

bool N3DSFontReplace_apply(Runner* runner, int32_t fontIndex, const char* ttfPath, float emSize, bool bold, bool italic) {
    if (runner == NULL || runner->dataWin == NULL || runner->renderer == NULL) return false;
    DataWin* dw = runner->dataWin;
    if (fontIndex < 0 || (uint32_t) fontIndex >= dw->font.count) return false;
    Font* font = &dw->font.fonts[fontIndex];
    if (!font->present) return false;

    (void) bold;
    (void) italic; // a pixel font has one face; the game's own bold is just a thicker outline

    const float sizeFromIni = emSize; // what sizes.ini asked for, for the log when it is overridden below

    // A pixel font carries exactly one bitmap strike, so the size a font is asked for has to match the strike of the
    // file it reads. 04b09.ttf only has 12px, and the game asks it for both fontGUI (12px) and fontSubScr (8px); the
    // 8px request got the 12px strike, which is what made the panel headings look like a smear. The size is pinned
    // here rather than left to sizes.ini: that file is SD data the player has to keep in step with the build, and a
    // stale copy silently brought the old sizes back.
    const char* path = ttfPath;
    if (font->name != NULL) {
        static const struct { const char* fontName; const char* path; float emSize; } kOverrides[] = {
            { "fontSubScr", "lang/fonts/uni_05_53.ttf", 10.0f },   // panel headings (装备/光束/综合能力)
            { "fontMenuTiny", "lang/fonts/uni_05_53.ttf", 10.0f }, // item names, which drew too small at 8px
            // uni_05_53.ttf carries a single 10x10 bitmap strike, so asking it for 12px silently falls back to the 10px
            // strike and only widens the advance: the glyphs themselves stay 10px. 04b09.ttf carries a real 12x12 strike
            // and covers the same Chinese characters, and fontGUI2 already built that strip, so this reuses it.
            { "fontMenuSmall", "lang/fonts/04b09.ttf", 12.0f }, // file-select slot names, a real step up from 10px
        };
        for (size_t i = 0; i < sizeof(kOverrides) / sizeof(kOverrides[0]); i++) {
            size_t length = strlen(kOverrides[i].fontName);
            if (strncmp(font->name, kOverrides[i].fontName, length) != 0) continue;
            const char* rest = font->name + length;
            if (*rest != '\0' && strcmp(rest, "Default") != 0) continue;
            path = kOverrides[i].path;
            emSize = kOverrides[i].emSize;
            logInfo("AM2R: %s loads %s at %gpx (the game asked for %gpx from %s)\n",
                font->name, path, (double) emSize, (double) sizeFromIni, ttfPath);
            break;
        }
    }

    size_t size = 0;
    uint8_t* d = N3DSFontReplace_readFile(path, &size);
    if (d == NULL) {
        logError("AM2R: cannot read font %s\n", path);
        return false;
    }

    N3DSFontCmap4 cmap;
    N3DSFontStrike strike;
    if (!N3DSFontReplace_cmap4(d, size, &cmap) || !N3DSFontReplace_findStrike(d, size, emSize, &strike)) {
        logError("AM2R: %s has no usable bitmap strike (an outline font cannot be used)\n", path);
        free(d);
        return false;
    }

    const N3DSGlyphSet* set = N3DSFontReplace_glyphSet();
    bool ok = N3DSFontReplace_buildAndInstall(runner, fontIndex, d, size, &cmap, &strike, set, emSize, path);
    if (ok) {
        // A swapped font has a whole new glyph array; a layout cached before the swap still holds the old rectangles,
        // so strings laid out during the init room (before oControl replaced the fonts) kept drawing nothing.
        N3DSRenderer_invalidateTextLayoutCache(runner->renderer);
    }

    free(d); // the file is not kept: 4-16 MB of it stayed resident once and the game ran out of heap
    return ok;
}
