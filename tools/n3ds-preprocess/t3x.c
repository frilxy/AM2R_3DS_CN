// Tex3DS writer. Ported from devkitPro tex3ds 2.3 (https://github.com/devkitPro/tex3ds), GPLv3-or-later,
// Copyright (c) 2017-2021 Michael Theall: atlas.cpp (packer), encode.cpp (pixel formats), swizzle.cpp, quantum.h
// and the header writer in tex3ds.cpp. ETC1 blocks come from rg_etc1, the same encoder tex3ds uses.
//
// tex3ds runs on ImageMagick (Q16 HDRI). What that means for 8-bit input, and what is reproduced here:
// - Every image is composited over a transparent canvas, so fully transparent pixels come out as (0, 0, 0, 0).
// - quantum_to_bits<n>(257 * v) is v >> (8 - n) for every 8-bit v.
// - Luminance (L4/LA4) is BT.709 on linearised sRGB in double, stored as a float quantum, then truncated.
// - The atlas packer sorts its inputs with std::sort, which isn't stable; equal-sized frames end up in libstdc++'s
//   order, so that sort is reproduced too (stdSortImages) to give the same layout.

#include "t3x.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void t3xEtc1Init(void);
void t3xEtc1PackBlock(uint8_t out[8], const uint8_t rgba[64]);

// ===[ Byte buffer ]===

typedef struct {
    uint8_t* data;
    size_t size;
    size_t capacity;
} ByteBuffer;

static bool bufferReserve(ByteBuffer* buffer, size_t extra) {
    if (buffer->size + extra <= buffer->capacity) return true;
    size_t capacity = buffer->capacity != 0 ? buffer->capacity : 256;
    while (capacity < buffer->size + extra) capacity *= 2;
    uint8_t* data = realloc(buffer->data, capacity);
    if (data == NULL) return false;
    buffer->data = data;
    buffer->capacity = capacity;
    return true;
}

static void bufferPut8(ByteBuffer* buffer, uint8_t value) {
    buffer->data[buffer->size++] = value;
}

static void bufferPut16(ByteBuffer* buffer, uint16_t value) {
    bufferPut8(buffer, (uint8_t) value);
    bufferPut8(buffer, (uint8_t) (value >> 8));
}

// Texture coordinates are stored as 6.10 fixed point (tex3ds encode<float>).
static void bufferPutCoord(ByteBuffer* buffer, float value) {
    bufferPut16(buffer, (uint16_t) (value * 1024.0f));
}

// ===[ Images ]===

typedef struct {
    uint8_t* rgba;
    uint32_t width;
    uint32_t height;
} Canvas;

static bool canvasCreate(Canvas* canvas, uint32_t width, uint32_t height) {
    canvas->width = width;
    canvas->height = height;
    canvas->rgba = calloc((size_t) width * height, 4);
    return canvas->rgba != NULL;
}

// Over a transparent canvas: visible pixels keep their colour, transparent ones become (0, 0, 0, 0).
static void canvasDraw(Canvas* canvas, const T3xImage* image, uint32_t x, uint32_t y, bool rotated) {
    uint32_t width = rotated ? image->height : image->width;
    uint32_t height = rotated ? image->width : image->height;
    for (uint32_t row = 0; row < height; row++) {
        uint8_t* dst = canvas->rgba + (((size_t) (y + row) * canvas->width) + x) * 4;
        for (uint32_t col = 0; col < width; col++, dst += 4) {
            // rotate(-90) is a quarter turn anticlockwise: (col, row) comes from (width - 1 - row, col) of the source.
            uint32_t srcX = rotated ? image->width - 1 - row : col;
            uint32_t srcY = rotated ? col : row;
            const uint8_t* src = image->rgba + (((size_t) srcY * image->width) + srcX) * 4;
            if (src[3] == 0) continue;
            memcpy(dst, src, 4);
        }
    }
}

static uint32_t potCeil(uint32_t value) {
    if (value < 8) return 8;
    uint32_t pot = 8;
    while (pot < value) pot *= 2;
    return pot;
}

// ===[ Pixel formats ]===

static double gammaInverse(double v) {
    if (v <= 0.04045) return v / 12.92;
    return pow((v + 0.055) / 1.055, 2.4);
}

static double gammaForward(double v) {
    if (v <= 0.0031308) return v * 12.92;
    return 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

// quantum.h luminance() then quantum_to_bits<bits>, with ImageMagick's float quantum.
static uint8_t luminanceBits(const uint8_t* px, int bits) {
    const float quantumRange = 65535.0f;
    double r = (double) (float) (px[0] * 257) / quantumRange;
    double g = (double) (float) (px[1] * 257) / quantumRange;
    double b = (double) (float) (px[2] * 257) / quantumRange;
    double v = gammaForward(0.212655 * gammaInverse(r) + 0.715158 * gammaInverse(g) + 0.072187 * gammaInverse(b));
    if (v > 1.0) v = 1.0;
    if (v < 0.0) v = 0.0;
    float quantum = (float) (v * quantumRange);
    return (uint8_t) ((float) (1 << bits) * quantum / (quantumRange + 1.0f));
}

static void swizzleTile(uint8_t tile[64][4]) {
    static const unsigned char table[][4] = {
        {2, 8, 16, 4},     {3, 9, 17, 5},     {6, 10, 24, 20},   {7, 11, 25, 21},
        {14, 26, 28, 22},  {15, 27, 29, 23},  {34, 40, 48, 36},  {35, 41, 49, 37},
        {38, 42, 56, 52},  {39, 43, 57, 53},  {46, 58, 60, 54},  {47, 59, 61, 55},
    };
    static const unsigned char pairs[][2] = {{12, 18}, {13, 19}, {44, 50}, {45, 51}};
    uint8_t tmp[4];
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        memcpy(tmp, tile[table[i][0]], 4);
        memcpy(tile[table[i][0]], tile[table[i][1]], 4);
        memcpy(tile[table[i][1]], tile[table[i][2]], 4);
        memcpy(tile[table[i][2]], tile[table[i][3]], 4);
        memcpy(tile[table[i][3]], tmp, 4);
    }
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        memcpy(tmp, tile[pairs[i][0]], 4);
        memcpy(tile[pairs[i][0]], tile[pairs[i][1]], 4);
        memcpy(tile[pairs[i][1]], tmp, 4);
    }
}

static size_t bytesPerTile(T3xFormat format) {
    switch (format) {
        case T3X_RGBA8: return 256;
        case T3X_RGB8: return 192;
        case T3X_RGBA5551: case T3X_RGB565: case T3X_RGBA4: case T3X_LA8: case T3X_HILO8: return 128;
        case T3X_L8: case T3X_A8: case T3X_LA4: case T3X_ETC1A4: return 64;
        case T3X_L4: case T3X_A4: case T3X_ETC1: return 32;
    }
    return 0;
}

static void encodeEtc1Tile(uint8_t tile[64][4], bool alpha, ByteBuffer* out) {
    for (int j = 0; j < 8; j += 4) {
        for (int i = 0; i < 8; i += 4) {
            uint8_t block[64];
            uint8_t alphaBlock[8] = {0};
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    const uint8_t* px = tile[(j + y) * 8 + i + x];
                    block[y * 16 + x * 4 + 0] = px[0];
                    block[y * 16 + x * 4 + 1] = px[1];
                    block[y * 16 + x * 4 + 2] = px[2];
                    block[y * 16 + x * 4 + 3] = 0xFF;
                    // 4 bpp alpha, X and Y swapped.
                    if (alpha) alphaBlock[2 * x + y / 2] |= (uint8_t) ((px[3] >> 4) << ((y & 1) ? 4 : 0));
                }
            }
            uint8_t etc1[8];
            t3xEtc1PackBlock(etc1, block);
            if (alpha) {
                for (int k = 0; k < 8; k++) bufferPut8(out, alphaBlock[k]);
            }
            // rg_etc1 writes big-endian.
            for (int k = 0; k < 8; k++) bufferPut8(out, etc1[7 - k]);
        }
    }
}

static void encodeTile(uint8_t tile[64][4], T3xFormat format, ByteBuffer* out) {
    if (format == T3X_ETC1 || format == T3X_ETC1A4) {
        encodeEtc1Tile(tile, format == T3X_ETC1A4, out);
        return;
    }
    swizzleTile(tile);
    for (int k = 0; k < 64; k++) {
        const uint8_t* px = tile[k];
        switch (format) {
            case T3X_RGBA8:
                bufferPut8(out, px[3]);
                bufferPut8(out, px[2]);
                bufferPut8(out, px[1]);
                bufferPut8(out, px[0]);
                break;
            case T3X_RGB8:
                bufferPut8(out, px[2]);
                bufferPut8(out, px[1]);
                bufferPut8(out, px[0]);
                break;
            case T3X_RGBA5551:
                bufferPut16(out, (uint16_t) (((px[0] >> 3) << 11) | ((px[1] >> 3) << 6) | ((px[2] >> 3) << 1) | (px[3] >> 7)));
                break;
            case T3X_RGB565:
                bufferPut16(out, (uint16_t) (((px[0] >> 3) << 11) | ((px[1] >> 2) << 5) | (px[2] >> 3)));
                break;
            case T3X_RGBA4:
                bufferPut16(out, (uint16_t) (((px[0] >> 4) << 12) | ((px[1] >> 4) << 8) | ((px[2] >> 4) << 4) | (px[3] >> 4)));
                break;
            case T3X_LA8:
                bufferPut8(out, px[3]);
                bufferPut8(out, luminanceBits(px, 8));
                break;
            case T3X_HILO8:
                bufferPut8(out, px[1]);
                bufferPut8(out, px[0]);
                break;
            case T3X_L8:
                bufferPut8(out, luminanceBits(px, 8));
                break;
            case T3X_A8:
                bufferPut8(out, px[3]);
                break;
            case T3X_LA4:
                bufferPut8(out, (uint8_t) ((luminanceBits(px, 4) << 4) | (px[3] >> 4)));
                break;
            case T3X_L4:
                bufferPut8(out, (uint8_t) ((luminanceBits(tile[k + 1], 4) << 4) | luminanceBits(px, 4)));
                k++;
                break;
            case T3X_A4:
                bufferPut8(out, (uint8_t) ((tile[k + 1][3] >> 4) << 4 | (px[3] >> 4)));
                k++;
                break;
            case T3X_ETC1:
            case T3X_ETC1A4:
                break;
        }
    }
}

// ===[ File layout ]===

typedef struct {
    size_t index;
    float left, top, right, bottom;
    bool rotated;
} SubImage;

static bool writeT3x(const Canvas* canvas, T3xFormat format, const SubImage* subs, size_t subCount, T3xBuffer* out) {
    static bool etc1Ready = false;
    if ((format == T3X_ETC1 || format == T3X_ETC1A4) && !etc1Ready) {
        t3xEtc1Init();
        etc1Ready = true;
    }

    uint32_t width = canvas->width;
    uint32_t height = canvas->height;
    size_t dataSize = (size_t) (width / 8) * (height / 8) * bytesPerTile(format);
    ByteBuffer buffer = {0};
    if (!bufferReserve(&buffer, 5 + subCount * 12 + 8 + dataSize + 4)) return false;

    int log2W = 0, log2H = 0;
    while ((1u << log2W) < width) log2W++;
    while ((1u << log2H) < height) log2H++;

    bufferPut16(&buffer, (uint16_t) subCount);
    bufferPut8(&buffer, (uint8_t) ((log2W - 3) | ((log2H - 3) << 3)));
    bufferPut8(&buffer, (uint8_t) format);
    bufferPut8(&buffer, 0); // mipmaps
    for (size_t i = 0; i < subCount; i++) {
        const SubImage* sub = &subs[i];
        uint16_t subW, subH;
        if (sub->rotated) {
            subH = (uint16_t) ((sub->bottom - sub->top) * (float) width);
            subW = (uint16_t) ((sub->right - sub->left) * (float) height);
        } else {
            subW = (uint16_t) ((sub->right - sub->left) * (float) width);
            subH = (uint16_t) ((sub->top - sub->bottom) * (float) height);
        }
        bufferPut16(&buffer, subW);
        bufferPut16(&buffer, subH);
        bufferPutCoord(&buffer, sub->left);
        bufferPutCoord(&buffer, sub->top);
        bufferPutCoord(&buffer, sub->right);
        bufferPutCoord(&buffer, sub->bottom);
    }

    // "-z none": the 4-byte compression header (type 0, 24-bit size), then the data padded to 4 bytes.
    size_t compressedStart = buffer.size;
    bufferPut8(&buffer, 0x00);
    bufferPut8(&buffer, (uint8_t) dataSize);
    bufferPut8(&buffer, (uint8_t) (dataSize >> 8));
    bufferPut8(&buffer, (uint8_t) (dataSize >> 16));

    uint8_t tile[64][4];
    for (uint32_t ty = 0; ty < height; ty += 8) {
        for (uint32_t tx = 0; tx < width; tx += 8) {
            for (int y = 0; y < 8; y++) memcpy(tile[y * 8], canvas->rgba + (((size_t) (ty + y) * width) + tx) * 4, 32);
            encodeTile(tile, format, &buffer);
        }
    }
    while ((buffer.size - compressedStart) & 3) bufferPut8(&buffer, 0);

    out->data = buffer.data;
    out->size = buffer.size;
    return true;
}

bool t3xEncodeImage(const T3xImage* image, T3xFormat format, T3xBuffer* out) {
    if (image->width == 0 || image->height == 0 || image->width > 1024 || image->height > 1024) return false;
    Canvas canvas;
    if (!canvasCreate(&canvas, potCeil(image->width), potCeil(image->height))) return false;
    canvasDraw(&canvas, image, 0, 0, false);

    SubImage sub = {
        .index = 0,
        .left = 0.0f / (float) canvas.width,
        .top = 1.0f - 0.0f / (float) canvas.height,
        .right = (float) image->width / (float) canvas.width,
        .bottom = 1.0f - (float) image->height / (float) canvas.height,
        .rotated = false,
    };
    bool ok = writeT3x(&canvas, format, &sub, 1, out);
    free(canvas.rgba);
    return ok;
}

// ===[ Atlas packer (atlas.cpp) ]===

typedef struct {
    size_t index;
    const T3xImage* image;
} AtlasInput;

typedef struct {
    size_t index;
    const T3xImage* image;
    size_t x, y, w, h;
    bool rotated;
} Block;

typedef struct {
    size_t x, y;
} XY;

typedef struct {
    size_t width, height;
    Block* placed; // ordered by (x, y), like std::set<Block>
    size_t placedCount;
    XY* free; // ordered by (x, y), like std::set<XY>
    size_t freeCount;
} Packer;

static bool areaSizeLess(size_t w1, size_t h1, size_t w2, size_t h2) {
    size_t area1 = w1 * h1;
    size_t area2 = w2 * h2;
    if (area1 != area2) return area1 < area2;
    size_t max1 = w1 > h1 ? w1 : h1;
    size_t max2 = w2 > h2 ? w2 : h2;
    if (max1 == max2) return w1 < w2;
    return max1 < max2;
}

static bool inputLess(const AtlasInput* a, const AtlasInput* b) {
    return areaSizeLess(a->image->width, a->image->height, b->image->width, b->image->height);
}

// libstdc++'s std::sort (introsort, median-of-three pivot, heapsort fallback, final insertion sort), so equal
// elements land where they land in tex3ds.
static void swapInputs(AtlasInput* a, AtlasInput* b) {
    AtlasInput tmp = *a;
    *a = *b;
    *b = tmp;
}

static void adjustHeap(AtlasInput* first, ptrdiff_t hole, ptrdiff_t len, AtlasInput value) {
    const ptrdiff_t top = hole;
    ptrdiff_t child = hole;
    while (child < (len - 1) / 2) {
        child = 2 * (child + 1);
        if (inputLess(&first[child], &first[child - 1])) child--;
        first[hole] = first[child];
        hole = child;
    }
    if ((len & 1) == 0 && child == (len - 2) / 2) {
        child = 2 * (child + 1);
        first[hole] = first[child - 1];
        hole = child - 1;
    }
    ptrdiff_t parent = (hole - 1) / 2;
    while (hole > top && inputLess(&first[parent], &value)) {
        first[hole] = first[parent];
        hole = parent;
        parent = (hole - 1) / 2;
    }
    first[hole] = value;
}

static void heapSort(AtlasInput* first, AtlasInput* last) {
    ptrdiff_t len = last - first;
    if (len < 2) return;
    for (ptrdiff_t parent = (len - 2) / 2;; parent--) {
        adjustHeap(first, parent, len, first[parent]);
        if (parent == 0) break;
    }
    while (last - first > 1) {
        last--;
        AtlasInput value = *last;
        *last = *first;
        adjustHeap(first, 0, last - first, value);
    }
}

static void moveMedianToFirst(AtlasInput* result, AtlasInput* a, AtlasInput* b, AtlasInput* c) {
    if (inputLess(a, b)) {
        if (inputLess(b, c)) swapInputs(result, b);
        else if (inputLess(a, c)) swapInputs(result, c);
        else swapInputs(result, a);
    } else if (inputLess(a, c)) {
        swapInputs(result, a);
    } else if (inputLess(b, c)) {
        swapInputs(result, c);
    } else {
        swapInputs(result, b);
    }
}

static AtlasInput* unguardedPartition(AtlasInput* first, AtlasInput* last, AtlasInput* pivot) {
    for (;;) {
        while (inputLess(first, pivot)) first++;
        last--;
        while (inputLess(pivot, last)) last--;
        if (!(first < last)) return first;
        swapInputs(first, last);
        first++;
    }
}

static void introsortLoop(AtlasInput* first, AtlasInput* last, int depthLimit) {
    while (last - first > 16) {
        if (depthLimit == 0) {
            heapSort(first, last);
            return;
        }
        depthLimit--;
        AtlasInput* mid = first + (last - first) / 2;
        moveMedianToFirst(first, first + 1, mid, last - 1);
        AtlasInput* cut = unguardedPartition(first + 1, last, first);
        introsortLoop(cut, last, depthLimit);
        last = cut;
    }
}

static void unguardedLinearInsert(AtlasInput* last) {
    AtlasInput value = *last;
    AtlasInput* next = last - 1;
    while (inputLess(&value, next)) {
        *last = *next;
        last = next;
        next--;
    }
    *last = value;
}

static void insertionSort(AtlasInput* first, AtlasInput* last) {
    if (first == last) return;
    for (AtlasInput* i = first + 1; i != last; i++) {
        if (inputLess(i, first)) {
            AtlasInput value = *i;
            memmove(first + 1, first, (size_t) (i - first) * sizeof(*first));
            *first = value;
        } else {
            unguardedLinearInsert(i);
        }
    }
}

static void stdSortInputs(AtlasInput* first, AtlasInput* last) {
    ptrdiff_t n = last - first;
    if (n == 0) return;
    int lg = 0;
    while (((ptrdiff_t) 1 << (lg + 1)) <= n) lg++;
    introsortLoop(first, last, 2 * lg);
    if (n > 16) {
        insertionSort(first, first + 16);
        for (AtlasInput* i = first + 16; i != last; i++) unguardedLinearInsert(i);
    } else {
        insertionSort(first, last);
    }
}

static bool intersectsPlaced(const Packer* packer, size_t x, size_t y) {
    for (size_t i = 0; i < packer->placedCount; i++) {
        const Block* block = &packer->placed[i];
        if (x >= block->x && x < block->x + block->w && y >= block->y && y < block->y + block->h) return true;
    }
    return false;
}

static bool xyLess(size_t ax, size_t ay, size_t bx, size_t by) {
    return ax != bx ? ax < bx : ay < by;
}

static void insertFree(Packer* packer, size_t x, size_t y) {
    size_t pos = 0;
    while (pos < packer->freeCount && xyLess(packer->free[pos].x, packer->free[pos].y, x, y)) pos++;
    if (pos < packer->freeCount && packer->free[pos].x == x && packer->free[pos].y == y) return;
    memmove(&packer->free[pos + 1], &packer->free[pos], (packer->freeCount - pos) * sizeof(XY));
    packer->free[pos] = (XY) {x, y};
    packer->freeCount++;
}

static void insertPlaced(Packer* packer, const Block* block) {
    size_t pos = 0;
    while (pos < packer->placedCount && xyLess(packer->placed[pos].x, packer->placed[pos].y, block->x, block->y)) pos++;
    memmove(&packer->placed[pos + 1], &packer->placed[pos], (packer->placedCount - pos) * sizeof(Block));
    packer->placed[pos] = *block;
    packer->placedCount++;
}

static void addFree(Packer* packer, size_t x, size_t y) {
    if (x >= packer->width || y >= packer->height) return;
    if (intersectsPlaced(packer, x, y)) return;
    insertFree(packer, x, y);
}

static void pack(const Packer* packer, size_t* x, size_t* y) {
    bool left = *x == 0 || intersectsPlaced(packer, *x - 1, *y);
    bool up = *y == 0 || intersectsPlaced(packer, *x, *y - 1);
    if (!left && !up) abort(); // should be adjacent to a placed block
    if (left && up) return;
    if (left) {
        // move up as far as possible
        --*y;
        while (*y > 0 && !intersectsPlaced(packer, *x, *y - 1)) --*y;
    } else {
        // move left as far as possible
        --*x;
        while (*x > 0 && !intersectsPlaced(packer, *x - 1, *y)) --*x;
    }
}

static size_t calcScore(const Packer* packer, size_t x, size_t y, size_t w, size_t h) {
    size_t score = 0;
    pack(packer, &x, &y);
    if (x + w > packer->width) return 0;
    if (y + h > packer->height) return 0;

    for (size_t i = 0; i < packer->placedCount; i++) {
        const Block* block = &packer->placed[i];
        if (x + w < block->x) break;
        if (x < block->x + block->w && x + w > block->x && y < block->y + block->h && y + h > block->y) return 0;
        if (x == block->x + block->w || x + w == block->x) {
            size_t start = y > block->y ? y : block->y;
            size_t end = (y + h) < (block->y + block->h) ? (y + h) : (block->y + block->h);
            if (end > start) score += end - start;
        }
        if (y == block->y + block->h || y + h == block->y) {
            size_t start = x > block->x ? x : block->x;
            size_t end = (x + w) < (block->x + block->w) ? (x + w) : (block->x + block->w);
            if (end > start) score += end - start;
        }
    }

    if (x == 0) score += h;
    if (x + w == packer->width) score += h;
    if (y == 0) score += w;
    if (y + h == packer->height) score += w;
    return score;
}

// Places the inputs, largest (last) first. On success the blocks are in packer->placed.
static bool packerSolve(Packer* packer, const AtlasInput* inputs, size_t count) {
    packer->placedCount = 0;
    packer->freeCount = 0;
    insertFree(packer, 0, 0);

    for (size_t next = count; next-- > 0;) {
        Block block = {
            .index = inputs[next].index,
            .image = inputs[next].image,
            .w = inputs[next].image->width,
            .h = inputs[next].image->height,
        };

        XY best = {0, 0};
        size_t bestScore = 0;
        bool bestRotated = false;
        for (size_t i = 0; i < packer->freeCount; i++) {
            XY it = packer->free[i];
            size_t score = calcScore(packer, it.x, it.y, block.w, block.h);
            if (score > bestScore) {
                best = it;
                bestScore = score;
                bestRotated = false;
            }
            if (block.w != block.h) {
                score = calcScore(packer, it.x, it.y, block.h, block.w);
                if (score > bestScore) {
                    best = it;
                    bestScore = score;
                    bestRotated = true;
                }
            }
        }
        if (bestScore == 0) return false;

        block.x = best.x;
        block.y = best.y;
        if (bestRotated) {
            size_t tmp = block.w;
            block.w = block.h;
            block.h = tmp;
            block.rotated = true;
        }
        pack(packer, &block.x, &block.y);
        insertPlaced(packer, &block);

        for (size_t i = 0; i < packer->freeCount; i++) {
            if (packer->free[i].x == best.x && packer->free[i].y == best.y) {
                memmove(&packer->free[i], &packer->free[i + 1], (packer->freeCount - i - 1) * sizeof(XY));
                packer->freeCount--;
                break;
            }
        }
        addFree(packer, block.x + block.w, block.y);
        addFree(packer, block.x, block.y + block.h);

        size_t kept = 0;
        for (size_t i = 0; i < packer->freeCount; i++) {
            if (!intersectsPlaced(packer, packer->free[i].x, packer->free[i].y)) packer->free[kept++] = packer->free[i];
        }
        packer->freeCount = kept;
    }
    return true;
}

static size_t calcPot(size_t value) {
    if (value < 8) return 8;
    size_t pot = 8;
    while (pot < value) pot *= 2;
    return pot;
}

typedef struct {
    size_t width, height;
} PackerSize;

static int subImageCompare(const void* a, const void* b) {
    size_t ia = ((const SubImage*) a)->index;
    size_t ib = ((const SubImage*) b)->index;
    return ia < ib ? -1 : ia > ib ? 1 : 0;
}

bool t3xEncodeAtlas(const T3xImage* images, size_t count, T3xFormat format, T3xBuffer* out) {
    if (count == 0) return false;
    for (size_t i = 0; i < count; i++) {
        if (images[i].width == 0 || images[i].height == 0) return false;
    }

    AtlasInput* inputs = malloc(count * sizeof(AtlasInput));
    Block* placed = malloc(count * sizeof(Block));
    XY* free_ = malloc((2 * count + 1) * sizeof(XY));
    PackerSize sizes[64];
    size_t sizeCount = 0;
    bool ok = false;
    if (inputs == NULL || placed == NULL || free_ == NULL) goto done;

    size_t totalArea = 0;
    for (size_t i = 0; i < count; i++) {
        inputs[i] = (AtlasInput) {i, &images[i]};
        totalArea += (size_t) images[i].width * images[i].height;
    }
    stdSortInputs(inputs, inputs + count);

    const T3xImage* largest = inputs[count - 1].image;
    size_t start = calcPot(largest->width < largest->height ? largest->width : largest->height);
    for (size_t h = start; h <= 1024; h *= 2) {
        for (size_t w = start; w <= 1024; w *= 2) {
            if (w * h >= totalArea) sizes[sizeCount++] = (PackerSize) {w, h};
        }
    }
    // Sizes are distinct, so any sort gives tex3ds's order.
    for (size_t i = 1; i < sizeCount; i++) {
        PackerSize value = sizes[i];
        size_t j = i;
        while (j > 0 && areaSizeLess(value.width, value.height, sizes[j - 1].width, sizes[j - 1].height)) {
            sizes[j] = sizes[j - 1];
            j--;
        }
        sizes[j] = value;
    }

    for (size_t s = 0; s < sizeCount; s++) {
        Packer packer = {.width = sizes[s].width, .height = sizes[s].height, .placed = placed, .free = free_};
        if (!packerSolve(&packer, inputs, count)) continue;

        Canvas canvas;
        if (!canvasCreate(&canvas, (uint32_t) packer.width, (uint32_t) packer.height)) goto done;
        SubImage* subs = malloc(count * sizeof(SubImage));
        if (subs == NULL) {
            free(canvas.rgba);
            goto done;
        }
        for (size_t i = 0; i < packer.placedCount; i++) {
            const Block* block = &packer.placed[i];
            canvasDraw(&canvas, block->image, (uint32_t) block->x, (uint32_t) block->y, block->rotated);
            float left = (float) block->x / (float) packer.width;
            float top = 1.0f - ((float) block->y / (float) packer.height);
            float right = (float) (block->x + block->w) / (float) packer.width;
            float bottom = 1.0f - ((float) (block->y + block->h) / (float) packer.height);
            if (block->rotated) subs[i] = (SubImage) {block->index, bottom, left, top, right, true};
            else subs[i] = (SubImage) {block->index, left, top, right, bottom, false};
        }
        qsort(subs, packer.placedCount, sizeof(SubImage), subImageCompare);
        ok = writeT3x(&canvas, format, subs, packer.placedCount, out);
        free(subs);
        free(canvas.rgba);
        goto done;
    }

done:
    free(inputs);
    free(placed);
    free(free_);
    return ok;
}

void t3xFree(T3xBuffer* buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
}
