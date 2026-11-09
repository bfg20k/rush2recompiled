#include "gfx_cc.h"

// Resolves a raw combiner input number to a CCS_* source. Slots: 0 = color a, 1 = color b, 2 = color c, 3 = color d.
// Numbering is the RDP's (gbi.h G_CCMUX_* for color, G_ACMUX_* for alpha). CENTER, SCALE, K4, K5 and NOISE are not
// modelled and read as 0.
static uint8_t rgb_source(int slot, uint32_t v) {
    switch (v) {
        case 0: return CCS_COMB;
        case 1: return CCS_T0;
        case 2: return CCS_T1;
        case 3: return CCS_PRIM;
        case 4: return CCS_SHADE;
        case 5: return CCS_ENV;
    }
    if (slot == 0 || slot == 3) {
        return v == 6 ? CCS_ONE : CCS_ZERO; // a: 6 = 1, 7 = noise; d: 6 = 1, 7 = 0
    }
    if (slot == 2) {
        switch (v) {
            case 7: return CCS_COMBA;
            case 8: return CCS_T0A;
            case 9: return CCS_T1A;
            case 10: return CCS_PRIMA;
            case 11: return CCS_SHADEA;
            case 12: return CCS_ENVA;
            case 13: return CCS_LOD;
            case 14: return CCS_PRIMLOD;
        }
    }
    return CCS_ZERO;
}

// Alpha slots are 3 bits: a, b, d: 0 = combined alpha, 1..5 = texel0, texel1, prim, shade, env alpha, 6 = 1, 7 = 0.
// c: 0 = LOD fraction, 1..5 as above, 6 = primitive LOD fraction, 7 = 0.
static uint8_t alpha_source(int slot, uint32_t v) {
    switch (v) {
        case 1: return CCS_T0A;
        case 2: return CCS_T1A;
        case 3: return CCS_PRIMA;
        case 4: return CCS_SHADEA;
        case 5: return CCS_ENVA;
    }
    if (slot == 2) {
        return v == 0 ? CCS_LOD : v == 6 ? CCS_PRIMLOD : CCS_ZERO;
    }
    return v == 0 ? CCS_COMBA : v == 6 ? CCS_ONE : CCS_ZERO;
}

// In cycle 1 the texel pipeline has advanced one tile: TEXEL0 reads what cycle 0 called TEXEL1 and vice versa.
static uint8_t swap_texel(uint8_t s) {
    switch (s) {
        case CCS_T0: return CCS_T1;
        case CCS_T0A: return CCS_T1A;
        case CCS_T1: return CCS_T0;
        case CCS_T1A: return CCS_T0A;
        default: return s;
    }
}

// Cycle 0 has no earlier result to combine with.
static uint8_t no_combined(uint8_t s) {
    return s == CCS_COMB || s == CCS_COMBA ? CCS_ZERO : s;
}

static int input_of(uint8_t s) {
    switch (s) {
        case CCS_SHADE: case CCS_SHADEA: return CCIN_SHADE;
        case CCS_PRIM: case CCS_PRIMA: return CCIN_PRIM;
        case CCS_ENV: case CCS_ENVA: return CCIN_ENV;
        case CCS_LOD: case CCS_PRIMLOD: return CCIN_MISC;
        default: return -1;
    }
}

void gfx_cc_get_features(uint64_t shader_id, struct CCFeatures *f) {
    f->two_cycle = (shader_id & SHADER_OPT_TWO_CYCLE) != 0;
    f->opt_alpha = (shader_id & SHADER_OPT_ALPHA) != 0;
    f->opt_fog = (shader_id & SHADER_OPT_FOG) != 0;
    f->opt_texture_edge = (shader_id & SHADER_OPT_TEXTURE_EDGE) != 0;
    f->opt_noise = (shader_id & SHADER_OPT_NOISE) != 0;
    f->used_textures[0] = f->used_textures[1] = false;
    f->num_inputs = 0;

    bool used_input[4] = {false, false, false, false};
    const int cycles = f->two_cycle ? 2 : 1;
    for (int cy = 0; cy < 2; cy++) {
        uint32_t w = (uint32_t)(shader_id >> (cy * 28)) & 0xFFFFFFF;
        uint32_t rgb_raw[4] = {w & 15, (w >> 4) & 15, (w >> 8) & 31, (w >> 13) & 7};
        uint32_t alp_raw[4] = {(w >> 16) & 7, (w >> 19) & 7, (w >> 22) & 7, (w >> 25) & 7};
        for (int i = 0; i < 4; i++) {
            uint8_t r = rgb_source(i, rgb_raw[i]);
            uint8_t a = alpha_source(i, alp_raw[i]);
            if (cy == 0) {
                r = no_combined(r);
                a = no_combined(a);
            } else {
                r = swap_texel(r);
                a = swap_texel(a);
            }
            f->rgb[cy][i] = r;
            f->alpha[cy][i] = a;
            if (cy >= cycles) {
                continue;
            }
            const uint8_t srcs[2] = {r, a};
            for (int k = 0; k < 2; k++) {
                uint8_t s = srcs[k];
                if (s == CCS_T0 || s == CCS_T0A) f->used_textures[0] = true;
                if (s == CCS_T1 || s == CCS_T1A) f->used_textures[1] = true;
                int in = input_of(s);
                if (in >= 0) used_input[in] = true;
            }
        }
    }
    for (int k = 0; k < 4; k++) {
        if (used_input[k]) {
            f->input_kind[f->num_inputs++] = (uint8_t)k;
        }
    }
}
