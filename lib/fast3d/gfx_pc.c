#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>

// Rush 2 recomp: embedded Fast3D core. Adapted from Emill/n64-fast3d-engine (see docs/fast3d_port_plan.md): display lists,
// vertices, lights, viewports, textures and palettes are read from the recompiled game's word-swapped RDRAM (f3d_gbi.h).
#include "f3d_gbi.h"

#include "gfx_pc.h"
#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "gfx_screen_config.h"

#define SUPPORT_CHECK(x) ((void)(x))

// SCALE_M_N: upscale/downscale M-bit integer to N-bit
#define SCALE_5_8(VAL_) (((VAL_) * 0xFF) / 0x1F)
#define SCALE_8_5(VAL_) ((((VAL_) + 4) * 0x1F) / 0xFF)
#define SCALE_4_8(VAL_) ((VAL_) * 0x11)
#define SCALE_8_4(VAL_) ((VAL_) / 0x11)
#define SCALE_3_8(VAL_) ((VAL_) * 0x24)
#define SCALE_8_3(VAL_) ((VAL_) / 0x24)

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define HALF_SCREEN_WIDTH (SCREEN_WIDTH / 2)
#define HALF_SCREEN_HEIGHT (SCREEN_HEIGHT / 2)

#define RATIO_X (gfx_current_dimensions.width / (2.0f * HALF_SCREEN_WIDTH))
#define RATIO_Y (gfx_current_dimensions.height / (2.0f * HALF_SCREEN_HEIGHT))

#define MAX_BUFFERED 256
#define MAX_LIGHTS 7 // F3DEX2 takes up to 7 directional lights plus the ambient one (SM64's 2 is far too few here)
#define MAX_VERTICES 64

struct RGBA {
    uint8_t r, g, b, a;
};

struct XYWidthHeight {
    int32_t x, y, width, height; // signed: a view may start above or left of the screen (the track select preview does)
};

struct LoadedVertex {
    float x, y, z, w;
    float u, v;
    struct RGBA color;
    uint8_t clip_rej;
};

// RDRAM access state. g_rdram is the recompiled game's RDRAM; g_segments is the RSP segment table (G_MW_SEGMENT).
static uint8_t *g_rdram;
static uint32_t g_segments[16];
// TMEM model. The RDP loads textures into 4 KB of texture memory and eight tile descriptors say how to read them back;
// games rely on that (several textures resident at once, sizes from G_SETTILESIZE, rows at the tile's line stride). Loads
// copy bytes out of RDRAM into `tmem` in canonical (big-endian) byte order. The hardware's odd-row word swaps are not
// modeled: they cancel between a load and the matching sample. RGBA32 is kept as plain 4-byte texels (the hardware splits
// it over two banks), and palette entries live in `tlut`, one slot per 64-bit TMEM word starting at word 256.
static uint8_t tmem[4096];
static uint16_t tlut[256];
static uint64_t tlut_hash;
static uint8_t tex_level; // G_TEXTURE's tile: TEXEL0 reads this tile and TEXEL1 the next one

struct TextureHashmapNode {
    struct TextureHashmapNode *next;

    uint64_t texture_key;
    uint8_t fmt, siz;
    
    uint32_t texture_id;
    uint8_t cms, cmt;
    bool linear_filter;
};
static struct {
    struct TextureHashmapNode *hashmap[1024];
    struct TextureHashmapNode pool[512];
    uint32_t pool_pos;
} gfx_texture_cache;

struct ColorCombiner {
    uint64_t cc_id;
    struct ShaderProgram *prg;
    uint8_t num_inputs;
    uint8_t input_kind[4]; // CCIN_* of each per-vertex combiner input, in the order the vertex writes them
};

#define COMBINER_POOL_MAX 512
static struct ColorCombiner color_combiner_pool[COMBINER_POOL_MAX];
static uint32_t color_combiner_pool_size;

static struct RSP {
    float modelview_matrix_stack[11][4][4];
    uint8_t modelview_matrix_stack_size;
    
    float MP_matrix[4][4];
    float P_matrix[4][4];
    
    Light_t current_lights[MAX_LIGHTS + 1];
    float current_lights_coeffs[MAX_LIGHTS][3];
    float current_lookat_coeffs[2][3]; // lookat_x, lookat_y
    uint8_t current_num_lights; // includes ambient light
    bool lights_changed;
    
    uint32_t geometry_mode;
    float fog_mul, fog_offset; // G_MW_FOG values, or the floats of gEXSetFogFloat
    
    struct {
        // U0.16
        uint16_t s, t;
    } texture_scaling_factor;
    
    struct LoadedVertex loaded_vertices[MAX_VERTICES + 4];
} rsp;

static struct RDP {
    struct {
        const uint8_t *addr; // G_SETTIMG source in RDRAM
        uint8_t siz;
        uint32_t width;      // texels
    } timg;
    struct TileDesc {
        uint8_t fmt, siz, palette;
        uint8_t cms, cmt, masks, maskt, shifts, shiftt;
        uint16_t line, tmem;         // 64-bit words
        uint16_t uls, ult, lrs, lrt; // U10.2
    } tiles[8];
    bool textures_changed[2];
    
    uint32_t other_mode_l, other_mode_h;
    uint64_t combine_mode; // both cycles of G_SETCOMBINE in gfx_cc.h packing, without option bits
    uint8_t prim_lod_frac;
    
    struct RGBA env_color, prim_color, fog_color, fill_color;
    struct XYWidthHeight viewport, scissor;
    bool viewport_or_scissor_changed;
    void *z_buf_address;
    void *color_image_address;
} rdp;

static struct RenderingState {
    bool depth_test;
    bool depth_mask;
    bool decal_mode;
    bool alpha_blend;
    struct XYWidthHeight viewport, scissor;
    struct ShaderProgram *shader_program;
    struct TextureHashmapNode *textures[2];
} rendering_state;

struct GfxDimensions gfx_current_dimensions;

static bool dropped_frame;

static float buf_vbo[MAX_BUFFERED * (26 * 3)]; // 3 vertices in a triangle and 26 floats per vtx
static size_t buf_vbo_len;
static size_t buf_vbo_num_tris;

static struct GfxRenderingAPI *gfx_rapi;

static unsigned long get_time(void) {
    return 0;
}

static void dbg_note_draw(size_t tris);
static void dbg_note_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry);

// Debugging (replay tool): RUSH2_F3D_DRAW_RANGE=a,b draws only flushes number a..b-1 of the frame (read once).
static uint64_t dbg_last_cc_id;
static int dbg_range_lo = -1, dbg_range_hi = -1, dbg_flush_no = 0;

static void gfx_flush(void) {
    if (buf_vbo_len > 0) {
        int num = buf_vbo_num_tris;
        dbg_note_draw(buf_vbo_num_tris);
        unsigned long t0 = get_time();
        if (dbg_range_hi < 0 && dbg_range_lo == -1) {
            const char *r = getenv("RUSH2_F3D_DRAW_RANGE");
            dbg_range_lo = 0;
            dbg_range_hi = 0x7FFFFFFF;
            if (r != NULL) sscanf(r, "%d,%d", &dbg_range_lo, &dbg_range_hi);
        }
        const int no = dbg_flush_no++;
        if (getenv("RUSH2_F3D_FLUSH_VERTS") != NULL && buf_vbo_len >= 8) {
            if (getenv("RUSH2_F3D_FLUSH_VERTS")[0] == '2') {
                const size_t per = buf_vbo_len / (buf_vbo_num_tris * 3);
                fprintf(stderr, "[fast3d] flush %d vertex floats (%zu each):", no, per);
                for (size_t k = 0; k < per; k++) fprintf(stderr, " %.3f", buf_vbo[k]);
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "[fast3d] flush %d shader id %016llx alpha_blend %d\n", no, (unsigned long long)dbg_last_cc_id, (int)rendering_state.alpha_blend);
            fprintf(stderr, "[fast3d] flush %d: tris=%zu first vertex x=%.3f y=%.3f z=%.3f w=%.3f viewport %d,%d %dx%d scissor %d,%d %dx%d\n", no, buf_vbo_num_tris, buf_vbo[0], buf_vbo[1], buf_vbo[2], buf_vbo[3],
                    rendering_state.viewport.x, rendering_state.viewport.y, rendering_state.viewport.width, rendering_state.viewport.height,
                    rendering_state.scissor.x, rendering_state.scissor.y, rendering_state.scissor.width, rendering_state.scissor.height);
            fprintf(stderr, "[fast3d]   alpha_blend=%d depth_test=%d depth_mask=%d floats/vertex=%zu\n", (int)rendering_state.alpha_blend, (int)rendering_state.depth_test, (int)rendering_state.depth_mask, buf_vbo_len / (buf_vbo_num_tris * 3));
        }
        if (dbg_range_hi == -1 || (no >= dbg_range_lo && no < dbg_range_hi)) {
            gfx_rapi->draw_triangles(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
        }
        buf_vbo_len = 0;
        buf_vbo_num_tris = 0;
        unsigned long t1 = get_time();
        /*if (t1 - t0 > 1000) {
            printf("f: %d %d\n", num, (int)(t1 - t0));
        }*/
    }
}

static struct ShaderProgram *gfx_lookup_or_create_shader_program(uint64_t shader_id) {
    struct ShaderProgram *prg = gfx_rapi->lookup_shader(shader_id);
    if (prg == NULL) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        prg = gfx_rapi->create_and_load_new_shader(shader_id);
        rendering_state.shader_program = prg;
    }
    return prg;
}

static void gfx_generate_cc(struct ColorCombiner *comb, uint64_t cc_id) {
    struct CCFeatures f;
    gfx_cc_get_features(cc_id, &f);
    comb->cc_id = cc_id;
    comb->num_inputs = (uint8_t)f.num_inputs;
    memcpy(comb->input_kind, f.input_kind, sizeof(comb->input_kind));
    comb->prg = gfx_lookup_or_create_shader_program(cc_id);
}

static struct ColorCombiner *gfx_lookup_or_create_color_combiner(uint64_t cc_id) {
    static struct ColorCombiner *prev_combiner;
    if (prev_combiner != NULL && prev_combiner->cc_id == cc_id) {
        return prev_combiner;
    }
    
    for (size_t i = 0; i < color_combiner_pool_size; i++) {
        if (color_combiner_pool[i].cc_id == cc_id) {
            return prev_combiner = &color_combiner_pool[i];
        }
    }
    gfx_flush();
    if (color_combiner_pool_size >= COMBINER_POOL_MAX) {
        color_combiner_pool_size = 0; // the programs live in the backend's table; only these small records are rebuilt
        prev_combiner = NULL;
    }
    struct ColorCombiner *comb = &color_combiner_pool[color_combiner_pool_size++];
    gfx_generate_cc(comb, cc_id);
    return prev_combiner = comb;
}

static bool gfx_texture_cache_lookup(int tile, struct TextureHashmapNode **n, uint64_t orig_addr, uint32_t fmt, uint32_t siz) {
    size_t hash = (size_t)(orig_addr ^ (orig_addr >> 21) ^ (orig_addr >> 42));
    hash &= 0x3ff;
    struct TextureHashmapNode **node = &gfx_texture_cache.hashmap[hash];
    while (*node != NULL && *node - gfx_texture_cache.pool < (int)gfx_texture_cache.pool_pos) {
        if ((*node)->texture_key == orig_addr && (*node)->fmt == fmt && (*node)->siz == siz) {
            gfx_rapi->select_texture(tile, (*node)->texture_id);
            *n = *node;
            return true;
        }
        node = &(*node)->next;
    }
    if (gfx_texture_cache.pool_pos == sizeof(gfx_texture_cache.pool) / sizeof(struct TextureHashmapNode)) {
        // Pool is full. We just invalidate everything and start over.
        gfx_texture_cache.pool_pos = 0;
        node = &gfx_texture_cache.hashmap[hash];
        //puts("Clearing texture cache");
    }
    *node = &gfx_texture_cache.pool[gfx_texture_cache.pool_pos++];
    if ((*node)->texture_key == 0) {
        (*node)->texture_id = gfx_rapi->new_texture();
    }
    gfx_rapi->select_texture(tile, (*node)->texture_id);
    gfx_rapi->set_sampler_parameters(tile, false, 0, 0);
    (*node)->cms = 0;
    (*node)->cmt = 0;
    (*node)->linear_filter = false;
    (*node)->next = NULL;
    (*node)->texture_key = orig_addr;
    (*node)->fmt = fmt;
    (*node)->siz = siz;
    *n = *node;
    return false;
}

static inline uint8_t rdram_byte(const uint8_t *p, uint32_t i) {
    return g_rdram[(uint32_t)((p - g_rdram) + i) ^ 3];
}

static uint64_t gfx_fnv1a(uint64_t h, const uint8_t *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Diagnostics for bring-up: each distinct line is printed once to stderr, at most F3D_LOG_MAX lines in all, so a log
// shows exactly which combiners, render modes and texture setups the game uses without costing frame time.
#define F3D_LOG_MAX 160
static uint64_t f3d_log_seen[F3D_LOG_MAX];
static int f3d_log_count;
static void f3d_log_once(uint64_t key, const char *fmt, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g) {
    key |= 1;
    for (int i = 0; i < f3d_log_count; i++) {
        if (f3d_log_seen[i] == key) {
            return;
        }
    }
    if (f3d_log_count >= F3D_LOG_MAX) {
        return;
    }
    f3d_log_seen[f3d_log_count++] = key;
    fprintf(stderr, fmt, a, b, c, d, e, f, g);
}

// A format or state the core cannot decode yet draws as a 2x2 magenta texture instead of aborting the game.
static void gfx_import_unsupported(int tile) {
    static const uint8_t magenta[16] = {255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255};
    gfx_rapi->upload_texture(magenta, 2, 2);
}

static const uint8_t k_siz_bits[4] = {4, 8, 16, 32};

// Canonical bytes out of RDRAM (byte at A lives at rdram[A ^ 3]; an aligned word reads natively as its four bytes, high
// byte first), a word at a time where aligned.
static void rdram_copy(uint8_t *dst, uint32_t off, uint32_t n) {
    uint32_t i = 0;
    for (; i < n && ((off + i) & 3) != 0; i++) {
        dst[i] = g_rdram[(off + i) ^ 3];
    }
    for (; i + 4 <= n; i += 4) {
        uint32_t w;
        memcpy(&w, g_rdram + off + i, 4);
        dst[i] = (uint8_t)(w >> 24);
        dst[i + 1] = (uint8_t)(w >> 16);
        dst[i + 2] = (uint8_t)(w >> 8);
        dst[i + 3] = (uint8_t)w;
    }
    for (; i < n; i++) {
        dst[i] = g_rdram[(off + i) ^ 3];
    }
}

// Bytes per row of a tile in `tmem` (RGBA32 rows are twice as wide in the plain 4-byte layout).
static uint32_t tile_stride(const struct TileDesc *t) {
    return (uint32_t)t->line * 8 * (t->siz == G_IM_SIZ_32b ? 2 : 1);
}

// The region of the tile a draw samples: from G_SETTILESIZE, limited by the tile's line and by the mask (a power-of-two
// repeat). Falls back to the line's full width when no sane size was set.
static void tile_dims(const struct TileDesc *t, uint32_t *x0, uint32_t *y0, uint32_t *w, uint32_t *h) {
    uint32_t stride = tile_stride(t);
    uint32_t bits = k_siz_bits[t->siz];
    uint32_t max_w = stride * 8 / bits;
    uint32_t ww = t->lrs >= t->uls ? ((uint32_t)(t->lrs - t->uls) >> 2) + 1 : 0;
    uint32_t hh = t->lrt >= t->ult ? ((uint32_t)(t->lrt - t->ult) >> 2) + 1 : 0;
    if (ww == 0 || ww > max_w) {
        ww = max_w;
    }
    uint32_t base = (uint32_t)t->tmem * 8;
    uint32_t max_h = stride != 0 && base < sizeof(tmem) ? (uint32_t)(sizeof(tmem) - base) / stride : 0;
    if (hh == 0 || hh > max_h) {
        hh = max_h; // before the masks, so a tile with no size still repeats at its mask
    }
    if (t->masks != 0 && (1u << t->masks) < ww) {
        ww = 1u << t->masks;
    }
    if (t->maskt != 0 && (1u << t->maskt) < hh) {
        hh = 1u << t->maskt;
    }
    // The RDP subtracts the tile's (uls, ult) from every texture coordinate, and LOADTILE / LOADBLOCK store from the
    // tile's TMEM origin, so the image always starts at TMEM row 0 / column 0 of the tile. A tile whose size window starts
    // at ult = 54 (the radar map's strips) addresses the rows that were loaded, not row 54 of an image that isn't there.
    *x0 = 0;
    *y0 = 0;
    *w = ww;
    *h = hh;
}

// Cache key: the bytes of the region, the sizes that shape the image, and the palette for CI textures.
static inline bool tlut_is_ia16(void);
static uint64_t gfx_texture_key(const struct TileDesc *t, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h) {
    uint32_t bits = k_siz_bits[t->siz];
    uint32_t stride = tile_stride(t);
    uint64_t k = 0xcbf29ce484222325ULL;
    uint32_t row_bytes = (w * bits + 7) / 8;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t off = (uint32_t)t->tmem * 8 + (y0 + y) * stride + (x0 * bits) / 8;
        if (off + row_bytes > sizeof(tmem)) {
            break;
        }
        k = gfx_fnv1a(k, tmem + off, row_bytes);
    }
    k = (k ^ ((uint64_t)w << 32 | h)) * 0x100000001b3ULL;
    k = (k ^ ((uint64_t)t->siz << 8 | t->fmt)) * 0x100000001b3ULL;
    if (t->fmt == G_IM_FMT_CI) {
        k = (k ^ tlut_hash ^ t->palette ^ ((uint64_t)tlut_is_ia16() << 12)) * 0x100000001b3ULL;
    }
    return k | 1; // 0 means "unused node"
}

// G_SETOTHERMODE_H bits 14-15 (G_MDSFT_TEXTLUT): 2 = palette entries are RGBA5551, 3 = IA16 (intensity high byte, alpha low).
static inline bool tlut_is_ia16(void) {
    return ((rdp.other_mode_h >> 14) & 3) == 3;
}

static void put_rgba5551(uint8_t *out, uint16_t c) {
    // A transparent texel keeps no color: with bilinear filtering its (often magenta) RGB would bleed into the opaque
    // texels next to it as a fringe, which the RDP's point-sampled 1:1 rectangles never showed.
    const bool opaque = (c & 1) != 0;
    out[0] = opaque ? SCALE_5_8(c >> 11) : 0;
    out[1] = opaque ? SCALE_5_8((c >> 6) & 0x1f) : 0;
    out[2] = opaque ? SCALE_5_8((c >> 1) & 0x1f) : 0;
    out[3] = opaque ? 255 : 0;
}

static void import_texture(int slot) {
    static uint8_t rgba[8192 * 4]; // a tile fits in 4 KB: at most 8192 4-bit texels
    const struct TileDesc *t = &rdp.tiles[(tex_level + slot) & 7];
    uint32_t x0, y0, w, h;
    tile_dims(t, &x0, &y0, &w, &h);
    uint32_t bits = k_siz_bits[t->siz];
    uint32_t stride = tile_stride(t);

    if (w == 0 || h == 0 || w * h > 8192 || stride == 0) {
        f3d_log_once(0x1000000000ULL ^ ((uint64_t)t->fmt << 40) ^ ((uint64_t)t->siz << 36) ^ ((uint64_t)t->line << 20) ^ (w << 10) ^ h,
                     "[fast3d] unsupported texture: fmt=%u siz=%u line=%u tmem=%u size=%ux%u%.0u\n", t->fmt, t->siz, t->line, t->tmem, w, h, 0);
        gfx_import_unsupported(slot); // a tile that was never set up
        return;
    }
    if (gfx_texture_cache_lookup(slot, &rendering_state.textures[slot], gfx_texture_key(t, x0, y0, w, h), t->fmt, t->siz)) {
        return;
    }

    for (uint32_t y = 0; y < h; y++) {
        uint32_t row = (uint32_t)t->tmem * 8 + (y0 + y) * stride;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *out = rgba + 4 * (y * w + x);
            uint32_t tx = x0 + x;
            uint32_t at = row + (tx * bits) / 8;
            if (y & 1) {
                at ^= 4; // odd lines hold their 64-bit words' halves swapped
            }
            // Reads past TMEM (a window beyond the rows) give transparent black rather than leaving the array.
            uint8_t b0 = at < sizeof(tmem) ? tmem[at] : 0;
            uint8_t b1 = at + 1 < sizeof(tmem) ? tmem[at + 1] : 0;
            uint8_t nib = (tx & 1) ? (b0 & 0xf) : (b0 >> 4);
            uint8_t v = 0;
            out[0] = out[1] = out[2] = out[3] = 0;
            switch (t->fmt) {
                case G_IM_FMT_RGBA:
                    if (t->siz == G_IM_SIZ_16b) {
                        put_rgba5551(out, (uint16_t)(b0 << 8 | b1));
                    } else if (t->siz == G_IM_SIZ_32b && at + 3 < sizeof(tmem)) {
                        memcpy(out, tmem + at, 4);
                    }
                    break;
                case G_IM_FMT_IA:
                    if (t->siz == G_IM_SIZ_16b) {
                        out[0] = out[1] = out[2] = b0;
                        out[3] = b1;
                    } else if (t->siz == G_IM_SIZ_8b) {
                        out[0] = out[1] = out[2] = SCALE_4_8(b0 >> 4);
                        out[3] = SCALE_4_8(b0 & 0xf);
                    } else if (t->siz == G_IM_SIZ_4b) {
                        out[0] = out[1] = out[2] = SCALE_3_8(nib >> 1);
                        out[3] = (nib & 1) ? 255 : 0;
                    }
                    break;
                case G_IM_FMT_I:
                    v = t->siz == G_IM_SIZ_8b ? b0 : SCALE_4_8(nib);
                    out[0] = out[1] = out[2] = out[3] = v; // intensity is also the alpha
                    break;
                case G_IM_FMT_CI: {
                    uint32_t idx = t->siz == G_IM_SIZ_8b ? b0 : (uint32_t)t->palette * 16 + nib;
                    uint16_t e = tlut[idx & 255];
                    if (tlut_is_ia16()) {
                        out[0] = out[1] = out[2] = (uint8_t)(e >> 8);
                        out[3] = (uint8_t)e;
                    } else {
                        put_rgba5551(out, e);
                    }
                    break;
                }
            }
        }
    }
    gfx_rapi->upload_texture(rgba, w, h);
}

static void gfx_normalize_vector(float v[3]) {
    float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= s;
    v[1] /= s;
    v[2] /= s;
}

static void gfx_transposed_matrix_mul(float res[3], const float a[3], const float b[4][4]) {
    res[0] = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
    res[1] = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
    res[2] = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
}

static void calculate_normal_dir(const Light_t *light, float coeffs[3]) {
    float light_dir[3] = {
        light->dir[0] / 127.0f,
        light->dir[1] / 127.0f,
        light->dir[2] / 127.0f
    };
    gfx_transposed_matrix_mul(coeffs, light_dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}

static void gfx_matrix_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] +
                        a[i][1] * b[1][j] +
                        a[i][2] * b[2][j] +
                        a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}

static void gfx_sp_matrix_apply(uint8_t parameters, float matrix[4][4]);

// G_SETTILE shift: 1..10 shifts the coordinate right (divide by 2^n), 11..15 left (multiply by 2^(16 - n)).
static inline float tile_shift_scale(uint8_t shift) {
    return shift == 0 ? 1.0f : shift <= 10 ? 1.0f / (float)(1u << shift) : (float)(1u << (16 - shift));
}
static void gfx_sp_matrix(uint8_t parameters, const int32_t *addr) {
    float matrix[4][4];
#ifndef GBI_FLOATS
    // Original GBI where fixed point matrices are used
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            int32_t int_part = addr[i * 2 + j / 2];
            uint32_t frac_part = addr[8 + i * 2 + j / 2];
            matrix[i][j] = (int32_t)((int_part & 0xffff0000) | (frac_part >> 16)) / 65536.0f;
            matrix[i][j + 1] = (int32_t)((int_part << 16) | (frac_part & 0xffff)) / 65536.0f;
        }
    }
#else
    // For a modified GBI where fixed point values are replaced with floats
    memcpy(matrix, addr, sizeof(matrix));
#endif
    gfx_sp_matrix_apply(parameters, matrix);
}

static void gfx_sp_matrix_apply(uint8_t parameters, float matrix[4][4]) {
    if (parameters & G_MTX_PROJECTION) {
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.P_matrix, matrix, sizeof(float[4][4]));
        } else {
            gfx_matrix_mul(rsp.P_matrix, matrix, rsp.P_matrix);
        }
    } else { // G_MTX_MODELVIEW
        if ((parameters & G_MTX_PUSH) && rsp.modelview_matrix_stack_size < 11) {
            ++rsp.modelview_matrix_stack_size;
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 2], sizeof(float[4][4]));
        }
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, sizeof(float[4][4]));
        } else {
            gfx_matrix_mul(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
        }
        rsp.lights_changed = 1;
    }
    gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
}

static void gfx_sp_pop_matrix(uint32_t count) {
    while (count--) {
        if (rsp.modelview_matrix_stack_size > 0) {
            --rsp.modelview_matrix_stack_size;
            if (rsp.modelview_matrix_stack_size > 0) {
                gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
            }
        }
    }
}

static float gfx_adjust_x_for_aspect_ratio(float x) {
    return x * (4.0f / 3.0f) / ((float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height);
}

static void gfx_sp_vertex(size_t n_vertices, size_t dest_index, const void *vertices) {
    if (dest_index >= MAX_VERTICES) {
        return; // also catches the unsigned underflow of a corrupt v0 - n
    }
    if (dest_index + n_vertices > MAX_VERTICES) {
        n_vertices = MAX_VERTICES - dest_index;
    }
    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        Vtx_t v_storage;
        Vtx_tn vn_storage;
        f3d_read_vtx(g_rdram, (uint32_t)((const uint8_t *)vertices - g_rdram) + (uint32_t)i * F3D_VTX_SIZE, &v_storage, &vn_storage);
        const Vtx_t *v = &v_storage;
        const Vtx_tn *vn = &vn_storage;
        struct LoadedVertex *d = &rsp.loaded_vertices[dest_index];
        
        float x = v->ob[0] * rsp.MP_matrix[0][0] + v->ob[1] * rsp.MP_matrix[1][0] + v->ob[2] * rsp.MP_matrix[2][0] + rsp.MP_matrix[3][0];
        float y = v->ob[0] * rsp.MP_matrix[0][1] + v->ob[1] * rsp.MP_matrix[1][1] + v->ob[2] * rsp.MP_matrix[2][1] + rsp.MP_matrix[3][1];
        float z = v->ob[0] * rsp.MP_matrix[0][2] + v->ob[1] * rsp.MP_matrix[1][2] + v->ob[2] * rsp.MP_matrix[2][2] + rsp.MP_matrix[3][2];
        float w = v->ob[0] * rsp.MP_matrix[0][3] + v->ob[1] * rsp.MP_matrix[1][3] + v->ob[2] * rsp.MP_matrix[2][3] + rsp.MP_matrix[3][3];
        
        x = gfx_adjust_x_for_aspect_ratio(x);
        
        short U = v->tc[0] * rsp.texture_scaling_factor.s >> 16;
        short V = v->tc[1] * rsp.texture_scaling_factor.t >> 16;
        
        if (rsp.geometry_mode & G_LIGHTING) {
            if (rsp.lights_changed) {
                for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                    calculate_normal_dir(&rsp.current_lights[i], rsp.current_lights_coeffs[i]);
                }
                static const Light_t lookat_x = {{0, 0, 0}, 0, {0, 0, 0}, 0, {127, 0, 0}, 0};
                static const Light_t lookat_y = {{0, 0, 0}, 0, {0, 0, 0}, 0, {0, 127, 0}, 0};
                calculate_normal_dir(&lookat_x, rsp.current_lookat_coeffs[0]);
                calculate_normal_dir(&lookat_y, rsp.current_lookat_coeffs[1]);
                rsp.lights_changed = false;
            }
            
            int r = rsp.current_lights[rsp.current_num_lights - 1].col[0];
            int g = rsp.current_lights[rsp.current_num_lights - 1].col[1];
            int b = rsp.current_lights[rsp.current_num_lights - 1].col[2];
            
            for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                float intensity = 0;
                intensity += vn->n[0] * rsp.current_lights_coeffs[i][0];
                intensity += vn->n[1] * rsp.current_lights_coeffs[i][1];
                intensity += vn->n[2] * rsp.current_lights_coeffs[i][2];
                intensity /= 127.0f;
                if (intensity > 0.0f) {
                    r += intensity * rsp.current_lights[i].col[0];
                    g += intensity * rsp.current_lights[i].col[1];
                    b += intensity * rsp.current_lights[i].col[2];
                }
            }
            
            d->color.r = r > 255 ? 255 : r;
            d->color.g = g > 255 ? 255 : g;
            d->color.b = b > 255 ? 255 : b;
            
            if (rsp.geometry_mode & G_TEXTURE_GEN) {
                float dotx = 0, doty = 0;
                dotx += vn->n[0] * rsp.current_lookat_coeffs[0][0];
                dotx += vn->n[1] * rsp.current_lookat_coeffs[0][1];
                dotx += vn->n[2] * rsp.current_lookat_coeffs[0][2];
                doty += vn->n[0] * rsp.current_lookat_coeffs[1][0];
                doty += vn->n[1] * rsp.current_lookat_coeffs[1][1];
                doty += vn->n[2] * rsp.current_lookat_coeffs[1][2];
                
                U = (int32_t)((dotx / 127.0f + 1.0f) / 4.0f * rsp.texture_scaling_factor.s);
                V = (int32_t)((doty / 127.0f + 1.0f) / 4.0f * rsp.texture_scaling_factor.t);
            }
        } else {
            d->color.r = v->cn[0];
            d->color.g = v->cn[1];
            d->color.b = v->cn[2];
        }
        
        d->u = U;
        d->v = V;
        
        // trivial clip rejection
        d->clip_rej = 0;
        if (x < -w) d->clip_rej |= 1;
        if (x > w) d->clip_rej |= 2;
        if (y < -w) d->clip_rej |= 4;
        if (y > w) d->clip_rej |= 8;
        if (z < -w) d->clip_rej |= 16;
        if (z > w) d->clip_rej |= 32;
        
        d->x = x;
        d->y = y;
        d->z = z;
        d->w = w;
        
        if (rsp.geometry_mode & G_FOG) {
            if (fabsf(w) < 0.001f) {
                // To avoid division by zero
                w = 0.001f;
            }
            
            float winv = 1.0f / w;
            if (winv < 0.0f) {
                winv = 32767.0f;
            }
            
            float fog_z = z * winv * rsp.fog_mul + rsp.fog_offset;
            if (fog_z < 0) fog_z = 0;
            if (fog_z > 255) fog_z = 255;
            d->color.a = fog_z; // Use alpha variable to store fog factor
        } else {
            d->color.a = v->cn[3];
        }
    }
}

static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
    if (vtx1_idx >= MAX_VERTICES + 4 || vtx2_idx >= MAX_VERTICES + 4 || vtx3_idx >= MAX_VERTICES + 4) { // the last 4 slots are the rect corners
        return;
    }
    struct LoadedVertex *v1 = &rsp.loaded_vertices[vtx1_idx];
    struct LoadedVertex *v2 = &rsp.loaded_vertices[vtx2_idx];
    struct LoadedVertex *v3 = &rsp.loaded_vertices[vtx3_idx];
    struct LoadedVertex *v_arr[3] = {v1, v2, v3};
    
    //if (rand()%2) return;
    
    if (v1->clip_rej & v2->clip_rej & v3->clip_rej) {
        // The whole triangle lies outside the visible area
        return;
    }
    
    if ((rsp.geometry_mode & G_CULL_BOTH) != 0) {
        float dx1 = v1->x / (v1->w) - v2->x / (v2->w);
        float dy1 = v1->y / (v1->w) - v2->y / (v2->w);
        float dx2 = v3->x / (v3->w) - v2->x / (v2->w);
        float dy2 = v3->y / (v3->w) - v2->y / (v2->w);
        float cross = dx1 * dy2 - dy1 * dx2;
        
        if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) {
            // If one vertex lies behind the eye, negating cross will give the correct result.
            // If all vertices lie behind the eye, the triangle will be rejected anyway.
            cross = -cross;
        }
        
        switch (rsp.geometry_mode & G_CULL_BOTH) {
            case G_CULL_FRONT:
                if (cross <= 0) return;
                break;
            case G_CULL_BACK:
                if (cross >= 0) return;
                break;
            case G_CULL_BOTH:
                // Why is this even an option?
                return;
        }
    }
    
    bool depth_test = (rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER;
    if (depth_test != rendering_state.depth_test) {
        gfx_flush();
        gfx_rapi->set_depth_test(depth_test);
        rendering_state.depth_test = depth_test;
    }
    
    bool z_upd = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    if (z_upd != rendering_state.depth_mask) {
        gfx_flush();
        gfx_rapi->set_depth_mask(z_upd);
        rendering_state.depth_mask = z_upd;
    }
    
    bool zmode_decal = (rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (zmode_decal != rendering_state.decal_mode) {
        gfx_flush();
        gfx_rapi->set_zmode_decal(zmode_decal);
        rendering_state.decal_mode = zmode_decal;
    }
    
    if (rdp.viewport_or_scissor_changed) {
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush();
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush();
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }
    
    const uint32_t cycle_type = (rdp.other_mode_h >> G_MDSFT_CYCLETYPE) & 3;
    const bool two_cycle = cycle_type == 1;
    uint64_t cc_id = rdp.combine_mode;
    f3d_log_once(rdp.combine_mode * 0x9E3779B97F4A7C15ull ^ ((uint64_t)rdp.other_mode_l << 20) ^ ((uint64_t)rdp.other_mode_h << 3) ^ ((uint64_t)rsp.geometry_mode << 41),
                 "[fast3d] draw: combine=%07x:%07x cycle=%u omL=%08x omH=%08x geom=%08x lights=%u\n",
                 (uint32_t)(rdp.combine_mode >> 28), (uint32_t)rdp.combine_mode & 0xFFFFFFF, cycle_type, rdp.other_mode_l, rdp.other_mode_h, rsp.geometry_mode, rsp.current_num_lights);
    
    // The framebuffer blend is only needed when the blender mixes with what is already there by (1 - alpha), i.e.
    // P*A + M*(1-A) with M = CLR_MEM (the *_XLU_* render modes). Opaque modes (including the AA ones that use A_MEM
    // coverage, fog-only blends and decals) must not take the combiner's alpha as translucency. The final blender
    // cycle holds the fields: cycle 2 in two-cycle mode, cycle 1 otherwise.
    const uint32_t m2a_shift = two_cycle ? 20 : 22, m2b_shift = two_cycle ? 16 : 18;
    bool use_alpha = ((rdp.other_mode_l >> m2a_shift) & 3) == 1 /*G_BL_CLR_MEM*/ && ((rdp.other_mode_l >> m2b_shift) & 3) == 0 /*G_BL_1MA*/;
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    // Alpha cutouts: coverage-times-alpha modes and the alpha-compare threshold both discard transparent texels.
    bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA || (rdp.other_mode_l & 3) == 1 /*G_AC_THRESHOLD*/;
    bool use_noise = (rdp.other_mode_l & G_AC_DITHER) == G_AC_DITHER;
    
    if (two_cycle) cc_id |= SHADER_OPT_TWO_CYCLE;
    if (use_alpha) cc_id |= SHADER_OPT_ALPHA;
    if (use_fog) cc_id |= SHADER_OPT_FOG;
    if (texture_edge) cc_id |= SHADER_OPT_TEXTURE_EDGE;
    if (use_noise) cc_id |= SHADER_OPT_NOISE;
    
    dbg_last_cc_id = cc_id;
    struct ColorCombiner *comb = gfx_lookup_or_create_color_combiner(cc_id);
    struct ShaderProgram *prg = comb->prg;
    if (prg != rendering_state.shader_program) {
        gfx_flush();
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(prg);
        rendering_state.shader_program = prg;
    }
    if (use_alpha != rendering_state.alpha_blend) {
        gfx_flush();
        gfx_rapi->set_use_alpha(use_alpha);
        rendering_state.alpha_blend = use_alpha;
    }
    uint8_t num_inputs;
    bool used_textures[2];
    gfx_rapi->shader_get_info(prg, &num_inputs, used_textures);
    
    for (int i = 0; i < 2; i++) {
        if (used_textures[i]) {
            if (rdp.textures_changed[i]) {
                gfx_flush();
                import_texture(i);
                rdp.textures_changed[i] = false;
            }
            const struct TileDesc *st = &rdp.tiles[(tex_level + i) & 7];
            bool linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
            if (linear_filter != rendering_state.textures[i]->linear_filter || st->cms != rendering_state.textures[i]->cms || st->cmt != rendering_state.textures[i]->cmt) {
                gfx_flush();
                gfx_rapi->set_sampler_parameters(i, linear_filter, st->cms, st->cmt);
                rendering_state.textures[i]->linear_filter = linear_filter;
                rendering_state.textures[i]->cms = st->cms;
                rendering_state.textures[i]->cmt = st->cmt;
            }
        }
    }
    
    bool use_texture = used_textures[0] || used_textures[1];
    // Texture coordinates are scaled by the first texture the shader uses (TEXEL1 shares them).
    const struct TileDesc *prim_tile = &rdp.tiles[(tex_level + (used_textures[0] ? 0 : 1)) & 7];
    uint32_t tex_x0, tex_y0, tex_width, tex_height;
    tile_dims(prim_tile, &tex_x0, &tex_y0, &tex_width, &tex_height);
    if (tex_width == 0) tex_width = 1;
    if (tex_height == 0) tex_height = 1;
    
    bool z_is_from_0_to_1 = gfx_rapi->z_is_from_0_to_1();
    
    for (int i = 0; i < 3; i++) {
        float z = v_arr[i]->z, w = v_arr[i]->w;
        if (z_is_from_0_to_1) {
            z = (z + w) / 2.0f;
        }
        buf_vbo[buf_vbo_len++] = v_arr[i]->x;
        buf_vbo[buf_vbo_len++] = v_arr[i]->y;
        buf_vbo[buf_vbo_len++] = z;
        buf_vbo[buf_vbo_len++] = w;
        
        if (use_texture) {
            float u = (v_arr[i]->u * tile_shift_scale(prim_tile->shifts) - prim_tile->uls * 8) / 32.0f;
            float v = (v_arr[i]->v * tile_shift_scale(prim_tile->shiftt) - prim_tile->ult * 8) / 32.0f;
            if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
                // Linear filter adds 0.5f to the coordinates
                u += 0.5f;
                v += 0.5f;
            }
            buf_vbo[buf_vbo_len++] = u / tex_width;
            buf_vbo[buf_vbo_len++] = v / tex_height;
        }
        
        if (use_fog) {
            buf_vbo[buf_vbo_len++] = rdp.fog_color.r / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.g / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.b / 255.0f;
            buf_vbo[buf_vbo_len++] = v_arr[i]->color.a / 255.0f; // fog factor (not alpha)
        }
        
        for (int j = 0; j < comb->num_inputs; j++) {
            float x[4];
            switch (comb->input_kind[j]) {
                case CCIN_SHADE:
                    x[0] = v_arr[i]->color.r / 255.0f;
                    x[1] = v_arr[i]->color.g / 255.0f;
                    x[2] = v_arr[i]->color.b / 255.0f;
                    x[3] = use_fog ? 1.0f : v_arr[i]->color.a / 255.0f; // with fog the vertex alpha holds the fog factor
                    break;
                case CCIN_PRIM:
                    x[0] = rdp.prim_color.r / 255.0f;
                    x[1] = rdp.prim_color.g / 255.0f;
                    x[2] = rdp.prim_color.b / 255.0f;
                    x[3] = rdp.prim_color.a / 255.0f;
                    break;
                case CCIN_ENV:
                    x[0] = rdp.env_color.r / 255.0f;
                    x[1] = rdp.env_color.g / 255.0f;
                    x[2] = rdp.env_color.b / 255.0f;
                    x[3] = rdp.env_color.a / 255.0f;
                    break;
                default:
                {
                    float distance_frac = (v1->w - 3000.0f) / 3000.0f;
                    if (distance_frac < 0.0f) distance_frac = 0.0f;
                    if (distance_frac > 1.0f) distance_frac = 1.0f;
                    x[0] = distance_frac;
                    x[1] = rdp.prim_lod_frac / 255.0f;
                    x[2] = x[3] = 0.0f;
                    break;
                }
            }
            memcpy(&buf_vbo[buf_vbo_len], x, sizeof(x));
            buf_vbo_len += 4;
        }
        /*struct RGBA *color = &v_arr[i]->color;
        buf_vbo[buf_vbo_len++] = color->r / 255.0f;
        buf_vbo[buf_vbo_len++] = color->g / 255.0f;
        buf_vbo[buf_vbo_len++] = color->b / 255.0f;
        buf_vbo[buf_vbo_len++] = color->a / 255.0f;*/
    }
    if (++buf_vbo_num_tris == MAX_BUFFERED) {
        gfx_flush();
    }
}

static void gfx_sp_geometry_mode(uint32_t clear, uint32_t set) {
    rsp.geometry_mode &= ~clear;
    rsp.geometry_mode |= set;
}

static void gfx_calc_and_set_viewport(const Vp_t *viewport) {
    // 2 bits fraction
    float width = 2.0f * viewport->vscale[0] / 4.0f;
    float height = 2.0f * viewport->vscale[1] / 4.0f;
    float x = (viewport->vtrans[0] / 4.0f) - width / 2.0f;
    float y = SCREEN_HEIGHT - ((viewport->vtrans[1] / 4.0f) + height / 2.0f);
    
    width *= RATIO_X;
    height *= RATIO_Y;
    x *= RATIO_X;
    y *= RATIO_Y;
    
    rdp.viewport.x = x;
    rdp.viewport.y = y;
    rdp.viewport.width = width;
    rdp.viewport.height = height;
    
    rdp.viewport_or_scissor_changed = true;
}

static void gfx_sp_movemem(uint8_t index, uint8_t offset, const void* data) {
    switch (index) {
        case G_MV_VIEWPORT:
        {
            Vp_t vp;
            f3d_read_vp(g_rdram, (uint32_t)((const uint8_t *)data - g_rdram), &vp);
            gfx_calc_and_set_viewport(&vp);
        }
            break;
#if 0
        case G_MV_LOOKATY:
        case G_MV_LOOKATX:
            memcpy(rsp.current_lookat + (index - G_MV_LOOKATY) / 2, data, sizeof(Light_t));
            //rsp.lights_changed = 1;
            break;
#endif
#ifdef F3DEX_GBI_2
        case G_MV_LIGHT: {
            int lightidx = offset / 24 - 2;
            if (lightidx >= 0 && lightidx <= MAX_LIGHTS) { // skip lookat
                // NOTE: reads out of bounds if it is an ambient light
                f3d_read_light(g_rdram, (uint32_t)((const uint8_t *)data - g_rdram), rsp.current_lights + lightidx);
                rsp.lights_changed = 1; // a light reloaded alone (no new G_NUMLIGHT) still needs its direction redone
            }
            break;
        }
#else
        case G_MV_L0:
        case G_MV_L1:
        case G_MV_L2:
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(rsp.current_lights + (index - G_MV_L0) / 2, data, sizeof(Light_t));
            break;
#endif
    }
}

static void gfx_sp_moveword(uint8_t index, uint16_t offset, uint32_t data) {
    switch (index) {
        case G_MW_NUMLIGHT:
#ifdef F3DEX_GBI_2
            rsp.current_num_lights = data / 24 + 1; // add ambient light
            if (rsp.current_num_lights > MAX_LIGHTS + 1) {
                rsp.current_num_lights = MAX_LIGHTS + 1; // a corrupt count must not index past the light arrays
            }
#else
            // Ambient light is included
            // The 31th bit is a flag that lights should be recalculated
            rsp.current_num_lights = (data - 0x80000000U) / 32;
#endif
            rsp.lights_changed = 1;
            break;
        case G_MW_SEGMENT:
            g_segments[(offset >> 2) & 15] = data & 0x1FFFFFFF; // KSEG0 pointer or physical
            break;
        case G_MW_FOG:
            rsp.fog_mul = (float)(int16_t)(data >> 16);
            rsp.fog_offset = (float)(int16_t)data;
            break;
    }
}

static void gfx_sp_texture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    rsp.texture_scaling_factor.s = sc;
    rsp.texture_scaling_factor.t = tc;
    if (tex_level != (tile & 7)) {
        tex_level = tile & 7;
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
}

static void gfx_dp_set_scissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    float x = ulx / 4.0f * RATIO_X;
    float y = (SCREEN_HEIGHT - lry / 4.0f) * RATIO_Y;
    float width = (lrx - ulx) / 4.0f * RATIO_X;
    float height = (lry - uly) / 4.0f * RATIO_Y;
    
    rdp.scissor.x = x;
    rdp.scissor.y = y;
    rdp.scissor.width = width;
    rdp.scissor.height = height;
    
    rdp.viewport_or_scissor_changed = true;
}

static void gfx_dp_set_texture_image(uint32_t format, uint32_t size, uint32_t width, const void* addr) {
    rdp.timg.addr = addr;
    rdp.timg.siz = size;
    rdp.timg.width = width + 1; // the command holds width - 1
}

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, uint32_t palette, uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts) {
    struct TileDesc *t = &rdp.tiles[tile & 7];
    t->fmt = fmt;
    t->siz = siz;
    t->line = line;
    t->tmem = tmem;
    t->palette = palette;
    t->cms = cms;
    t->cmt = cmt;
    t->masks = masks;
    t->maskt = maskt;
    t->shifts = shifts;
    t->shiftt = shiftt;
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}

static void gfx_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    struct TileDesc *t = &rdp.tiles[tile & 7];
    t->uls = uls;
    t->ult = ult;
    t->lrs = lrs;
    t->lrt = lrt;
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}

// G_LOADTLUT: (count) 16-bit entries from the texture image into palette slots starting at the tile's TMEM word.
// `count_minus_1` is bits 14..23 of word 1, which the gDPLoadTLUT macros fill with (count - 1) as a plain integer
// (15 for pal16, 255 for pal256). The hardware reads the same bits as the high 8 bits of a 10.2 coordinate, so it is
// NOT shifted down again here; doing so loaded a quarter of every palette.
static void gfx_dp_load_tlut(uint8_t tile, uint32_t count_minus_1) {
    const struct TileDesc *t = &rdp.tiles[tile & 7];
    if (rdp.timg.addr == NULL) {
        return;
    }
    uint32_t entries = count_minus_1 + 1;
    uint32_t slot = t->tmem >= 256 ? t->tmem - 256 : 0;
    uint32_t src = (uint32_t)(rdp.timg.addr - g_rdram);
    for (uint32_t i = 0; i < entries && slot + i < 256; i++) {
        uint8_t b[2];
        rdram_copy(b, src + i * 2, 2);
        tlut[slot + i] = (uint16_t)(b[0] << 8 | b[1]);
    }
    tlut_hash = gfx_fnv1a(0xcbf29ce484222325ULL, (const uint8_t *)tlut, sizeof(tlut));
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}

// G_LOADBLOCK: a linear run of texels into TMEM at the tile's address.
static void gfx_dp_load_block(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    const struct TileDesc *t = &rdp.tiles[tile & 7];
    if (rdp.timg.addr == NULL || lrs < uls) {
        return;
    }
    uint32_t bits = k_siz_bits[rdp.timg.siz];
    uint32_t bytes = ((lrs - uls + 1) * bits + 7) / 8;
    uint32_t src = (uint32_t)(rdp.timg.addr - g_rdram) + ((ult * rdp.timg.width + uls) * bits) / 8;
    uint32_t dst = (uint32_t)t->tmem * 8;
    if (dst >= sizeof(tmem)) {
        return;
    }
    if (bytes > sizeof(tmem) - dst) {
        bytes = sizeof(tmem) - dst;
    }
    rdram_copy(tmem + dst, src, bytes);
    // TMEM stores the odd lines of a texture with the two 32-bit halves of every 64-bit word swapped. LOADBLOCK finds the
    // line by accumulating dxt (2048 per line) over the words it moves; dxt 0 never leaves line 0, so those textures
    // arrive unswapped (the game ships them pre-swapped, and the sampler's swap below puts them right).
    if (dxt != 0) {
        for (uint32_t q = 0; (q + 1) * 8 <= bytes; q++) {
            if (((q * dxt) >> 11) & 1) {
                uint8_t *w = tmem + dst + q * 8, tmp[4];
                memcpy(tmp, w, 4);
                memcpy(w, w + 4, 4);
                memcpy(w + 4, tmp, 4);
            }
        }
    }
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}

// G_LOADTILE: a rectangle of the texture image, row by row, at the load tile's line stride. It also sets that tile's size.
static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    struct TileDesc *t = &rdp.tiles[tile & 7];
    t->uls = uls;
    t->ult = ult;
    t->lrs = lrs;
    t->lrt = lrt;
    if (rdp.timg.addr == NULL || lrs < uls || lrt < ult) {
        return;
    }
    uint32_t bits = k_siz_bits[rdp.timg.siz];
    uint32_t w = ((lrs - uls) >> 2) + 1;
    uint32_t h = ((lrt - ult) >> 2) + 1;
    uint32_t src_stride = (rdp.timg.width * bits + 7) / 8;
    uint32_t row_bytes = (w * bits + 7) / 8;
    uint32_t dst_stride = tile_stride(t) != 0 ? tile_stride(t) : row_bytes;
    uint32_t src = (uint32_t)(rdp.timg.addr - g_rdram) + (ult >> 2) * src_stride + ((uls >> 2) * bits) / 8;
    for (uint32_t r = 0; r < h; r++) {
        uint32_t dst = (uint32_t)t->tmem * 8 + r * dst_stride;
        if (dst + row_bytes > sizeof(tmem)) {
            break;
        }
        rdram_copy(tmem + dst, src + r * src_stride, row_bytes);
        if (r & 1) { // odd lines are stored at TMEM address ^ 4: each 64-bit word's 32-bit halves swapped (see gfx_dp_load_block)
            uint8_t row[2048];
            const uint32_t n = row_bytes < sizeof(row) ? row_bytes : (uint32_t)sizeof(row);
            memcpy(row, tmem + dst, n);
            memset(tmem + dst, 0, n);
            for (uint32_t o = 0; o < n; o++) {
                if (dst + (o ^ 4) < sizeof(tmem)) {
                    tmem[dst + (o ^ 4)] = row[o];
                }
            }
        }
    }
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}


// Combiner for the fixed-function cases: d only (color and alpha), everything else 0.
static uint64_t cc_only(uint32_t rgb_d, uint32_t alpha_d) {
    return gfx_cc_pack_cycle(15, 15, 31, rgb_d, 7, 7, 7, alpha_d);
}

static void gfx_dp_set_combine_mode(uint64_t id) {
    rdp.combine_mode = id;
}

static void gfx_dp_set_env_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.env_color.r = r;
    rdp.env_color.g = g;
    rdp.env_color.b = b;
    rdp.env_color.a = a;
}

static void gfx_dp_set_prim_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.prim_color.r = r;
    rdp.prim_color.g = g;
    rdp.prim_color.b = b;
    rdp.prim_color.a = a;
}

static void gfx_dp_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.fog_color.r = r;
    rdp.fog_color.g = g;
    rdp.fog_color.b = b;
    rdp.fog_color.a = a;
}

static void gfx_dp_set_fill_color(uint32_t packed_color) {
    uint16_t col16 = (uint16_t)packed_color;
    uint32_t r = col16 >> 11;
    uint32_t g = (col16 >> 6) & 0x1f;
    uint32_t b = (col16 >> 1) & 0x1f;
    uint32_t a = col16 & 1;
    rdp.fill_color.r = SCALE_5_8(r);
    rdp.fill_color.g = SCALE_5_8(g);
    rdp.fill_color.b = SCALE_5_8(b);
    rdp.fill_color.a = a * 255;
}

static void gfx_draw_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    dbg_note_rect(ulx, uly, lrx, lry);
    uint32_t saved_other_mode_h = rdp.other_mode_h;
    uint32_t cycle_type = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));
    
    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_POINT;
    }
    
    // U10.2 coordinates
    float ulxf = ulx;
    float ulyf = uly;
    float lrxf = lrx;
    float lryf = lry;
    
    ulxf = ulxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    ulyf = -(ulyf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;
    lrxf = lrxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    lryf = -(lryf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;
    
    ulxf = gfx_adjust_x_for_aspect_ratio(ulxf);
    lrxf = gfx_adjust_x_for_aspect_ratio(lrxf);
    
    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];
    
    ul->x = ulxf;
    ul->y = ulyf;
    ul->z = -1.0f;
    ul->w = 1.0f;
    
    ll->x = ulxf;
    ll->y = lryf;
    ll->z = -1.0f;
    ll->w = 1.0f;
    
    lr->x = lrxf;
    lr->y = lryf;
    lr->z = -1.0f;
    lr->w = 1.0f;
    
    ur->x = lrxf;
    ur->y = ulyf;
    ur->z = -1.0f;
    ur->w = 1.0f;
    
    // The coordinates for texture rectangle shall bypass the viewport setting
    struct XYWidthHeight default_viewport = {0, 0, gfx_current_dimensions.width, gfx_current_dimensions.height};
    struct XYWidthHeight viewport_saved = rdp.viewport;
    uint32_t geometry_mode_saved = rsp.geometry_mode;
    
    rdp.viewport = default_viewport;
    rdp.viewport_or_scissor_changed = true;
    rsp.geometry_mode = 0;
    
    gfx_sp_tri1(MAX_VERTICES + 0, MAX_VERTICES + 1, MAX_VERTICES + 3);
    gfx_sp_tri1(MAX_VERTICES + 1, MAX_VERTICES + 2, MAX_VERTICES + 3);
    
    rsp.geometry_mode = geometry_mode_saved;
    rdp.viewport = viewport_saved;
    rdp.viewport_or_scissor_changed = true;
    
    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = saved_other_mode_h;
    }
}

static void gfx_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    uint64_t saved_combine_mode = rdp.combine_mode;
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1 instead
        dsdx >>= 2;
        
        // Color combiner is turned off in copy mode
        gfx_dp_set_combine_mode(cc_only(1 /*TEXEL0*/, 1 /*TEXEL0 alpha*/));
        
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    
    // uls and ult are S10.5
    // dsdx and dtdy are S5.10
    // lrx, lry, ulx, uly are U10.2
    // lrs, lrt are S10.5
    if (flip) {
        dsdx = -dsdx;
        dtdy = -dtdy;
    }
    int16_t width = !flip ? lrx - ulx : lry - uly;
    int16_t height = !flip ? lry - uly : lrx - ulx;
    float lrs = ((uls << 7) + dsdx * width) >> 7;
    float lrt = ((ult << 7) + dtdy * height) >> 7;
    
    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];
    float u0 = uls, v0 = ult, u1 = lrs, v1 = lrt;
    // The RDP samples a 1:1 rectangle exactly on texel centers, so bilinear never reaches outside it. Drawn larger than
    // 320x240, GL's bilinear filter blends the rectangle's edge pixels with the texels next to it (the next glyph, or the
    // far side of a wrapped tile). Pulling each end in by half a texel keeps every sample inside the rectangle's texels.
    if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT && (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) != G_CYC_COPY &&
        gfx_current_dimensions.height > 240 + 8) {
        const float half = 16.0f; // half a texel in S10.5
        if (u1 > u0 + 2 * half) { u0 += half; u1 -= half; } else if (u0 > u1 + 2 * half) { u0 -= half; u1 += half; }
        if (v1 > v0 + 2 * half) { v0 += half; v1 -= half; } else if (v0 > v1 + 2 * half) { v0 -= half; v1 += half; }
    }
    ul->u = u0;
    ul->v = v0;
    lr->u = u1;
    lr->v = v1;
    if (!flip) {
        ll->u = u0;
        ll->v = v1;
        ur->u = u1;
        ur->v = v0;
    } else {
        ll->u = u1;
        ll->v = v0;
        ur->u = u0;
        ur->v = v1;
    }
    
    f3d_log_once(0x3000000000ULL ^ ((uint64_t)tile << 40) ^ rdp.other_mode_l ^ ((uint64_t)rdp.other_mode_h << 7) ^ (rdp.combine_mode * 0x9E3779B97F4A7C15ull),
                 "[fast3d] texrect: tile=%u fmt=%u siz=%u cycle=%u omL=%08x omH=%08x combine=%07x\n", tile & 7, rdp.tiles[tile & 7].fmt,
                 rdp.tiles[tile & 7].siz, (rdp.other_mode_h >> G_MDSFT_CYCLETYPE) & 3, rdp.other_mode_l, rdp.other_mode_h,
                 (uint32_t)rdp.combine_mode & 0xFFFFFFF);
    // The rectangle names its own tile; G_TEXTURE (which triangles use) may have selected another one.
    const uint8_t saved_tex_level = tex_level;
    if ((tile & 7) != tex_level) {
        gfx_flush();
        tex_level = tile & 7;
        rdp.textures_changed[0] = rdp.textures_changed[1] = true;
    }
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    if (tex_level != saved_tex_level) {
        gfx_flush();
        tex_level = saved_tex_level;
        rdp.textures_changed[0] = rdp.textures_changed[1] = true;
    }
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    if (rdp.color_image_address == rdp.z_buf_address) {
        // Don't clear Z buffer here since we already did it with glClear
        return;
    }
    uint32_t mode = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));
    
    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    
    if (mode != G_CYC_COPY && mode != G_CYC_FILL) {
        // In 1- and 2-cycle mode a FILLRECT goes through the combiner and blender like any other primitive (the car select's
        // color swatches are FILLRECTs colored by PRIMITIVE), with no texture and no shade: it does not use the fill color.
        for (int i = MAX_VERTICES; i < MAX_VERTICES + 4; i++) {
            struct LoadedVertex* v = &rsp.loaded_vertices[i];
            v->color.r = v->color.g = v->color.b = v->color.a = 1.0f;
        }
        f3d_log_once(rdp.combine_mode ^ ((uint64_t)rdp.other_mode_l << 32), "[fast3d] 1/2-cycle fillrect: combine=%07x:%07x omL=%08x prim=%08x\n",
                     (uint32_t)(rdp.combine_mode >> 28), (uint32_t)rdp.combine_mode & 0xFFFFFFF, rdp.other_mode_l, 0, 0, 0, 0);
        gfx_draw_rectangle(ulx, uly, lrx, lry);
        return;
    }

    for (int i = MAX_VERTICES; i < MAX_VERTICES + 4; i++) {
        struct LoadedVertex* v = &rsp.loaded_vertices[i];
        v->color = rdp.fill_color;
    }

    uint64_t saved_combine_mode = rdp.combine_mode;
    gfx_dp_set_combine_mode(cc_only(4 /*SHADE*/, 4 /*SHADE alpha*/));
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_set_z_image(void *z_buf_address) {
    rdp.z_buf_address = z_buf_address;
}

static void gfx_dp_set_color_image(uint32_t format, uint32_t size, uint32_t width, void* address) {
    rdp.color_image_address = address;
}

static void gfx_sp_set_other_mode(uint32_t shift, uint32_t num_bits, uint64_t mode) {
    uint64_t mask = (((uint64_t)1 << num_bits) - 1) << shift;
    uint64_t om = rdp.other_mode_l | ((uint64_t)rdp.other_mode_h << 32);
    om = (om & ~mask) | mode;
    if ((((uint64_t)rdp.other_mode_h << 32) ^ om) & ((uint64_t)3 << (32 + 14))) {
        rdp.textures_changed[0] = rdp.textures_changed[1] = true; // palette type changed: CI textures decode differently
    }
    rdp.other_mode_l = (uint32_t)om;
    rdp.other_mode_h = (uint32_t)(om >> 32);
}

// Resolves a display list address to a host pointer into RDRAM. A KSEG0/KSEG1 pointer (bit 31 set, e.g. 0x81234567) is a
// direct physical address: the recompiled game's heap lives above 16 MB (0x01000000..0x1FFFFFFF), which the 24-bit
// segment offset cannot reach, and reading such a pointer as "segment 1, offset 0x234567" returned unrelated low memory
// (noise textures, wrong vertices). Anything else is segment number (bits 24-27) + 24-bit offset. Masked to 512 MB.
#define F3D_PHYS_MASK 0x1FFFFFFFu
static inline void *seg_addr(uintptr_t w1) {
    uint32_t a = (uint32_t)w1;
    if (a & 0x80000000u) {
        return g_rdram + (a & F3D_PHYS_MASK);
    }
    uint32_t seg = (a >> 24) & 0x0F;
    return g_rdram + ((g_segments[seg] + (a & 0x00FFFFFFu)) & F3D_PHYS_MASK);
}

#define DL_MAX_DEPTH 18 // the RSP's display list stack depth
static int dl_depth;
static uint32_t dl_cmd_budget; // stops a display list that branches to itself

#define C0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << width) - 1))
#define C1(pos, width) ((cmd->words.w1 >> (pos)) & ((1U << width) - 1))


// ---- RT64 extended GBI --------------------------------------------------------------------------------------------
// The recomp's own code (HUD placement, draw distance, interpolation, pause menu) writes RT64 extended commands into
// the display lists: opcode 0x64 with a 24-bit id, followed by 0 to 2 more 8-byte words whose contents are raw data.
// Every word has to be consumed or those data words run as commands (an alignment offset of -960 is 0xFC40...: a
// G_SETCOMBINE with garbage). Implemented: float matrices, the viewport command, float fog, and the push/pop state
// stacks. Everything else (rect/viewport/scissor alignment origins, matrix groups for interpolation, car reflections,
// sky fog) is consumed and ignored: the whole window is treated as the 4:3 area the game draws in.
#define F3D_EX_STACK_DEPTH 8
enum { EXS_OTHERMODE, EXS_COMBINE, EXS_ENV, EXS_FOG, EXS_FILL, EXS_PRIM, EXS_GEOM, EXS_SCISSOR, EXS_VIEWPORT, EXS_PROJ, EXS_COUNT };
static struct ExSaved {
    uint32_t om_l, om_h, geom;
    uint64_t cc;
    struct RGBA color;
    uint8_t prim_lod;
    struct XYWidthHeight rect;
    float proj[4][4];
} ex_stack[EXS_COUNT][F3D_EX_STACK_DEPTH];
static int ex_depth[EXS_COUNT];

static void gfx_ex_stack(int cat, bool push) {
    if (push) {
        if (ex_depth[cat] >= F3D_EX_STACK_DEPTH) {
            return;
        }
        struct ExSaved *e = &ex_stack[cat][ex_depth[cat]++];
        switch (cat) {
            case EXS_OTHERMODE: e->om_l = rdp.other_mode_l; e->om_h = rdp.other_mode_h; break;
            case EXS_COMBINE: e->cc = rdp.combine_mode; break;
            case EXS_ENV: e->color = rdp.env_color; break;
            case EXS_FOG: e->color = rdp.fog_color; break;
            case EXS_FILL: e->color = rdp.fill_color; break;
            case EXS_PRIM: e->color = rdp.prim_color; e->prim_lod = rdp.prim_lod_frac; break;
            case EXS_GEOM: e->geom = rsp.geometry_mode; break;
            case EXS_SCISSOR: e->rect = rdp.scissor; break;
            case EXS_VIEWPORT: e->rect = rdp.viewport; break;
            case EXS_PROJ: memcpy(e->proj, rsp.P_matrix, sizeof(e->proj)); break;
        }
        return;
    }
    if (ex_depth[cat] <= 0) {
        return;
    }
    struct ExSaved *e = &ex_stack[cat][--ex_depth[cat]];
    switch (cat) {
        case EXS_OTHERMODE:
            rdp.other_mode_l = e->om_l;
            rdp.other_mode_h = e->om_h;
            rdp.textures_changed[0] = rdp.textures_changed[1] = true;
            break;
        case EXS_COMBINE: rdp.combine_mode = e->cc; break;
        case EXS_ENV: rdp.env_color = e->color; break;
        case EXS_FOG: rdp.fog_color = e->color; break;
        case EXS_FILL: rdp.fill_color = e->color; break;
        case EXS_PRIM: rdp.prim_color = e->color; rdp.prim_lod_frac = e->prim_lod; break;
        case EXS_GEOM: rsp.geometry_mode = e->geom; break;
        case EXS_SCISSOR: rdp.scissor = e->rect; rdp.viewport_or_scissor_changed = true; break;
        case EXS_VIEWPORT: rdp.viewport = e->rect; rdp.viewport_or_scissor_changed = true; break;
        case EXS_PROJ:
            memcpy(rsp.P_matrix, e->proj, sizeof(e->proj));
            gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
            break;
    }
}

// Runs the extended command at cmd and returns how many 8-byte words it occupies.
static void dbg_count_ext(void);
static void dbg_count_unknown(void);
static int gfx_run_ex(const Gfx *cmd) {
    dbg_count_ext();
    const uint32_t id = cmd->words.w0 & 0xFFFFFF;
    switch (id) {
        case 0x30: { // G_EX_MATRIX_FLOAT: params (G_MTX flags with push inverted) here, float matrix address in word 2
            float m[4][4];
            memcpy(m, seg_addr(cmd[1].words.w1), sizeof(m));
            gfx_sp_matrix_apply((uint8_t)((cmd->words.w1 & 0xFF) ^ G_MTX_PUSH), m);
            return 2;
        }
        case 0x04: { // G_EX_SETVIEWPORT: origin here, Vp address in word 2
            Vp_t vp;
            f3d_read_vp(g_rdram, (uint32_t)((const uint8_t *)seg_addr(cmd[1].words.w1) - g_rdram), &vp);
            gfx_calc_and_set_viewport(&vp);
            return 2;
        }
        case 0x34: { // G_EX_SETFOGFLOAT (recomp addition): fog = z / w * mul + offset in G_MW_FOG units, as floats
            uint32_t mul_bits = (uint32_t)cmd[1].words.w0, offset_bits = (uint32_t)cmd[1].words.w1;
            memcpy(&rsp.fog_mul, &mul_bits, sizeof(float));
            memcpy(&rsp.fog_offset, &offset_bits, sizeof(float));
            return 2;
        }
        case 0x15: gfx_ex_stack(EXS_VIEWPORT, true); return 1;
        case 0x16: gfx_ex_stack(EXS_VIEWPORT, false); return 1;
        case 0x17: gfx_ex_stack(EXS_SCISSOR, true); return 1;
        case 0x18: gfx_ex_stack(EXS_SCISSOR, false); return 1;
        case 0x19: gfx_ex_stack(EXS_OTHERMODE, true); return 1;
        case 0x1A: gfx_ex_stack(EXS_OTHERMODE, false); return 1;
        case 0x1B: gfx_ex_stack(EXS_COMBINE, true); return 1;
        case 0x1C: gfx_ex_stack(EXS_COMBINE, false); return 1;
        case 0x1D: gfx_ex_stack(EXS_PROJ, true); return 1;
        case 0x1E: gfx_ex_stack(EXS_PROJ, false); return 1;
        case 0x1F: gfx_ex_stack(EXS_ENV, true); return 1;
        case 0x20: gfx_ex_stack(EXS_ENV, false); return 1;
        case 0x23: gfx_ex_stack(EXS_FOG, true); return 1;
        case 0x24: gfx_ex_stack(EXS_FOG, false); return 1;
        case 0x25: gfx_ex_stack(EXS_FILL, true); return 1;
        case 0x26: gfx_ex_stack(EXS_FILL, false); return 1;
        case 0x27: gfx_ex_stack(EXS_PRIM, true); return 1;
        case 0x28: gfx_ex_stack(EXS_PRIM, false); return 1;
        case 0x29: gfx_ex_stack(EXS_GEOM, true); return 1;
        case 0x2A: gfx_ex_stack(EXS_GEOM, false); return 1;
        // Consumed and ignored; the word counts are those of the gEX* macros in rt64_extended_gbi.h.
        case 0x02: case 0x08: return 3;                                          // texture rectangle, scissor align
        case 0x05: case 0x06: case 0x07: case 0x0C: case 0x13: case 0x14: case 0x31: return 2; // scissor, rect/viewport align, matrix group, edit group, vertex, vertex segment
        default:
            if (id >= 0x38) {
                dbg_count_unknown();
                f3d_log_once(0x4000000000ULL | id, "[fast3d] unknown RT64 extended command 0x%06x%.0u%.0u%.0u%.0u%.0u%.0u\n", id, 0, 0, 0, 0, 0, 0);
            }
            return 1;
    }
}


// ---- debugging: frame dumps, command traces and per-frame statistics ------------------------------------------------
// RUSH2_F3D_DUMP_FRAMES=120,300  dumps those frames (counted from 1, one per gfx_begin_frame); creating the file
// <dump dir>/f3d_dump_now dumps the next frame (checked every 20 frames, the file is deleted). RUSH2_F3D_DUMP_DIR is the
// output directory (default "."). A dump is <dir>/f3d_frame_<n>.bin (non-zero RDRAM pages per task, replayable with
// tools/f3d_replay) and <dir>/f3d_frame_<n>.txt (every command and draw). A stats line goes to stderr for dumped frames
// and every 300th frame.
// .bin layout (little endian): "F3DDUMP2", u32 width, u32 height, then records:
//   u32 'TASK', u32 dl_addr, u32 npages, npages * (u32 page_index, 4096 bytes). Pages are the RDRAM pages that differ
//   from the previous record (the first record is all non-zero pages). Ends at EOF.
#define DBG_RDRAM_BYTES 0x20000000u // default dump range; RUSH2_F3D_DUMP_MB overrides it (the tests use a smaller RDRAM)
#define DBG_PAGE 4096u
static struct {
    unsigned frame;
    bool trace;
    FILE *bin, *txt;
    uint8_t *shadow;
    unsigned stat_tasks, stat_draws, stat_tris, stat_rects, stat_cmds, stat_unknown, stat_ext;
    unsigned want[32];
    int num_want;
    bool parsed;
    uint32_t rdram_bytes;
    char dir[512];
} dbg;

static const char *dbg_opname(uint32_t op) {
    switch (op) {
        case 0x01: return "VTX"; case 0x05: return "TRI1"; case 0x06: return "TRI2"; case 0x07: return "QUAD";
        case 0x09: return "LINE3D"; case 0xD7: return "TEXTURE"; case 0xD9: return "GEOMETRYMODE"; case 0xDA: return "MTX";
        case 0xDB: return "MOVEWORD"; case 0xDC: return "MOVEMEM"; case 0xDE: return "DL"; case 0xDF: return "ENDDL";
        case 0xE2: return "SETOTHERMODE_L"; case 0xE3: return "SETOTHERMODE_H"; case 0xE4: return "TEXRECT";
        case 0xE5: return "TEXRECTFLIP"; case 0xE6: return "RDPLOADSYNC"; case 0xE7: return "RDPPIPESYNC";
        case 0xE8: return "RDPTILESYNC"; case 0xE9: return "RDPFULLSYNC"; case 0xEF: return "RDPSETOTHERMODE";
        case 0xF0: return "LOADTLUT"; case 0xF2: return "SETTILESIZE"; case 0xF3: return "LOADBLOCK"; case 0xF4: return "LOADTILE";
        case 0xF5: return "SETTILE"; case 0xF6: return "FILLRECT"; case 0xF7: return "SETFILLCOLOR"; case 0xF8: return "SETFOGCOLOR";
        case 0xF9: return "SETBLENDCOLOR"; case 0xFA: return "SETPRIMCOLOR"; case 0xFB: return "SETENVCOLOR";
        case 0xFC: return "SETCOMBINE"; case 0xFD: return "SETTIMG"; case 0xFE: return "SETZIMG"; case 0xFF: return "SETCIMG";
        case 0x03: return "CULLDL"; case 0x04: return "BRANCH_Z"; case 0xD6: return "DMA_IO"; case 0xD8: return "POPMTX";
        case 0x00: return "NOOP"; case 0x64: return "RT64_EXT"; case 0xE1: return "RDPHALF_1";
        default: return "?";
    }
}

static void dbg_count_ext(void) { dbg.stat_ext++; }
static void dbg_count_unknown(void) { dbg.stat_unknown++; }

static void dbg_trace_cmd(const Gfx *cmd) {
    dbg.stat_cmds++;
    if (dbg.txt != NULL) {
        fprintf(dbg.txt, "%*s%-16s %08X %08X\n", dl_depth * 2, "", dbg_opname(cmd->words.w0 >> 24), cmd->words.w0, cmd->words.w1);
    }
}

static void dbg_note_draw(size_t tris) {
    dbg.stat_draws++;
    dbg.stat_tris += (unsigned)tris;
    if (dbg.txt != NULL) {
        const struct TileDesc *t = &rdp.tiles[0];
        fprintf(dbg.txt, "%*s  => DRAW tris=%u combine=%016llX othermode=%08X/%08X geom=%08X tile0 fmt=%d siz=%d %ux%u cm=%d/%d\n",
                dl_depth * 2, "", (unsigned)tris, (unsigned long long)rdp.combine_mode, rdp.other_mode_h, rdp.other_mode_l,
                rsp.geometry_mode, t->fmt, t->siz, (unsigned)(t->lrs - t->uls) / 4 + 1, (unsigned)(t->lrt - t->ult) / 4 + 1, t->cms, t->cmt);
    }
}

static void dbg_note_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    dbg.stat_rects++;
    if (dbg.txt != NULL) {
        fprintf(dbg.txt, "%*s  => RECT %d,%d .. %d,%d (1/4 px) combine=%016llX othermode=%08X/%08X\n", dl_depth * 2, "", ulx, uly,
                lrx, lry, (unsigned long long)rdp.combine_mode, rdp.other_mode_h, rdp.other_mode_l);
    }
}

static void dbg_parse_env(void) {
    dbg.parsed = true;
    const char *mb = getenv("RUSH2_F3D_DUMP_MB");
    dbg.rdram_bytes = mb != NULL && atoi(mb) > 0 ? (uint32_t)atoi(mb) << 20 : DBG_RDRAM_BYTES;
    const char *dir = getenv("RUSH2_F3D_DUMP_DIR");
    snprintf(dbg.dir, sizeof(dbg.dir), "%s", dir != NULL && dir[0] ? dir : ".");
    const char *list = getenv("RUSH2_F3D_DUMP_FRAMES");
    while (list != NULL && *list != 0 && dbg.num_want < 32) {
        char *end;
        unsigned long v = strtoul(list, &end, 10);
        if (end == list) {
            break;
        }
        dbg.want[dbg.num_want++] = (unsigned)v;
        list = *end == ',' ? end + 1 : end;
    }
}

static bool dbg_should_dump(void) {
    for (int i = 0; i < dbg.num_want; i++) {
        if (dbg.want[i] == dbg.frame) {
            return true;
        }
    }
    if (dbg.frame % 20 == 0) {
        char path[640];
        snprintf(path, sizeof(path), "%s/f3d_dump_now", dbg.dir);
        FILE *f = fopen(path, "rb");
        if (f != NULL) {
            fclose(f);
            remove(path);
            return true;
        }
    }
    return false;
}

static void dbg_begin_frame(void) {
    if (!dbg.parsed) {
        dbg_parse_env();
    }
    dbg.frame++;
    dbg.stat_tasks = dbg.stat_draws = dbg.stat_tris = dbg.stat_rects = dbg.stat_cmds = dbg.stat_unknown = dbg.stat_ext = 0;
    if (!dbg_should_dump()) {
        return;
    }
    char path[640];
    snprintf(path, sizeof(path), "%s/f3d_frame_%u.bin", dbg.dir, dbg.frame);
    dbg.bin = fopen(path, "wb");
    snprintf(path, sizeof(path), "%s/f3d_frame_%u.txt", dbg.dir, dbg.frame);
    dbg.txt = fopen(path, "w");
    if (dbg.bin == NULL) {
        fprintf(stderr, "[fast3d] cannot write the frame dump in %s\n", dbg.dir);
        if (dbg.txt != NULL) fclose(dbg.txt);
        dbg.txt = NULL;
        return;
    }
    dbg.trace = true;
    if (dbg.shadow == NULL) {
        dbg.shadow = (uint8_t *)calloc(1, dbg.rdram_bytes); // untouched pages cost no memory
    }
    if (dbg.shadow == NULL) {
        fclose(dbg.bin);
        dbg.bin = NULL;
        return;
    }
    memset(dbg.shadow, 0, 0); // shadow is re-zeroed lazily below
    uint32_t hdr[2] = {gfx_current_dimensions.width, gfx_current_dimensions.height};
    fwrite("F3DDUMP2", 1, 8, dbg.bin);
    fwrite(hdr, 4, 2, dbg.bin);
    fprintf(stderr, "[fast3d] dumping frame %u to %s/f3d_frame_%u.bin\n", dbg.frame, dbg.dir, dbg.frame);
}

// Writes the RDRAM pages that changed since the previous record.
static void dbg_dump_task(uint32_t dl_addr) {
    if (dbg.bin == NULL) {
        return;
    }
    // The game threads keep writing RDRAM while this runs, so the changed pages are listed once and exactly that list is
    // written (counting and writing in two scans produced a record with more pages than its header said).
    const uint32_t pages = dbg.rdram_bytes / DBG_PAGE;
    uint32_t *list = (uint32_t *)malloc(sizeof(uint32_t) * pages);
    if (list == NULL) {
        return;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < pages; i++) {
        if (memcmp(g_rdram + (size_t)i * DBG_PAGE, dbg.shadow + (size_t)i * DBG_PAGE, DBG_PAGE) != 0) {
            list[count++] = i;
        }
    }
    uint32_t head[3] = {0x4B534154u /* "TASK" */, dl_addr, count};
    fwrite(head, 4, 3, dbg.bin);
    for (uint32_t k = 0; k < count; k++) {
        const uint32_t i = list[k];
        uint8_t page[DBG_PAGE];
        memcpy(page, g_rdram + (size_t)i * DBG_PAGE, DBG_PAGE);
        fwrite(&i, 4, 1, dbg.bin);
        fwrite(page, 1, DBG_PAGE, dbg.bin);
        memcpy(dbg.shadow + (size_t)i * DBG_PAGE, page, DBG_PAGE);
    }
    free(list);
}

static void dbg_end_frame(void) {
    const bool dumping = dbg.bin != NULL || dbg.txt != NULL;
    if (dumping || dbg.frame % 300 == 0) {
        fprintf(stderr, "[fast3d] frame %u: %u tasks, %u cmds, %u draws (%u tris), %u rects, %u unknown ops, %u extended cmds\n",
                dbg.frame, dbg.stat_tasks, dbg.stat_cmds, dbg.stat_draws, dbg.stat_tris, dbg.stat_rects, dbg.stat_unknown, dbg.stat_ext);
    }
    if (dbg.bin != NULL) fclose(dbg.bin);
    if (dbg.txt != NULL) fclose(dbg.txt);
    dbg.bin = NULL;
    dbg.txt = NULL;
    dbg.trace = false;
    if (dbg.shadow != NULL) {
        free(dbg.shadow); // the next dump starts from a full snapshot again
        dbg.shadow = NULL;
    }
}

static void gfx_run_dl(Gfx* cmd) {
    int dummy = 0;
    for (;;) {
        if (dl_cmd_budget == 0) {
            return;
        }
        dl_cmd_budget--;
        uint32_t opcode = cmd->words.w0 >> 24;
        if (dbg.trace) {
            dbg_trace_cmd(cmd);
        }
        switch (opcode) {
            // RSP commands:
            case G_MTX:
#ifdef F3DEX_GBI_2
                gfx_sp_matrix(C0(0, 8) ^ G_MTX_PUSH, (const int32_t *) seg_addr(cmd->words.w1));
#else
                gfx_sp_matrix(C0(16, 8), (const int32_t *) seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_POPMTX:
#ifdef F3DEX_GBI_2
                gfx_sp_pop_matrix(cmd->words.w1 / 64);
#else
                gfx_sp_pop_matrix(1);
#endif
                break;
            case G_MOVEMEM:
#ifdef F3DEX_GBI_2
                gfx_sp_movemem(C0(0, 8), C0(8, 8) * 8, seg_addr(cmd->words.w1));
#else
                gfx_sp_movemem(C0(16, 8), 0, seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_MOVEWORD:
#ifdef F3DEX_GBI_2
                gfx_sp_moveword(C0(16, 8), C0(0, 16), cmd->words.w1);
#else
                gfx_sp_moveword(C0(0, 8), C0(8, 16), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_TEXTURE:
#ifdef F3DEX_GBI_2
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(1, 7));
#else
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(0, 8));
#endif
                break;
            case G_VTX:
#ifdef F3DEX_GBI_2
                gfx_sp_vertex(C0(12, 8), C0(1, 7) - C0(12, 8), seg_addr(cmd->words.w1));
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_vertex(C0(10, 6), C0(16, 8) / 2, seg_addr(cmd->words.w1));
#else
                gfx_sp_vertex((C0(0, 16)) / sizeof(Vtx), C0(16, 4), seg_addr(cmd->words.w1));
#endif
                break;
            case G_DL:
                if (C0(16, 1) == 0) {
                    // Push return address
                    if (dl_depth < DL_MAX_DEPTH) {
                        dl_depth++;
                        gfx_run_dl((Gfx *)seg_addr(cmd->words.w1));
                        dl_depth--;
                    }
                } else {
                    cmd = (Gfx *)seg_addr(cmd->words.w1);
                    --cmd; // increase after break
                }
                break;
            case (uint8_t)G_ENDDL:
                return;
#ifdef F3DEX_GBI_2
            case G_GEOMETRYMODE:
                gfx_sp_geometry_mode(~C0(0, 24), cmd->words.w1);
                break;
#else
            case (uint8_t)G_SETGEOMETRYMODE:
                gfx_sp_geometry_mode(0, cmd->words.w1);
                break;
            case (uint8_t)G_CLEARGEOMETRYMODE:
                gfx_sp_geometry_mode(cmd->words.w1, 0);
                break;
#endif
            case (uint8_t)G_TRI1:
#ifdef F3DEX_GBI_2
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
#else
                gfx_sp_tri1(C1(16, 8) / 10, C1(8, 8) / 10, C1(0, 8) / 10);
#endif
                break;
#if defined(F3DEX_GBI) || defined(F3DLP_GBI)
            case (uint8_t)G_TRI2:
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
                break;
#endif
#ifdef F3DEX_GBI_2
            case G_TRI2:
            case G_QUAD: // G_QUAD's second triangle is (v0, v2, v3), already encoded in w1 by the macro
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
                break;
#endif
            case (uint8_t)G_SETOTHERMODE_L:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(31 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, cmd->words.w1);
#else
                gfx_sp_set_other_mode(C0(8, 8), C0(0, 8), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_SETOTHERMODE_H:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(63 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, (uint64_t) cmd->words.w1 << 32);
#else
                gfx_sp_set_other_mode(C0(8, 8) + 32, C0(0, 8), (uint64_t) cmd->words.w1 << 32);
#endif
                break;
            
            // RDP Commands:
            case G_SETTIMG:
                gfx_dp_set_texture_image(C0(21, 3), C0(19, 2), C0(0, 12), seg_addr(cmd->words.w1)); // width - 1 is 12 bits
                break;
            case G_LOADBLOCK:
                gfx_dp_load_block(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTILE:
                gfx_dp_load_tile(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETTILE:
                gfx_dp_set_tile(C0(21, 3), C0(19, 2), C0(9, 9), C0(0, 9), C1(24, 3), C1(20, 4), C1(18, 2), C1(14, 4), C1(10, 4), C1(8, 2), C1(4, 4), C1(0, 4));
                break;
            case G_SETTILESIZE:
                gfx_dp_set_tile_size(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTLUT:
                gfx_dp_load_tlut(C1(24, 3), C1(14, 10));
                break;
            case G_SETENVCOLOR:
                gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMCOLOR:
                rdp.prim_lod_frac = (uint8_t)C0(0, 8);
                gfx_dp_set_prim_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFOGCOLOR:
                gfx_dp_set_fog_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFILLCOLOR:
                gfx_dp_set_fill_color(cmd->words.w1);
                break;
            case G_SETCOMBINE:
                // Both cycles in the RDP's own field numbering (gfx_cc.h resolves it per slot).
                gfx_dp_set_combine_mode(
                    gfx_cc_pack_cycle(C0(20, 4), C1(28, 4), C0(15, 5), C1(15, 3), C0(12, 3), C1(12, 3), C0(9, 3), C1(9, 3)) |
                    (gfx_cc_pack_cycle(C0(5, 4), C1(24, 4), C0(0, 5), C1(6, 3), C1(21, 3), C1(3, 3), C1(18, 3), C1(0, 3)) << 28));
                break;
            // G_SETPRIMCOLOR, G_CCMUX_PRIMITIVE, G_ACMUX_PRIMITIVE, is used by Goddard
            // G_CCMUX_TEXEL1, LOD_FRACTION is used in Bowser room 1
            case G_TEXRECT:
            case G_TEXRECTFLIP:
            {
                int32_t lrx, lry, tile, ulx, uly;
                uint32_t uls, ult, dsdx, dtdy;
#ifdef F3DEX_GBI_2E
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                uls = C0(16, 16);
                ult = C0(0, 16);
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
#else
                lrx = C0(12, 12);
                lry = C0(0, 12);
                tile = C1(24, 3);
                ulx = C1(12, 12);
                uly = C1(0, 12);
                ++cmd;
                uls = C1(16, 16);
                ult = C1(0, 16);
                ++cmd;
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
#endif
                gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == G_TEXRECTFLIP);
                break;
            }
            case G_FILLRECT:
#ifdef F3DEX_GBI_2E
            {
                int32_t lrx, lry, ulx, uly;
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                gfx_dp_fill_rectangle(ulx, uly, lrx, lry);
                break;
            }
#else
                gfx_dp_fill_rectangle(C1(12, 12), C1(0, 12), C0(12, 12), C0(0, 12));
                break;
#endif
            case 0x64: // RT64_EXTENDED_OPCODE
                cmd += gfx_run_ex(cmd) - 1; // the loop's own increment steps past the last word
                break;
            case 0xEF: // G_RDPSETOTHERMODE: the whole other-mode pair at once (high word in w0's low 24 bits)
                if (rdp.other_mode_h != C0(0, 24) || (rdp.other_mode_l != cmd->words.w1)) {
                    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
                }
                rdp.other_mode_h = C0(0, 24);
                rdp.other_mode_l = cmd->words.w1;
                break;
            case G_SETSCISSOR:
                gfx_dp_set_scissor(C1(24, 2), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETZIMG:
                gfx_dp_set_z_image(seg_addr(cmd->words.w1));
                break;
            case G_SETCIMG:
                gfx_dp_set_color_image(C0(21, 3), C0(19, 2), C0(0, 11), seg_addr(cmd->words.w1));
                break;
            case 0xE0: case 0xE1: case 0xE6: case 0xE7: case 0xE8: case 0xE9: case 0xF1: // no-op, half words, syncs
            case 0xF9: // blend color, not modelled
                break;
            default:
                dbg.stat_unknown++;
                f3d_log_once(0x2000000000ULL | opcode, "[fast3d] unhandled display list command 0x%02x (w0=%08x w1=%08x)%.0u%.0u%.0u%.0u\n",
                             opcode, cmd->words.w0, cmd->words.w1, 0, 0, 0, 0);
                break;
        }
        ++cmd;
    }
}

static void gfx_sp_reset() {
    rsp.modelview_matrix_stack_size = 1;
    rsp.current_num_lights = 2;
    rsp.lights_changed = true;
}

void gfx_init_embedded(struct GfxRenderingAPI *rapi, uint8_t *rdram) {
    gfx_rapi = rapi;
    g_rdram = rdram;
    memset(g_segments, 0, sizeof(g_segments));
    gfx_set_dimensions(SCREEN_WIDTH, SCREEN_HEIGHT);
    gfx_rapi->init();
}

void gfx_set_dimensions(uint32_t width, uint32_t height) {
    gfx_current_dimensions.width = width;
    gfx_current_dimensions.height = height != 0 ? height : 1; // avoid division by zero
    gfx_current_dimensions.aspect_ratio = (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
}

struct GfxRenderingAPI *gfx_get_current_rendering_api(void) {
    return gfx_rapi;
}

void gfx_begin_frame(void) {
    dbg_begin_frame();
    gfx_rapi->start_frame();
}

// Runs one RSP task's display list. dl_addr is the RDRAM address from OSTask::data_ptr.
void gfx_run_task(uint32_t dl_addr) {
    dbg.stat_tasks++;
    dbg_dump_task(dl_addr & 0x1FFFFFFF);
    gfx_sp_reset();
    memset(g_segments, 0, sizeof(g_segments));
    dl_depth = 0;
    dl_cmd_budget = 16u * 1024u * 1024u;
    gfx_run_dl((Gfx *)(g_rdram + (dl_addr & 0x1FFFFFFF)));
    gfx_flush();
}

// Re-applies every piece of GL state the core caches in rendering_state. Something else drew with the same GL context
// since the last display list (the UI overlay), so the cached values no longer say what the context holds. Call it
// after gfx_opengl_restore_state(), before the next display list.
void gfx_reapply_state(void) {
    gfx_rapi->set_depth_test(rendering_state.depth_test);
    gfx_rapi->set_depth_mask(rendering_state.depth_mask);
    gfx_rapi->set_zmode_decal(rendering_state.decal_mode);
    gfx_rapi->set_use_alpha(rendering_state.alpha_blend);
    gfx_rapi->set_viewport(rendering_state.viewport.x, rendering_state.viewport.y, rendering_state.viewport.width, rendering_state.viewport.height);
    gfx_rapi->set_scissor(rendering_state.scissor.x, rendering_state.scissor.y, rendering_state.scissor.width, rendering_state.scissor.height);
    if (rendering_state.shader_program != NULL) {
        gfx_rapi->load_shader(rendering_state.shader_program); // program, attribute pointers, uniforms
    }
    rdp.textures_changed[0] = true; // textures are selected again at the next draw
    rdp.textures_changed[1] = true;
}

void gfx_end_frame(void) {
    dbg_end_frame();
    gfx_rapi->end_frame();
    gfx_rapi->finish_render();
}
