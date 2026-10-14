// C entry points for rg_etc1, set up the way tex3ds calls it (medium quality, no dithering).

#include "../../vendor/rg_etc1/rg_etc1.h"

#include <stdint.h>

extern "C" void t3xEtc1Init(void) {
    rg_etc1::pack_etc1_block_init();
}

extern "C" void t3xEtc1PackBlock(uint8_t out[8], const uint8_t rgba[64]) {
    rg_etc1::etc1_pack_params params;
    params.clear();
    params.m_quality = rg_etc1::cMediumQuality;
    rg_etc1::pack_etc1_block(out, reinterpret_cast<const unsigned*>(rgba), params);
}
