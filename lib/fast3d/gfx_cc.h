#ifndef GFX_CC_H
#define GFX_CC_H

#include <stdint.h>
#include <stdbool.h>

// A shader id packs the RDP's color combiner for both cycles exactly as G_SETCOMBINE encodes it, plus option bits.
// Per cycle, 28 bits (cycle 1 starts at bit 28):
//   rgb:   a (4 bits @0)  b (4 @4)  c (5 @8)  d (3 @13)
//   alpha: a (3 @16)  b (3 @19)  c (3 @22)  d (3 @25)
// The combiner computes (a - b) * c + d per channel group. The hardware input numbering differs per slot (see
// gfx_cc.c); gfx_cc_get_features resolves every slot to one of the CCS_* sources below.
enum {
    CCS_ZERO,
    CCS_ONE,
    CCS_COMB,    // result of cycle 0 (0 in cycle 0)
    CCS_COMBA,
    CCS_T0,
    CCS_T0A,
    CCS_T1,
    CCS_T1A,
    CCS_PRIM,
    CCS_PRIMA,
    CCS_SHADE,
    CCS_SHADEA,
    CCS_ENV,
    CCS_ENVA,
    CCS_LOD,     // LOD fraction (approximated from distance)
    CCS_PRIMLOD  // primitive LOD fraction (G_SETPRIMCOLOR)
};

// Per-vertex inputs the core writes after position/texcoord/fog, in this order, only for those a combiner uses.
enum {
    CCIN_SHADE,
    CCIN_PRIM,
    CCIN_ENV,
    CCIN_MISC // x = LOD fraction, y = primitive LOD fraction
};

#define SHADER_OPT_TWO_CYCLE ((uint64_t)1 << 56)
#define SHADER_OPT_ALPHA ((uint64_t)1 << 57)
#define SHADER_OPT_FOG ((uint64_t)1 << 58)
#define SHADER_OPT_TEXTURE_EDGE ((uint64_t)1 << 59)
#define SHADER_OPT_NOISE ((uint64_t)1 << 60)

// Packs one cycle's raw G_SETCOMBINE fields (a0 b0 c0 d0 / Aa0 Ab0 Ac0 Ad0 in the hardware numbering).
static inline uint64_t gfx_cc_pack_cycle(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                                         uint32_t aa, uint32_t ab, uint32_t ac, uint32_t ad) {
    return (uint64_t)((a & 15) | ((b & 15) << 4) | ((c & 31) << 8) | ((d & 7) << 13) |
                      ((aa & 7) << 16) | ((ab & 7) << 19) | ((ac & 7) << 22) | ((ad & 7) << 25));
}

struct CCFeatures {
    uint8_t rgb[2][4];   // CCS_* per cycle and slot (a, b, c, d); cycle 1's TEXEL0/TEXEL1 are already swapped
    uint8_t alpha[2][4];
    bool two_cycle;
    bool opt_alpha;
    bool opt_fog;
    bool opt_texture_edge;
    bool opt_noise;
    bool used_textures[2];
    int num_inputs;
    uint8_t input_kind[4]; // CCIN_* of each used input, in vertex order
};

#ifdef __cplusplus
extern "C" {
#endif

void gfx_cc_get_features(uint64_t shader_id, struct CCFeatures *cc_features);

#ifdef __cplusplus
}
#endif

#endif
