#pragma once

// Tex3DS (.t3x) writer: what devkitPro's tex3ds does for `tex3ds [--atlas] -f <format> -z none`, without
// ImageMagick or a separate process. The output is meant to be byte-identical to tex3ds 2.3 for the formats the
// converter uses, so the CIA reads it unchanged (atlas layout, sub-image table, swizzle, ETC1 via rg_etc1).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// GPU_TEXCOLOR values, as tex3ds writes them in the header.
typedef enum {
    T3X_RGBA8 = 0x00,
    T3X_RGB8 = 0x01,
    T3X_RGBA5551 = 0x02,
    T3X_RGB565 = 0x03,
    T3X_RGBA4 = 0x04,
    T3X_LA8 = 0x05,
    T3X_HILO8 = 0x06,
    T3X_L8 = 0x07,
    T3X_A8 = 0x08,
    T3X_LA4 = 0x09,
    T3X_L4 = 0x0A,
    T3X_A4 = 0x0B,
    T3X_ETC1 = 0x0C,
    T3X_ETC1A4 = 0x0D,
} T3xFormat;

typedef struct {
    const uint8_t* rgba; // width * height * 4 bytes, straight alpha
    uint32_t width;
    uint32_t height;
} T3xImage;

typedef struct {
    uint8_t* data;
    size_t size;
} T3xBuffer;

// One image, as `tex3ds -f <format> -z none -o out.t3x in.png`. Returns false for an image over 1024x1024.
bool t3xEncodeImage(const T3xImage* image, T3xFormat format, T3xBuffer* out);

// Several images packed into one texture, as `tex3ds --atlas -f <format> -z none -o out.t3x <images...>`; the
// sub-image table is in input order. Returns false when tex3ds would say "No atlas solution found."
bool t3xEncodeAtlas(const T3xImage* images, size_t count, T3xFormat format, T3xBuffer* out);

void t3xFree(T3xBuffer* buffer);
