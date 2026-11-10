// Offline test for the embedded Fast3D core (lib/fast3d): no GPU, no ROM, no game.
//
// Builds a fake RDRAM in the N64Recomp layout (32-bit words hold the N64 value natively, bytes at addr ^ 3, halfwords at
// addr ^ 2), encodes F3DEX2 display lists by hand and runs them through gfx_run_task() against a null rendering backend
// that records every draw and texture upload. Run it after touching lib/fast3d:
//     cmake -S lib/fast3d -B tmp/fast3d && cmake --build tmp/fast3d && ctest --test-dir tmp/fast3d
// (see docs/fast3d_port_plan.md)
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

extern "C" {
#include "gfx_cc.h"
#include "gfx_pc.h"
#include "gfx_rendering_api.h"
}

// ---- null backend --------------------------------------------------------------------------------------------------

struct ShaderProgram {
    uint64_t id;
};

struct Draw {
    std::vector<float> vbo;
    size_t tris;
};
struct Upload {
    std::vector<uint8_t> rgba;
    int width, height;
};

static std::vector<Draw> g_draws;
static std::vector<Upload> g_uploads;
static std::map<uint64_t, ShaderProgram> g_shaders;
static uint32_t g_next_texture = 1;

static bool nb_z01() { return false; }
static void nb_unload(ShaderProgram *) {}
static void nb_load(ShaderProgram *) {}
static ShaderProgram *nb_create(uint64_t id) {
    g_shaders[id] = ShaderProgram{id};
    return &g_shaders[id];
}
static ShaderProgram *nb_lookup(uint64_t id) {
    auto it = g_shaders.find(id);
    return it == g_shaders.end() ? nullptr : &it->second;
}
static void nb_info(ShaderProgram *prg, uint8_t *num_inputs, bool used[2]) {
    CCFeatures f;
    gfx_cc_get_features(prg->id, &f);
    *num_inputs = (uint8_t)f.num_inputs;
    used[0] = f.used_textures[0];
    used[1] = f.used_textures[1];
}
static uint32_t nb_new_texture() { return g_next_texture++; }
static void nb_select(int, uint32_t) {}
static void nb_upload(const uint8_t *rgba, int w, int h) {
    g_uploads.push_back({std::vector<uint8_t>(rgba, rgba + (size_t)w * h * 4), w, h});
}
static void nb_sampler(int, bool, uint32_t, uint32_t) {}
static void nb_bool(bool) {}
static void nb_rect(int, int, int, int) {}
static void nb_draw(float *vbo, size_t len, size_t tris) { g_draws.push_back({std::vector<float>(vbo, vbo + len), tris}); }
static void nb_void() {}

static GfxRenderingAPI g_api = {
    nb_z01, nb_unload, nb_load, nb_create, nb_lookup, nb_info, nb_new_texture, nb_select, nb_upload, nb_sampler,
    nb_bool, nb_bool, nb_bool, nb_rect, nb_rect, nb_bool, nb_draw, nb_void, nb_void, nb_void, nb_void, nb_void,
};

#include "dl_encode.h"

// ---- checks ---------------------------------------------------------------------------------------------------------

static int g_failures;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            g_failures++;                                                                                             \
            std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond);                                               \
            std::printf(__VA_ARGS__);                                                                                 \
            std::printf("\n");                                                                                        \
        }                                                                                                             \
    } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
static void reset_capture() {
    g_draws.clear();
    g_uploads.clear();
}

// Common state: identity projection and modelview, flat shading, no culling.
static void common_setup(uint32_t mtx_addr) {
    g_mtx(mtx_addr, 0x04 | 0x02); // projection | load
    g_mtx(mtx_addr, 0x00 | 0x02); // modelview | load
    g_geometry_mode(0xFFFFFF, 0x4); // G_SHADE
    g_combine_shade();
}

static void test_vertices_and_matrices() {
    reset_capture();
    const uint32_t MTX = 0x1000, VTX = 0x1100, DL = 0x2000;
    put_matrix_identity(MTX);
    put_vtx(VTX + 0, -1, -1, 0, 0, 0, 255, 0, 0, 255);
    put_vtx(VTX + 16, 1, -1, 0, 0, 0, 0, 255, 0, 255);
    put_vtx(VTX + 32, 0, 1, 0, 0, 0, 0, 0, 255, 255);
    dl_begin(DL);
    common_setup(MTX);
    g_vertex(VTX, 3, 0);
    g_tri1(0, 1, 2);
    g_enddl();
    gfx_begin_frame();
    gfx_run_task(DL);
    gfx_end_frame();

    CHECK(g_draws.size() == 1, "draws=%zu", g_draws.size());
    if (g_draws.size() != 1) return;
    const Draw &d = g_draws[0];
    CHECK(d.tris == 1, "tris=%zu", d.tris);
    // Per vertex: x y z w, then one combiner input (shade rgb + alpha) = 8 floats.
    CHECK(d.vbo.size() == 24, "vbo floats=%zu", d.vbo.size());
    if (d.vbo.size() != 24) return;
    const float expect[3][8] = {{-1, -1, 0, 1, 1, 0, 0, 1}, {1, -1, 0, 1, 0, 1, 0, 1}, {0, 1, 0, 1, 0, 0, 1, 1}};
    for (int v = 0; v < 3; v++) {
        for (int k = 0; k < 8; k++) {
            CHECK(near(d.vbo[v * 8 + k], expect[v][k]), "vertex %d float %d = %f, expected %f", v, k, d.vbo[v * 8 + k], expect[v][k]);
        }
    }
}

static void test_segments_and_nested_dl() {
    reset_capture();
    const uint32_t MTX = 0x1000, SEG_BASE = 0x00200000, DL = 0x2000, SUB_DL = SEG_BASE + 0x400;
    put_matrix_identity(MTX);
    // Vertices live at segment 6 offset 0x100; the sub display list at segment 6 offset 0x400.
    put_vtx(SEG_BASE + 0x100, -1, -1, 0, 0, 0, 255, 255, 255, 255);
    put_vtx(SEG_BASE + 0x110, 1, -1, 0, 0, 0, 255, 255, 255, 255);
    put_vtx(SEG_BASE + 0x120, 0, 1, 0, 0, 0, 255, 255, 255, 255);
    put_vtx(SEG_BASE + 0x130, 1, 1, 0, 0, 0, 255, 255, 255, 255);
    dl_begin(SUB_DL);
    g_vertex(0x06000100, 4, 0);
    g_tri2(0, 1, 2, 1, 3, 2);
    g_quad(0, 1, 3, 2);
    g_enddl();
    dl_begin(DL);
    common_setup(MTX);
    g_segment(6, SEG_BASE);
    g_dl(0x06000400, false);
    g_enddl();
    gfx_begin_frame();
    gfx_run_task(DL);
    gfx_end_frame();

    size_t tris = 0;
    for (const Draw &d : g_draws) tris += d.tris;
    CHECK(tris == 4, "TRI2 (2) + QUAD (2) via segment 6 gave %zu triangles", tris);
}

// The recompiled heap sits above 16 MB: a direct 0x81xxxxxx pointer must reach it, not wrap into segment 1.
// Rectangles draw through the four extra vertex slots after the 64 the RSP holds; a bounds check on triangle indices
// once rejected them, so no menu or HUD rectangle was ever drawn.
static void test_rectangles_draw() {
    reset_capture();
    const uint32_t MTX = 0x1000, DL = 0x2000;
    put_matrix_identity(MTX);
    dl_begin(DL);
    common_setup(MTX);
    g_combine_shade();
    cmd(0xFE000000u, 0x3000);                             // SETZIMG
    cmd(0xFF000000u, 0x4000);                             // SETCIMG (a fill into the Z image is a Z clear and skipped)
    cmd(0xF7000000u, 0xFFFFFFFFu);                        // SETFILLCOLOR
    cmd(0xF6000000u | (40 << 14) | (40 << 2), (20 << 14) | (20 << 2)); // FILLRECT (20,20)..(40,40)
    g_enddl();
    gfx_begin_frame();
    gfx_run_task(DL);
    gfx_end_frame();
    size_t tris = 0;
    for (const Draw &d : g_draws) tris += d.tris;
    CHECK(tris == 2, "a FILLRECT should draw 2 triangles, got %zu", tris);
}

static void test_direct_heap_pointers() {
    reset_capture();
    const uint32_t MTX = 0x1000, DL = 0x2000, HEAP = 0x01800000;
    put_matrix_identity(MTX);
    put_vtx(HEAP + 0, -1, -1, 0, 0, 0, 255, 0, 0, 255);
    put_vtx(HEAP + 16, 1, -1, 0, 0, 0, 0, 255, 0, 255);
    put_vtx(HEAP + 32, 0, 1, 0, 0, 0, 0, 0, 255, 255);
    dl_begin(DL);
    common_setup(MTX);
    g_vertex(0x80000000u | HEAP, 3, 0);
    g_tri1(0, 1, 2);
    g_enddl();
    gfx_begin_frame();
    gfx_run_task(DL);
    gfx_end_frame();
    CHECK(g_draws.size() == 1 && g_draws[0].vbo.size() == 24 && near(g_draws[0].vbo[0], -1) && near(g_draws[0].vbo[4], 1),
          "vertices behind a 0x81800000 pointer (draws=%zu)", g_draws.size());
}

// RT64 extended commands carry raw data words: 0xFC40... must not run as a G_SETCOMBINE, and a float matrix must apply.
static void test_extended_commands() {
    reset_capture();
    const uint32_t MTX = 0x1000, FMTX = 0x1100, DL = 0x2000, VTX = 0x3000;
    put_matrix_identity(MTX);
    float fm[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1}; // identity translated by +0.5 in x
    memcpy(rdram + FMTX, fm, sizeof(fm));
    put_vtx(VTX + 0, -1, -1, 0, 0, 0, 255, 0, 0, 255);
    put_vtx(VTX + 16, 1, -1, 0, 0, 0, 0, 255, 0, 255);
    put_vtx(VTX + 32, 0, 1, 0, 0, 0, 0, 0, 255, 255);
    dl_begin(DL);
    common_setup(MTX);
    cmd(0xE0525464u, 0x10000064u);          // gEXEnable
    cmd(0x64000006u, 0x00000000u);          // gEXSetRectAlign: 2 words, the second one's data looks like opcodes
    cmd(0xFC40FB00u, 0x03C0FFFFu);
    cmd(0x64000030u, 0x00000003u);   // gEXMatrixFloat: modelview, load (0x02) with the push bit stored inverted
    cmd(0u, 0x80000000u | FMTX);
    g_vertex(VTX, 3, 0);
    g_tri1(0, 1, 2);
    g_enddl();
    gfx_begin_frame();
    gfx_run_task(DL);
    gfx_end_frame();
    CHECK(g_draws.size() == 1 && g_draws[0].vbo.size() == 24, "a draw follows extended commands (draws=%zu)", g_draws.size());
    // x of the first vertex: -1 + 0.5 (the float matrix replaced the identity) in clip space, w = 1.
    CHECK(g_draws.size() == 1 && near(g_draws[0].vbo[0], -0.5f), "float matrix applied, got %f", g_draws.empty() ? 0.0f : g_draws[0].vbo[0]);
}

static uint16_t col16(int i) { return (uint16_t)((i << 11) | ((31 - i) << 6) | (((i * 2) % 32) << 1) | (i & 1)); }

static void test_texture_decode_and_cache() {
    reset_capture();
    const uint32_t MTX = 0x1000, VTX = 0x1100, TEX = 0x3002 /* deliberately not 4-byte aligned */, DL = 0x2000;
    put_matrix_identity(MTX);
    put_vtx(VTX + 0, -1, -1, 0, 0, 0, 255, 255, 255, 255);
    put_vtx(VTX + 16, 1, -1, 0, 128, 0, 255, 255, 255, 255);
    put_vtx(VTX + 32, 0, 1, 0, 0, 128, 255, 255, 255, 255);
    for (int i = 0; i < 16; i++) h16(TEX + i * 2, col16(i));
    // 4x4 RGBA16 texture: SETTIMG, SETTILE (load tile), LOADBLOCK, SETTILE (render tile), SETTILESIZE.
    dl_begin(DL);
    common_setup(MTX);
    g_combine_tex_shade();
    g_settimg(0, 2, 4, TEX);
    g_settile(0, 2, 0, 0, 7);
    g_loadblock(7, 15);
    g_settile(0, 2, 1, 0, 0);
    g_settilesize(0, 4, 4);
    g_texture(0xFFFF, 0xFFFF);
    g_vertex(VTX, 3, 0);
    g_tri1(0, 1, 2);
    g_enddl();

    gfx_begin_frame();
    gfx_run_task(DL);
    gfx_end_frame();
    CHECK(g_uploads.size() == 1, "uploads=%zu", g_uploads.size());
    if (g_uploads.size() == 1) {
        const Upload &u = g_uploads[0];
        CHECK(u.width == 4 && u.height == 4, "size %dx%d", u.width, u.height);
        bool ok = u.rgba.size() == 64;
        for (int i = 0; ok && i < 16; i++) {
            int r = i, g = 31 - i, b = (i * 2) % 32, a = i & 1;
            // a transparent texel keeps no color (bilinear fringe)
            uint8_t e[4] = {(uint8_t)(a ? r * 255 / 31 : 0), (uint8_t)(a ? g * 255 / 31 : 0), (uint8_t)(a ? b * 255 / 31 : 0), (uint8_t)(a ? 255 : 0)};
            if (std::memcmp(&u.rgba[i * 4], e, 4) != 0) {
                std::printf("FAIL texel %d: got %u,%u,%u,%u expected %u,%u,%u,%u\n", i, u.rgba[i * 4], u.rgba[i * 4 + 1], u.rgba[i * 4 + 2],
                            u.rgba[i * 4 + 3], e[0], e[1], e[2], e[3]);
                g_failures++;
                ok = false;
            }
        }
    }
    CHECK(!g_draws.empty() && g_draws[0].vbo.size() == 30, "textured vertex floats=%zu (x y z w u v + 4 shade)",
          g_draws.empty() ? 0 : g_draws[0].vbo.size());

    // Same texture again: served from the cache, no second upload.
    gfx_begin_frame();
    gfx_run_task(DL);
    gfx_end_frame();
    CHECK(g_uploads.size() == 1, "identical texture re-uploaded (%zu uploads)", g_uploads.size());

    // The game rewrites the texture in place at the same address: the cache must notice.
    for (int i = 0; i < 16; i++) h16(TEX + i * 2, 0xFFFF);
    gfx_begin_frame();
    gfx_run_task(DL);
    gfx_end_frame();
    CHECK(g_uploads.size() == 2, "changed texture bytes were not re-uploaded (%zu uploads)", g_uploads.size());
}

// ---- the TMEM model ---------------------------------------------------------------------------------------------------

// Draws one textured triangle after `setup` has emitted the loads and tile state, and returns the texture uploads.
template <typename F>
static std::vector<Upload> draw_with_texture(F setup, uint32_t level = 0) {
    reset_capture();
    const uint32_t MTX = 0x1000, VTX = 0x1100, DL = 0x2000;
    put_matrix_identity(MTX);
    put_vtx(VTX + 0, -1, -1, 0, 0, 0, 255, 255, 255, 255);
    put_vtx(VTX + 16, 1, -1, 0, 128, 0, 255, 255, 255, 255);
    put_vtx(VTX + 32, 0, 1, 0, 0, 128, 255, 255, 255, 255);
    dl_begin(DL);
    common_setup(MTX);
    g_combine_tex_shade();
    setup();
    g_texture_tile(0xFFFF, 0xFFFF, level);
    g_vertex(VTX, 3, 0);
    g_tri1(0, 1, 2);
    g_enddl();
    gfx_begin_frame();
    gfx_run_task(DL);
    gfx_end_frame();
    return g_uploads;
}

static void put_bytes(uint32_t addr, const std::vector<uint8_t> &bytes) {
    for (size_t i = 0; i < bytes.size(); i++) b8(addr + (uint32_t)i, bytes[i]);
}

static bool upload_is(const std::vector<Upload> &ups, size_t index, int w, int h, const std::vector<uint8_t> &rgba, const char *what) {
    if (ups.size() <= index) {
        std::printf("FAIL %s: only %zu uploads\n", what, ups.size());
        g_failures++;
        return false;
    }
    const Upload &u = ups[index];
    if (u.width != w || u.height != h) {
        std::printf("FAIL %s: size %dx%d, expected %dx%d\n", what, u.width, u.height, w, h);
        g_failures++;
        return false;
    }
    if (u.rgba != rgba) {
        std::printf("FAIL %s: texels differ, got", what);
        for (size_t i = 0; i < u.rgba.size() && i < 32; i++) std::printf(" %u", u.rgba[i]);
        std::printf("\n");
        g_failures++;
        return false;
    }
    return true;
}

// Expected RGBA for a list of intensities (I8) or (intensity, alpha) pairs.
static std::vector<uint8_t> expand_i(const std::vector<uint8_t> &v) {
    std::vector<uint8_t> out;
    for (uint8_t i : v) out.insert(out.end(), {i, i, i, i});
    return out;
}

// Note on sizes: a row is padded to a multiple of 8 bytes in TMEM (the tile line is in 64-bit words), so these textures
// use 8-byte rows, which is what a real texture loaded as a block has.
static void test_tmem_formats() {
    const uint32_t TEX = 0x3000;
    // I8 8x2, loaded as a block of 16-bit units (how the libultra macros load 8-bit data).
    std::vector<uint8_t> i8;
    for (int i = 0; i < 16; i++) i8.push_back((uint8_t)(i * 17));
    put_bytes(TEX, i8);
    auto ups = draw_with_texture([&] {
        g_settimg(4, 2, 8, TEX);
        g_settile(4, 2, 0, 0, 7);
        g_loadblock(7, 7);
        g_settile(4, 1, 1, 0, 0);
        g_settilesize(0, 8, 2);
    });
    upload_is(ups, 0, 8, 2, expand_i(i8), "I8 8x2");

    // IA8: intensity high nibble, alpha low nibble, each x17.
    put_bytes(TEX, {0xF0, 0x0F, 0x84, 0xFF, 0x12, 0x34, 0x56, 0x78, 0, 0, 0, 0, 0, 0, 0, 0});
    ups = draw_with_texture([&] {
        g_settimg(3, 2, 8, TEX);
        g_settile(3, 2, 0, 0, 7);
        g_loadblock(7, 7);
        g_settile(3, 1, 1, 0, 0);
        g_settilesize(0, 8, 2);
    });
    CHECK(ups.size() == 1 && ups[0].width == 8 && ups[0].height == 2, "IA8 size");
    if (ups.size() == 1 && ups[0].rgba.size() == 64) {
        const std::vector<uint8_t> first = {255, 255, 255, 0, 0, 0, 0, 255, 136, 136, 136, 68, 255, 255, 255, 255,
                                            17, 17, 17, 34, 51, 51, 51, 68, 85, 85, 85, 102, 119, 119, 119, 136};
        CHECK(std::equal(first.begin(), first.end(), ups[0].rgba.begin()), "IA8 first row");
        CHECK(ups[0].rgba[32 + 3] == 0, "IA8 second row is the zero bytes");
    }

    // IA16 4x2: intensity byte then alpha byte, 8-byte rows.
    put_bytes(TEX, {10, 200, 20, 100, 30, 50, 40, 0, 1, 2, 3, 4, 5, 6, 7, 8});
    ups = draw_with_texture([&] {
        g_settimg(3, 2, 4, TEX);
        g_settile(3, 2, 0, 0, 7);
        g_loadblock(7, 7);
        g_settile(3, 2, 1, 0, 0);
        g_settilesize(0, 4, 2);
    });
    upload_is(ups, 0, 4, 2, {10, 10, 10, 200, 20, 20, 20, 100, 30, 30, 30, 50, 40, 40, 40, 0, 1, 1, 1, 2, 3, 3, 3, 4, 5, 5, 5, 6, 7, 7, 7, 8},
              "IA16 4x2");

    // I4 16x1 and IA4 16x1: two texels per byte, high nibble first.
    put_bytes(TEX, {0x0F, 0x84, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60});
    ups = draw_with_texture([&] {
        g_settimg(4, 2, 16, TEX);
        g_settile(4, 2, 0, 0, 7);
        g_loadblock(7, 3);
        g_settile(4, 0, 1, 0, 0);
        g_settilesize(0, 16, 1);
    });
    CHECK(ups.size() == 1 && ups[0].width == 16 && ups[0].height == 1, "I4 size");
    if (ups.size() == 1 && ups[0].rgba.size() == 64) {
        const std::vector<uint8_t> first = {0, 0, 0, 0, 255, 255, 255, 255, 136, 136, 136, 136, 68, 68, 68, 68};
        CHECK(std::equal(first.begin(), first.end(), ups[0].rgba.begin()), "I4 first texels");
    }
    put_bytes(TEX, {0xF1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}); // 15 = intensity 7 alpha 1, then 1 = intensity 0 alpha 1
    ups = draw_with_texture([&] {
        g_settimg(3, 2, 16, TEX);
        g_settile(3, 2, 0, 0, 7);
        g_loadblock(7, 3);
        g_settile(3, 0, 1, 0, 0);
        g_settilesize(0, 16, 1);
    });
    CHECK(ups.size() == 1 && ups[0].width == 16, "IA4 size");
    if (ups.size() == 1 && ups[0].rgba.size() == 64) {
        const std::vector<uint8_t> first = {252, 252, 252, 255, 0, 0, 0, 255, 0, 0, 0, 0};
        CHECK(std::equal(first.begin(), first.end(), ups[0].rgba.begin()), "IA4 first texels");
    }

    // RGBA32 4x2: R G B A bytes per texel, 16 bytes per row.
    std::vector<uint8_t> rgba32;
    for (int i = 0; i < 32; i++) rgba32.push_back((uint8_t)(i * 7 + 3));
    put_bytes(TEX, rgba32);
    ups = draw_with_texture([&] {
        g_settimg(0, 3, 4, TEX);
        g_settile(0, 3, 0, 0, 7);
        g_loadblock(7, 7, 0x400); // two 64-bit words per row
        g_settile(0, 3, 1, 0, 0);
        g_settilesize(0, 4, 2);
    });
    upload_is(ups, 0, 4, 2, rgba32, "RGBA32 4x2");
}

static void test_tmem_palettes() {
    const uint32_t TEX = 0x3000, PAL = 0x3400;
    // Palette: entry i = 5-bit red i, alpha 1.
    for (int i = 0; i < 32; i++) h16(PAL + i * 2, (uint16_t)((i << 11) | 1));
    auto red = [](int r) { return (uint8_t)(r * 255 / 31); };
    // CI8 8x2: indices into the palette at TMEM word 256.
    std::vector<uint8_t> idx = {0, 1, 2, 3, 31, 30, 29, 28, 4, 5, 6, 7, 8, 9, 10, 11};
    put_bytes(TEX, idx);
    auto ups = draw_with_texture([&] {
        g_settimg(0, 2, 32, PAL);
        g_settile(0, 2, 0, 256, 7);
        g_loadtlut(7, 32);
        g_settimg(2, 2, 8, TEX);
        g_settile(2, 2, 0, 0, 7);
        g_loadblock(7, 7);
        g_settile(2, 1, 1, 0, 0);
        g_settilesize(0, 8, 2);
    });
    std::vector<uint8_t> expect;
    for (uint8_t i : idx) expect.insert(expect.end(), {red(i), 0, 0, 255});
    upload_is(ups, 0, 8, 2, expect, "CI8 8x2");

    // CI4 16x1 using palette 1 (palette slots 16..31): the nibble indexes within that palette.
    put_bytes(TEX, {0x01, 0x23, 0xF0, 0x12, 0x00, 0x00, 0x00, 0x00});
    ups = draw_with_texture([&] {
        g_settimg(0, 2, 32, PAL);
        g_settile(0, 2, 0, 256, 7);
        g_loadtlut(7, 32);
        g_settimg(2, 2, 16, TEX);
        g_settile(2, 2, 0, 0, 7);
        g_loadblock(7, 3);
        g_settile_full(2, 0, 1, 0, 0, 1, 0, 0, 0, 0);
        g_settilesize(0, 16, 1);
    });
    expect.clear();
    for (int nib : {0, 1, 2, 3, 15, 0, 1, 2}) expect.insert(expect.end(), {red(16 + nib), 0, 0, 255});
    CHECK(ups.size() == 1 && ups[0].width == 16 && ups[0].rgba.size() >= 32 && std::equal(expect.begin(), expect.end(), ups[0].rgba.begin()),
          "CI4 palette 1 (size %d)", ups.empty() ? 0 : ups[0].width);

    // A second palette load does not disturb the first one's textures (cache key includes the palette).
    for (int i = 0; i < 32; i++) h16(PAL + i * 2, (uint16_t)(((31 - i) << 11) | 1));
    put_bytes(TEX, idx);
    ups = draw_with_texture([&] {
        g_settimg(0, 2, 32, PAL);
        g_settile(0, 2, 0, 256, 7);
        g_loadtlut(7, 32);
        g_settimg(2, 2, 8, TEX);
        g_settile(2, 2, 0, 0, 7);
        g_loadblock(7, 7);
        g_settile(2, 1, 1, 0, 0);
        g_settilesize(0, 8, 2);
    });
    CHECK(ups.size() == 1 && ups[0].rgba[0] == red(31), "a changed palette re-uploads the CI texture (got %u)", ups.empty() ? 0 : ups[0].rgba[0]);

    // Full-size palettes as the gDPLoadTLUT_pal16 / _pal256 macros load them: every entry must arrive, not a quarter
    // of them (regression: the count field was shifted down by 2 twice, leaving CI textures black and speckled).
    const uint32_t PAL256 = 0x4000;
    for (int i = 0; i < 256; i++) h16(PAL256 + i * 2, (uint16_t)(((i % 31 + 1) << 11) | 1));
    std::vector<uint8_t> hi = {255, 200, 100, 17, 64, 65, 66, 67};
    put_bytes(TEX, hi);
    ups = draw_with_texture([&] {
        g_settimg(0, 2, 256, PAL256);
        g_settile(0, 2, 0, 256, 7);
        g_loadtlut(7, 256);
        g_settimg(2, 2, 8, TEX);
        g_settile(2, 2, 0, 0, 7);
        g_loadblock(7, 7);
        g_settile(2, 1, 1, 0, 0);
        g_settilesize(0, 8, 1);
    });
    expect.clear();
    for (uint8_t i : hi) expect.insert(expect.end(), {red(i % 31 + 1), 0, 0, 255});
    upload_is(ups, 0, 8, 1, expect, "CI8 with a 256-entry palette");

    // TLUT type IA16 (G_SETOTHERMODE_H bits 14-15 = 3): entries are intensity (high byte) + alpha (low byte).
    for (int i = 0; i < 256; i++) h16(PAL256 + i * 2, (uint16_t)((i << 8) | (255 - i)));
    put_bytes(TEX, {1, 2, 3, 4, 5, 6, 7, 8});
    ups = draw_with_texture([&] {
        g_othermode_h(14, 2, 3u << 14);
        g_settimg(0, 2, 256, PAL256);
        g_settile(0, 2, 0, 256, 7);
        g_loadtlut(7, 256);
        g_settimg(2, 2, 8, TEX);
        g_settile(2, 2, 0, 0, 7);
        g_loadblock(7, 7);
        g_settile(2, 1, 1, 0, 0);
        g_settilesize(0, 8, 1);
    });
    expect.clear();
    for (int i = 1; i <= 8; i++) expect.insert(expect.end(), {(uint8_t)i, (uint8_t)i, (uint8_t)i, (uint8_t)(255 - i)});
    upload_is(ups, 0, 8, 1, expect, "CI8 with an IA16 palette");

    // pal16: palette 0 of a CI4 texture, entries 4..15 included.
    for (int i = 0; i < 16; i++) h16(PAL256 + i * 2, (uint16_t)(((i + 8) << 11) | 1));
    put_bytes(TEX, {0x4B, 0xFE, 0x57, 0x9A, 0x00, 0x00, 0x00, 0x00});
    ups = draw_with_texture([&] {
        g_othermode_h(14, 2, 2u << 14); // back to RGBA16 palettes
        g_settimg(0, 2, 16, PAL256);
        g_settile(0, 2, 0, 256, 7);
        g_loadtlut(7, 16);
        g_settimg(2, 2, 16, TEX);
        g_settile(2, 2, 0, 0, 7);
        g_loadblock(7, 3);
        g_settile_full(2, 0, 1, 0, 0, 0, 0, 0, 0, 0);
        g_settilesize(0, 16, 1);
    });
    expect.clear();
    for (int nib : {4, 0xB, 0xF, 0xE, 5, 7, 9, 0xA}) expect.insert(expect.end(), {red(nib + 8), 0, 0, 255});
    CHECK(ups.size() == 1 && ups[0].width == 16 && ups[0].rgba.size() >= 32 && std::equal(expect.begin(), expect.end(), ups[0].rgba.begin()),
          "CI4 with a 16-entry palette (size %d)", ups.empty() ? 0 : ups[0].width);
}

static void test_tmem_tiles_and_sizes() {
    const uint32_t TEX = 0x3000;
    // Two textures resident at once at different TMEM addresses; G_TEXTURE's tile picks which one a draw samples.
    std::vector<uint8_t> a, b;
    for (int i = 0; i < 16; i++) {
        a.push_back((uint8_t)(i + 1));
        b.push_back((uint8_t)(101 + i));
    }
    put_bytes(TEX, a);
    put_bytes(TEX + 0x100, b);
    auto setup_two = [&] {
        g_settimg(4, 2, 8, TEX);
        g_settile(4, 2, 0, 0, 7);
        g_loadblock(7, 7);
        g_settimg(4, 2, 8, TEX + 0x100);
        g_settile(4, 2, 0, 64, 7);
        g_loadblock(7, 7);
        g_settile(4, 1, 1, 0, 0);
        g_settilesize(0, 8, 2);
        g_settile(4, 1, 1, 64, 1);
        g_settilesize(1, 8, 2);
    };
    auto ups = draw_with_texture(setup_two, 0);
    upload_is(ups, 0, 8, 2, expand_i(a), "tile 0 (first texture)");
    ups = draw_with_texture(setup_two, 1);
    upload_is(ups, 0, 8, 2, expand_i(b), "tile 1 (second texture, still resident)");

    // G_LOADTILE of a sub-rectangle: an 8x4 I8 image, the 4x2 window at (2, 1).
    std::vector<uint8_t> image;
    for (int i = 0; i < 32; i++) image.push_back((uint8_t)(i + 1));
    put_bytes(TEX, image);
    ups = draw_with_texture([&] {
        g_settimg(4, 1, 8, TEX);
        g_settile(4, 1, 1, 0, 7);
        g_loadtile(7, 2, 1, 4, 2);
        g_settile(4, 1, 1, 0, 0);
        g_settilesize(0, 4, 2);
    });
    // Row 1 of the image is 9..16, row 2 is 17..24; the window starts at column 2.
    upload_is(ups, 0, 4, 2, expand_i({11, 12, 13, 14, 19, 20, 21, 22}), "LOADTILE sub-rectangle");

    // The size comes from SETTILESIZE, not from the line: a 16-texel line with a 6x2 window.
    ups = draw_with_texture([&] {
        g_settimg(4, 1, 16, TEX);
        g_settile(4, 1, 2, 0, 7);
        g_loadtile(7, 0, 0, 16, 2);
        g_settile(4, 1, 2, 0, 0);
        g_settilesize(0, 6, 2);
    });
    CHECK(ups.size() == 1 && ups[0].width == 6 && ups[0].height == 2, "window 6x2 got %dx%d", ups.empty() ? 0 : ups[0].width,
          ups.empty() ? 0 : ups[0].height);

    // A mask makes the repeat period the texture size even when the tile window is wider.
    ups = draw_with_texture([&] {
        g_settimg(4, 1, 16, TEX);
        g_settile(4, 1, 2, 0, 7);
        g_loadtile(7, 0, 0, 16, 2);
        g_settile_full(4, 1, 2, 0, 0, 0, 0, 1, 0, 3); // wrap 8 texels across, 2 down
        g_settilesize(0, 16, 2);
    });
    CHECK(ups.size() == 1 && ups[0].width == 8 && ups[0].height == 2, "masked 8x2 got %dx%d", ups.empty() ? 0 : ups[0].width,
          ups.empty() ? 0 : ups[0].height);


    // A size window that starts at row 2 (the radar map's strips do: ult = 54) addresses the rows that were loaded, from
    // TMEM row 0: the RDP subtracts the window origin from the coordinates.
    put_bytes(TEX + 0x200, {41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56});
    ups = draw_with_texture([&] {
        g_settimg(4, 1, 8, TEX + 0x200);
        g_settile(4, 1, 1, 0, 7);
        g_loadblock(7, 15);                       // 16 bytes: two rows of 8
        g_settile(4, 1, 1, 0, 0);
        g_settilesize_at(0, 0, 2, 8, 2);          // rows 2..3 of the image
    });
    upload_is(ups, 0, 8, 2, expand_i({41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56}), "window at ult=2 reads from TMEM row 0");

    // A tile that was never set up is a placeholder, not a crash.
    ups = draw_with_texture([&] { g_settile(4, 1, 0, 0, 0); });
    CHECK(ups.size() == 1 && ups[0].width == 2 && ups[0].height == 2, "unset tile placeholder");
}

static void test_malformed_lists() {
    reset_capture();
    const uint32_t MTX = 0x1000, DL = 0x2000, LOOP = 0x4000, RECURSE = 0x5000;
    put_matrix_identity(MTX);
    // A branch to itself must end through the command budget, not hang.
    dl_begin(LOOP);
    g_dl(LOOP, true);
    dl_begin(RECURSE);
    g_dl(RECURSE, false); // pushes itself until the RSP stack depth limit
    g_enddl();
    dl_begin(DL);
    common_setup(MTX);
    // 100 vertices from slot 0 (v0 - n underflows) and triangles naming slots past the vertex array.
    cmd(0x01000000u | (100u << 12), 0x1100);
    g_tri1(100, 101, 102);
    g_dl(RECURSE, false);
    g_dl(LOOP, true);
    g_enddl();
    gfx_begin_frame();
    gfx_run_task(DL);
    gfx_end_frame();
    CHECK(true, "survived");
}

int main() {
    rdram = (uint8_t *)std::calloc(1, 32 * 1024 * 1024);
    gfx_init_embedded(&g_api, rdram);
    gfx_set_dimensions(320, 240);

    test_vertices_and_matrices();
    test_segments_and_nested_dl();
    test_direct_heap_pointers();
    test_rectangles_draw();
    test_extended_commands();
    test_texture_decode_and_cache();
    test_tmem_formats();
    test_tmem_palettes();
    test_tmem_tiles_and_sizes();
    test_malformed_lists();

    std::free(rdram);
    if (g_failures == 0) {
        std::printf("fast3d core: all checks passed\n");
        return 0;
    }
    std::printf("fast3d core: %d check(s) failed\n", g_failures);
    return 1;
}
