// xdelta3 decoding for the SD-card page: applies a VCDIFF patch (the AM2R community patch uses LZMA secondary
// compression) to a source held in memory. Built with Emscripten by tools/web/build.sh.

#include "xdelta3.h"

#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>

// Returns the patched size, or a negative xdelta3 error code. *outData is malloc'd; the caller frees it.
EMSCRIPTEN_KEEPALIVE
int64_t xd3wasmDecode(const uint8_t* patch, uint32_t patchSize, const uint8_t* source, uint32_t sourceSize,
                      uint32_t outputCapacity, uint8_t** outData) {
    uint8_t* output = malloc(outputCapacity);
    if (output == NULL) return -1;
    usize_t outputSize = 0;
    int rc = xd3_decode_memory(patch, patchSize, source, sourceSize, output, &outputSize, outputCapacity, 0);
    if (rc != 0) {
        free(output);
        return rc < 0 ? rc : -rc;
    }
    *outData = output;
    return (int64_t) outputSize;
}
