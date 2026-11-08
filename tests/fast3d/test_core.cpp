// Offline test for the embedded Fast3D core (lib/fast3d): no GPU, no ROM, no game.
//
// Builds a fake RDRAM in the N64Recomp layout (32-bit words hold the N64 value natively, bytes at addr ^ 3, halfwords at
// addr ^ 2), encodes F3DEX2 display lists by hand and runs them through gfx_run_task() against a null rendering backend
// that records every draw and texture upload. Run it after touching lib/fast3d:
//     cmake -S lib/fast3d -B tmp/fast3d && cmake --build tmp/fast3d && ctest --test-dir tmp/fast3d
// (see docs/fast3d_port_plan.md)
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
    uint32_t id;
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
static std::map<uint32_t, ShaderProgram> g_shaders;
static uint32_t g_next_texture = 1;

static bool nb_z01() { return false; }
static void nb_unload(ShaderProgram *) {}
static void nb_load(ShaderProgram *) {}
static ShaderProgram *nb_create(uint32_t id) {
    g_shaders[id] = ShaderProgram{id};
    return &g_shaders[id];
}
static ShaderProgram *nb_lookup(uint32_t id) {
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

// ---- fake RDRAM and F3DEX2 encoders ----------------------------------------------------------------------------------

static uint8_t *rdram;
static uint32_t dl_cursor;

static void w32(uint32_t addr, uint32_t v) { *(uint32_t *)(rdram + addr) = v; }
static void b8(uint32_t addr, uint8_t v) { rdram[addr ^ 3] = v; }
static void h16(uint32_t addr, uint16_t v) {
    b8(addr, v >> 8);
    b8(addr + 1, v & 0xFF);
}
static void cmd(uint32_t w0, uint32_t w1) {
    w32(dl_cursor, w0);
    w32(dl_cursor + 4, w1);
    dl_cursor += 8;
}
static void dl_begin(uint32_t addr) { dl_cursor = addr; }

static void put_matrix_identity(uint32_t addr) {
    static const uint32_t ident[16] = {0x00010000, 0, 0x00000001, 0, 0, 0x00010000, 0, 0x00000001, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; i++) w32(addr + i * 4, ident[i]);
}
static void put_vtx(uint32_t addr, int16_t x, int16_t y, int16_t z, int16_t s, int16_t t, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    h16(addr + 0, (uint16_t)x);
    h16(addr + 2, (uint16_t)y);
    h16(addr + 4, (uint16_t)z);
    h16(addr + 6, 0);
    h16(addr + 8, (uint16_t)s);
    h16(addr + 10, (uint16_t)t);
    b8(addr + 12, r);
    b8(addr + 13, g);
    b8(addr + 14, b);
    b8(addr + 15, a);
}

static void g_mtx(uint32_t addr, uint32_t flags) { cmd(0xDA380000u | ((flags ^ 1u) & 0xFF), addr); }
static void g_vertex(uint32_t addr, uint32_t n, uint32_t v0) { cmd(0x01000000u | (n << 12) | ((v0 + n) << 1), addr); }
static void g_tri1(uint32_t a, uint32_t b, uint32_t c) { cmd(0x05000000u | (a * 2 << 16) | (b * 2 << 8) | (c * 2), 0); }
static void g_tri2(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
    cmd(0x06000000u | (a * 2 << 16) | (b * 2 << 8) | (c * 2), (d * 2 << 16) | (e * 2 << 8) | (f * 2));
}
static void g_quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    cmd(0x07000000u | (a * 2 << 16) | (b * 2 << 8) | (c * 2), (a * 2 << 16) | (c * 2 << 8) | (d * 2));
}
static void g_segment(uint32_t seg, uint32_t base) { cmd(0xDB000000u | (6u << 16) | (seg * 4), base); }
static void g_dl(uint32_t addr, bool branch) { cmd(0xDE000000u | (branch ? 0x10000u : 0), addr); }
static void g_enddl() { cmd(0xDF000000u, 0); }
static void g_geometry_mode(uint32_t clear, uint32_t set) { cmd(0xD9000000u | (~clear & 0x00FFFFFFu), set); }
static void g_combine_shade() { cmd(0xFC000000u, (4u << 15) | (4u << 9)); }
// TEXEL0 * SHADE for color and alpha.
static void g_combine_tex_shade() { cmd(0xFC000000u | (1u << 20) | (4u << 15) | (1u << 12) | (4u << 9), 0); }
static void g_settimg(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t addr) {
    cmd(0xFD000000u | (fmt << 21) | (siz << 19) | (width - 1), addr);
}
static void g_settile(uint32_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint32_t tile) {
    cmd(0xF5000000u | (fmt << 21) | (siz << 19) | (line << 9) | tmem, tile << 24);
}
static void g_loadblock(uint32_t tile, uint32_t lrs) { cmd(0xF3000000u, (tile << 24) | (lrs << 12)); }
static void g_settilesize(uint32_t tile, uint32_t w, uint32_t h) { cmd(0xF2000000u, (tile << 24) | (((w - 1) << 2) << 12) | ((h - 1) << 2)); }
static void g_texture(uint32_t s, uint32_t t) { cmd(0xD7000000u | (1u << 1), (s << 16) | t); }

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
            uint8_t e[4] = {(uint8_t)(r * 255 / 31), (uint8_t)(g * 255 / 31), (uint8_t)(b * 255 / 31), (uint8_t)(a ? 255 : 0)};
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
    rdram = (uint8_t *)std::calloc(1, 8 * 1024 * 1024);
    gfx_init_embedded(&g_api, rdram);
    gfx_set_dimensions(320, 240);

    test_vertices_and_matrices();
    test_segments_and_nested_dl();
    test_texture_decode_and_cache();
    test_malformed_lists();

    std::free(rdram);
    if (g_failures == 0) {
        std::printf("fast3d core: all checks passed\n");
        return 0;
    }
    std::printf("fast3d core: %d check(s) failed\n", g_failures);
    return 1;
}
