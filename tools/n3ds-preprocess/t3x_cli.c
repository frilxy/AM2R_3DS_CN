// Test front end with tex3ds's command line for the options the converter uses:
//   t3x-cli [--atlas] -f <format> -z none -o <out.t3x> <png...>
// It lets tools/n3ds-preprocess/t3x.c be compared with tex3ds byte for byte.

#include "t3x.h"

#include <stb/image/stb_image.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool parseFormat(const char* name, T3xFormat* out) {
    static const struct { const char* name; T3xFormat format; } formats[] = {
        {"rgba8", T3X_RGBA8}, {"rgba", T3X_RGBA8}, {"rgba8888", T3X_RGBA8}, {"rgb8", T3X_RGB8}, {"rgb888", T3X_RGB8},
        {"rgba5551", T3X_RGBA5551}, {"rgb565", T3X_RGB565}, {"rgba4", T3X_RGBA4}, {"rgba4444", T3X_RGBA4},
        {"la8", T3X_LA8}, {"la88", T3X_LA8}, {"hilo8", T3X_HILO8}, {"l8", T3X_L8}, {"a8", T3X_A8},
        {"la4", T3X_LA4}, {"la44", T3X_LA4}, {"l4", T3X_L4}, {"a4", T3X_A4}, {"etc1", T3X_ETC1}, {"etc1a4", T3X_ETC1A4},
    };
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        if (strcmp(name, formats[i].name) == 0) {
            *out = formats[i].format;
            return true;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    bool atlas = false;
    T3xFormat format = T3X_RGBA8;
    const char* outPath = NULL;
    const char** inputs = calloc((size_t) argc, sizeof(char*));
    size_t inputCount = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--atlas") == 0) atlas = true;
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            if (!parseFormat(argv[++i], &format)) {
                fprintf(stderr, "Unknown format %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-z") == 0 && i + 1 < argc) i++;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outPath = argv[++i];
        else inputs[inputCount++] = argv[i];
    }
    if (outPath == NULL || inputCount == 0 || (!atlas && inputCount > 1)) {
        fprintf(stderr, "Usage: %s [--atlas] -f <format> -z none -o <out.t3x> <png...>\n", argv[0]);
        return 1;
    }

    T3xImage* images = calloc(inputCount, sizeof(T3xImage));
    for (size_t i = 0; i < inputCount; i++) {
        int w, h, channels;
        uint8_t* rgba = stbi_load(inputs[i], &w, &h, &channels, 4);
        if (rgba == NULL) {
            fprintf(stderr, "Failed to load %s\n", inputs[i]);
            return 1;
        }
        images[i] = (T3xImage) {rgba, (uint32_t) w, (uint32_t) h};
    }

    T3xBuffer out = {0};
    bool ok = atlas ? t3xEncodeAtlas(images, inputCount, format, &out) : t3xEncodeImage(&images[0], format, &out);
    if (!ok) {
        fprintf(stderr, atlas ? "No atlas solution found.\n" : "Invalid image\n");
        return 1;
    }
    FILE* fp = fopen(outPath, "wb");
    if (fp == NULL || fwrite(out.data, 1, out.size, fp) != out.size) {
        fprintf(stderr, "Failed to write %s\n", outPath);
        return 1;
    }
    fclose(fp);
    return 0;
}
