// Offline test for the OpenGL backend (lib/fast3d/gfx_opengl.c) against a fake GL that records calls: no GPU needed.
// It checks the function loader, the vertex attribute layout against what the core writes, shader failure handling, the
// shader table, texture and sampler state, and runs a display list through the real core and the real backend together.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "gfx_cc.h"
#include "gfx_opengl.h"
#include "gfx_pc.h"
#include "gfx_rendering_api.h"
}
#include "dl_encode.h"

// ---- fake GL ----------------------------------------------------------------------------------------------------------

struct AttribPtr {
    unsigned index;
    int size;
    int stride;
    size_t offset;
};

static std::vector<std::string> g_calls; // "name(args)" log
static std::vector<AttribPtr> g_attrib_ptrs;
static std::vector<unsigned> g_enabled, g_disabled;
static unsigned g_next_id = 1;
static bool g_fail_compile;      // make glCompileShader report failure
static int g_compile_fail_after; // fail only after this many successful compiles (-1: unused)
static std::map<unsigned, bool> g_shader_ok;
static unsigned g_used_program;
static std::vector<int> g_draw_counts;
static size_t g_buffer_bytes;
static std::vector<int> g_tex_wrap_s, g_tex_wrap_t, g_tex_filter;
static int g_teximage_w, g_teximage_h, g_teximage_count;
static unsigned g_active_texture;
static int g_uniform_sets;
static std::string g_last_vs, g_last_fs;
static int g_compiles;

static void log_call(const char *fmt, ...) {
    char buf[200];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    g_calls.push_back(buf);
}

extern "C" {
static void f_genBuffers(int n, unsigned *b) { for (int i = 0; i < n; i++) b[i] = g_next_id++; }
static void f_bindBuffer(unsigned, unsigned) {}
static void f_bufferData(unsigned, ptrdiff_t size, const void *, unsigned) { g_buffer_bytes = (size_t)size; }
static void f_genVAO(int n, unsigned *b) { for (int i = 0; i < n; i++) b[i] = g_next_id++; log_call("glGenVertexArrays"); }
static void f_bindVAO(unsigned) { log_call("glBindVertexArray"); }
static std::vector<std::pair<unsigned, std::string>> g_bound_attribs;
static void f_bindAttrib(unsigned, unsigned index, const char *name) { g_bound_attribs.push_back({index, name}); }
static void f_enableAttrib(unsigned i) { g_enabled.push_back(i); }
static void f_disableAttrib(unsigned i) { g_disabled.push_back(i); }
static void f_attribPointer(unsigned i, int size, unsigned, unsigned char, int stride, const void *p) {
    g_attrib_ptrs.push_back({i, size, stride, (size_t)p});
}
static unsigned f_createShader(unsigned) { return g_next_id++; }
static void f_shaderSource(unsigned sh, int, const char *const *s, const int *) {
    (strstr(s[0], "gl_Position") ? g_last_vs : g_last_fs) = s[0];
    g_shader_ok[sh] = true;
}
static void f_compileShader(unsigned sh) {
    g_compiles++;
    if (g_fail_compile || (g_compile_fail_after >= 0 && g_compiles > g_compile_fail_after)) g_shader_ok[sh] = false;
}
static void f_getShaderiv(unsigned sh, unsigned pname, int *out) { *out = (pname == 0x8B81) ? (g_shader_ok[sh] ? 1 : 0) : 0; }
static void f_getShaderInfoLog(unsigned, int, int *len, char *log) { std::strcpy(log, "fake compile error"); if (len) *len = 18; }
static void f_deleteShader(unsigned) {}
static unsigned f_createProgram() { return g_next_id++; }
static void f_attachShader(unsigned, unsigned) {}
static void f_linkProgram(unsigned) {}
static void f_getProgramiv(unsigned, unsigned, int *out) { *out = 1; }
static void f_getProgramInfoLog(unsigned, int, int *, char *) {}
static void f_deleteProgram(unsigned) {}
static void f_useProgram(unsigned p) { g_used_program = p; }
static int f_getUniformLocation(unsigned, const char *) { return 7; }
static void f_uniform1i(int, int) { g_uniform_sets++; }
static void f_genTextures(int n, unsigned *t) { for (int i = 0; i < n; i++) t[i] = g_next_id++; }
static void f_activeTexture(unsigned t) { g_active_texture = t; }
static void f_bindTexture(unsigned, unsigned) {}
static void f_texImage2D(unsigned, int, int, int w, int h, int, unsigned, unsigned, const void *) { g_teximage_w = w; g_teximage_h = h; g_teximage_count++; }
static void f_texParameteri(unsigned, unsigned pname, int v) {
    if (pname == 0x2802) g_tex_wrap_s.push_back(v);
    if (pname == 0x2803) g_tex_wrap_t.push_back(v);
    if (pname == 0x2801) g_tex_filter.push_back(v);
}
static void f_noop1u(unsigned) {}
static void f_pixelStorei(unsigned, int) {}
static void f_depthMask(unsigned char) {}
static void f_depthFunc(unsigned) {}
static void f_blendFunc(unsigned, unsigned) {}
static void f_polygonOffset(float, float) {}
static void f_viewport(int, int, int, int) {}
static void f_scissor(int, int, int, int) {}
static void f_drawArrays(unsigned, int, int count) { g_draw_counts.push_back(count); }
static void f_clearColor(float, float, float, float) {}
static void f_clear(unsigned) {}
}

static std::map<std::string, void *> g_gl = {
    {"glGenBuffers", (void *)f_genBuffers}, {"glBindBuffer", (void *)f_bindBuffer}, {"glBufferData", (void *)f_bufferData},
    {"glGenVertexArrays", (void *)f_genVAO}, {"glBindVertexArray", (void *)f_bindVAO}, {"glBindAttribLocation", (void *)f_bindAttrib},
    {"glEnableVertexAttribArray", (void *)f_enableAttrib}, {"glDisableVertexAttribArray", (void *)f_disableAttrib},
    {"glVertexAttribPointer", (void *)f_attribPointer}, {"glCreateShader", (void *)f_createShader},
    {"glShaderSource", (void *)f_shaderSource}, {"glCompileShader", (void *)f_compileShader},
    {"glGetShaderiv", (void *)f_getShaderiv}, {"glGetShaderInfoLog", (void *)f_getShaderInfoLog},
    {"glDeleteShader", (void *)f_deleteShader}, {"glCreateProgram", (void *)f_createProgram},
    {"glAttachShader", (void *)f_attachShader}, {"glLinkProgram", (void *)f_linkProgram},
    {"glGetProgramiv", (void *)f_getProgramiv}, {"glGetProgramInfoLog", (void *)f_getProgramInfoLog},
    {"glDeleteProgram", (void *)f_deleteProgram}, {"glUseProgram", (void *)f_useProgram},
    {"glGetUniformLocation", (void *)f_getUniformLocation}, {"glUniform1i", (void *)f_uniform1i},
    {"glGenTextures", (void *)f_genTextures}, {"glActiveTexture", (void *)f_activeTexture},
    {"glBindTexture", (void *)f_bindTexture}, {"glTexImage2D", (void *)f_texImage2D},
    {"glTexParameteri", (void *)f_texParameteri}, {"glPixelStorei", (void *)f_pixelStorei},
    {"glEnable", (void *)f_noop1u}, {"glDisable", (void *)f_noop1u}, {"glDepthFunc", (void *)f_depthFunc},
    {"glDepthMask", (void *)f_depthMask}, {"glBlendFunc", (void *)f_blendFunc}, {"glPolygonOffset", (void *)f_polygonOffset},
    {"glViewport", (void *)f_viewport}, {"glScissor", (void *)f_scissor}, {"glDrawArrays", (void *)f_drawArrays},
    {"glClearColor", (void *)f_clearColor}, {"glClear", (void *)f_clear},
};

static void *get_proc(const char *name) {
    auto it = g_gl.find(name);
    return it == g_gl.end() ? nullptr : it->second;
}

static void reset_gl() {
    g_calls.clear();
    g_attrib_ptrs.clear();
    g_enabled.clear();
    g_disabled.clear();
    g_draw_counts.clear();
    g_tex_wrap_s.clear();
    g_tex_wrap_t.clear();
    g_tex_filter.clear();
    g_fail_compile = false;
    g_compile_fail_after = -1;
    g_compiles = 0;
    g_teximage_count = 0;
    g_buffer_bytes = 0;
}

// ---- checks -----------------------------------------------------------------------------------------------------------

static int g_failures;
#define CHECK(cond, ...)                                                                              \
    do {                                                                                              \
        if (!(cond)) {                                                                                \
            g_failures++;                                                                             \
            std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond);                               \
            std::printf(__VA_ARGS__);                                                                 \
            std::printf("\n");                                                                        \
        }                                                                                             \
    } while (0)

// Raw RDP numbering (see gfx_cc.h): d = SHADE (4) for color and alpha; texel0 * shade is a = TEXEL0 (1), c = SHADE (4).
static const uint64_t ID_SHADE = gfx_cc_pack_cycle(15, 15, 31, 4, 7, 7, 7, 4) | SHADER_OPT_ALPHA;
static const uint64_t ID_TEX_SHADE_FOG =
    gfx_cc_pack_cycle(1, 15, 4, 7, 1, 7, 4, 7) | SHADER_OPT_ALPHA | SHADER_OPT_FOG; // texel0 * shade, with alpha and fog

static bool has(const char *hay, const char *needle) { return std::strstr(hay, needle) != nullptr; }

// Inputs the old 3-bit combiner dropped to 0: SHADE_ALPHA / PRIM_ALPHA / ENV_ALPHA as c, constant 1, COMBINED, cycle 2.
static void test_combiner_sources() {
    // cycle 0: TEXEL0 * SHADE_ALPHA (c = 11) in rgb, alpha = constant 1 (d = 6); cycle 1: COMBINED (d = 0) * PRIM_ALPHA.
    uint64_t id = gfx_cc_pack_cycle(1, 15, 11, 7, 7, 7, 7, 6) | (gfx_cc_pack_cycle(0, 15, 10, 7, 0, 7, 3, 7) << 28) |
                  SHADER_OPT_TWO_CYCLE | SHADER_OPT_ALPHA;
    CCFeatures f;
    gfx_cc_get_features(id, &f);
    CHECK(f.rgb[0][2] == CCS_SHADEA && f.alpha[0][3] == CCS_ONE, "cycle 0: c = SHADE alpha, alpha d = 1");
    CHECK(f.rgb[1][0] == CCS_COMB && f.rgb[1][2] == CCS_PRIMA, "cycle 1: COMBINED * PRIM alpha");
    CHECK(f.used_textures[0] && !f.used_textures[1], "texel0 in cycle 0 only");
    CHECK(f.num_inputs == 2 && f.input_kind[0] == CCIN_SHADE && f.input_kind[1] == CCIN_PRIM, "shade and prim inputs");
    GfxGLSL g;
    CHECK(gfx_opengl_build_glsl(id, GFX_GLSL_120, &g), "build");
    CHECK(has(g.fs, "vec3(vShade.a)") && has(g.fs, "combined.rgb") && has(g.fs, "vec3(vPrim.a)"), "sources appear in the shader");
    // Cycle 1's TEXEL0 is the next tile's texel (TEXEL1 of cycle 0).
    id = gfx_cc_pack_cycle(15, 15, 31, 7, 7, 7, 7, 7) | (gfx_cc_pack_cycle(15, 15, 31, 1, 7, 7, 7, 1) << 28) | SHADER_OPT_TWO_CYCLE;
    gfx_cc_get_features(id, &f);
    CHECK(f.rgb[1][3] == CCS_T1 && f.used_textures[1] && !f.used_textures[0], "cycle 1 TEXEL0 reads tile + 1");
    // Single cycle ignores the second cycle's fields entirely.
    id = gfx_cc_pack_cycle(15, 15, 31, 4, 7, 7, 7, 4) | (gfx_cc_pack_cycle(1, 2, 3, 3, 1, 2, 3, 3) << 28);
    gfx_cc_get_features(id, &f);
    CHECK(!f.used_textures[0] && !f.used_textures[1] && f.num_inputs == 1, "cycle 1 unused in one-cycle mode");
}

static void test_loader() {
    CHECK(!gfx_opengl_load_functions(nullptr), "null loader must fail");
    // A loader that cannot find glDrawArrays must be rejected rather than crash later.
    auto saved = g_gl["glDrawArrays"];
    g_gl.erase("glDrawArrays");
    std::fprintf(stderr, "(expected message follows)\n");
    CHECK(!gfx_opengl_load_functions(get_proc), "missing function must fail");
    g_gl["glDrawArrays"] = saved;
    CHECK(gfx_opengl_load_functions(get_proc), "complete loader must succeed");
}

static void test_glsl_layout_matches_core_vertex_format() {
    // The core writes: x y z w, [u v], [fog rgb + factor], then per combiner input rgb(+a).
    GfxGLSL g;
    CHECK(gfx_opengl_build_glsl(ID_TEX_SHADE_FOG, GFX_GLSL_330_CORE, &g), "build");
    CHECK(g.used_textures[0] && !g.used_textures[1], "texture use");
    // inputs: c[0] = {5(texel0),4(input4),... } is decided by gfx_cc; derive the expectation from it.
    CCFeatures f;
    gfx_cc_get_features(ID_TEX_SHADE_FOG, &f);
    int expected_attribs = 1 + 1 + 1 + f.num_inputs;
    CHECK(g.num_attribs == expected_attribs, "attribs %d expected %d", g.num_attribs, expected_attribs);
    int sum = 0;
    for (int i = 0; i < g.num_attribs; i++) sum += g.attrib_sizes[i];
    CHECK(sum == g.num_floats, "floats %d vs sum %d", g.num_floats, sum);
    int expected_floats = 4 + 2 + 4 + f.num_inputs * 4;
    CHECK(g.num_floats == expected_floats, "floats per vertex %d expected %d", g.num_floats, expected_floats);
    CHECK(g.attrib_sizes[0] == 4 && g.attrib_sizes[1] == 2 && g.attrib_sizes[2] == 4, "pos/tex/fog sizes");
    CHECK(std::strstr(g.vs, "#version 330 core") && std::strstr(g.fs, "#version 330 core"), "330 core header");
    CHECK(!std::strstr(g.fs, "precision"), "desktop flavor must not carry precision qualifiers");
    GfxGLSL es;
    gfx_opengl_build_glsl(ID_TEX_SHADE_FOG, GFX_GLSL_300_ES, &es);
    CHECK(std::strstr(es.fs, "precision highp float;") != nullptr, "ES flavor needs precision");
}

static void test_shader_create_lookup_and_attribs() {
    reset_gl();
    GfxRenderingAPI *api = &gfx_opengl_api;
    api->init();
    CHECK(api->lookup_shader(ID_TEX_SHADE_FOG) == nullptr, "not created yet");
    ShaderProgram *prg = api->create_and_load_new_shader(ID_TEX_SHADE_FOG);
    CHECK(prg != nullptr && api->lookup_shader(ID_TEX_SHADE_FOG) == prg, "lookup returns the created program");
    CHECK(g_used_program != 0, "program must be in use after creation");

    GfxGLSL g;
    gfx_opengl_build_glsl(ID_TEX_SHADE_FOG, GFX_GLSL_330_CORE, &g);
    uint8_t n;
    bool tex[2];
    api->shader_get_info(prg, &n, tex);
    CHECK(n == g.num_inputs && tex[0] == g.used_textures[0] && tex[1] == g.used_textures[1], "shader info");

    // Pointers: one per attribute, consecutive offsets, stride = floats per vertex; the unused slots are disabled.
    CHECK((int)g_attrib_ptrs.size() == g.num_attribs, "pointer count %zu", g_attrib_ptrs.size());
    size_t off = 0;
    for (size_t i = 0; i < g_attrib_ptrs.size() && (int)i < g.num_attribs; i++) {
        CHECK(g_attrib_ptrs[i].index == i && g_attrib_ptrs[i].size == g.attrib_sizes[i], "attrib %zu index/size", i);
        CHECK(g_attrib_ptrs[i].stride == g.num_floats * 4, "attrib %zu stride %d", i, g_attrib_ptrs[i].stride);
        CHECK(g_attrib_ptrs[i].offset == off * 4, "attrib %zu offset %zu expected %zu", i, g_attrib_ptrs[i].offset, off * 4);
        off += g.attrib_sizes[i];
    }
    CHECK((int)g_disabled.size() == GFX_GL_MAX_ATTRIBS - g.num_attribs, "disabled slots %zu", g_disabled.size());

    // Drawing uploads the whole buffer and draws three vertices per triangle.
    float verts[5 * 3 * 4] = {0};
    api->draw_triangles(verts, g.num_floats * 3 * 2, 2);
    CHECK(g_buffer_bytes == (size_t)g.num_floats * 3 * 2 * 4, "uploaded %zu bytes", g_buffer_bytes);
    CHECK(g_draw_counts.size() == 1 && g_draw_counts[0] == 6, "drawArrays count");
}


// OpenGL 2.1: GLSL 120, attribute locations bound before linking, VAO optional.
static void test_legacy_gl21_flavor() {
    GfxGLSL g;
    const uint64_t id = ID_TEX_SHADE_FOG | SHADER_OPT_TEXTURE_EDGE; // texture edge variant, so no other test created it
    CHECK(gfx_opengl_build_glsl(id, GFX_GLSL_120, &g), "build 120");
    CHECK(has(g.vs, "#version 120") && has(g.fs, "#version 120"), "120 header");
    CHECK(!has(g.vs, "layout") && !has(g.fs, "layout"), "no layout qualifiers in GLSL 120");
    CHECK(has(g.vs, "attribute vec4 aVtxPos;") && has(g.vs, "varying vec2 vTexCoord;"), "attribute/varying in the vertex shader");
    CHECK(has(g.fs, "varying vec2 vTexCoord;") && !has(g.fs, "\nin ") && !has(g.fs, "\nout "), "varying in the fragment shader");
    CHECK(has(g.fs, "texture2D(uTex0") && !has(g.fs, " texture(uTex0"), "texture2D");
    CHECK(has(g.fs, "gl_FragColor") && !has(g.fs, "fragColor"), "gl_FragColor");
    CHECK(!has(g.fs, "precision"), "no precision qualifiers");
    CHECK(std::strcmp(g.attrib_names[0], "aVtxPos") == 0 && std::strcmp(g.attrib_names[1], "aTexCoord") == 0 &&
          std::strcmp(g.attrib_names[2], "aFog") == 0, "attribute names");
    GfxGLSL g330;
    gfx_opengl_build_glsl(id, GFX_GLSL_330_CORE, &g330);
    CHECK(g.num_attribs == g330.num_attribs && g.num_floats == g330.num_floats, "same vertex layout in every flavor");

    // A driver without a VAO: refused for 330 (core needs one), accepted for 120.
    reset_gl();
    auto gen = g_gl["glGenVertexArrays"], bind = g_gl["glBindVertexArray"];
    g_gl.erase("glGenVertexArrays");
    g_gl.erase("glBindVertexArray");
    std::fprintf(stderr, "(expected message follows)\n");
    gfx_opengl_set_glsl_flavor(GFX_GLSL_330_CORE);
    CHECK(!gfx_opengl_load_functions(get_proc), "330 needs a VAO");
    gfx_opengl_set_glsl_flavor(GFX_GLSL_120);
    CHECK(gfx_opengl_load_functions(get_proc), "120 works without a VAO");
    g_bound_attribs.clear();
    GfxRenderingAPI *api = &gfx_opengl_api;
    api->init();
    for (auto &c : g_calls) CHECK(c.find("VertexArray") == std::string::npos, "no VAO call without a VAO: %s", c.c_str());
    ShaderProgram *prg = api->create_and_load_new_shader(id);
    CHECK(prg != nullptr && g_used_program != 0, "program built");
    CHECK((int)g_bound_attribs.size() == g.num_attribs, "one bind per attribute, got %zu", g_bound_attribs.size());
    for (size_t i = 0; i < g_bound_attribs.size(); i++) {
        CHECK(g_bound_attribs[i].first == i && g_bound_attribs[i].second == g.attrib_names[i], "attrib %zu bound as %s", i, g_bound_attribs[i].second.c_str());
    }
    CHECK((int)g_attrib_ptrs.size() == g.num_attribs, "pointers set per shader without a VAO");

    // With a VAO available the 120 flavor still uses it.
    g_gl["glGenVertexArrays"] = gen;
    g_gl["glBindVertexArray"] = bind;
    CHECK(gfx_opengl_load_functions(get_proc), "120 with a VAO");
    reset_gl();
    api->init();
    bool used_vao = false;
    for (auto &c : g_calls) used_vao |= c.find("glGenVertexArrays") != std::string::npos;
    CHECK(used_vao, "120 uses the VAO when the driver has one");
    gfx_opengl_set_glsl_flavor(GFX_GLSL_330_CORE);
}

static void test_failed_shader_is_skipped_not_fatal() {
    reset_gl();
    g_fail_compile = true;
    GfxRenderingAPI *api = &gfx_opengl_api;
    std::fprintf(stderr, "(expected compile error messages follow)\n");
    const uint64_t id = ID_SHADE | SHADER_OPT_NOISE; // an id no earlier test created
    ShaderProgram *prg = api->create_and_load_new_shader(id);
    CHECK(prg != nullptr, "a failed shader still yields a program object for the core");
    CHECK(api->lookup_shader(id) == prg, "failure is cached, so it is not recompiled every draw");
    int compiles_after_first = g_compiles;
    api->lookup_shader(id);
    CHECK(g_compiles == compiles_after_first, "lookup must not recompile");
    float verts[40] = {0};
    api->draw_triangles(verts, 24, 1);
    CHECK(g_draw_counts.empty(), "a failed shader must not draw");
    uint8_t n;
    bool tex[2];
    api->shader_get_info(prg, &n, tex);
    CHECK(n == 1, "layout info is still correct (one combiner input: shade) so the core builds a valid vertex buffer, got %d", n);
    g_fail_compile = false;
}

static void test_shader_table_overflow() {
    reset_gl();
    GfxRenderingAPI *api = &gfx_opengl_api;
    // Create far more programs than the table holds. Nothing may crash, and the early ones must stay retrievable.
    std::fprintf(stderr, "(expected 'too many shader programs' messages follow)\n");
    std::vector<ShaderProgram *> first;
    for (uint32_t i = 0; i < 3500; i++) {
        uint64_t id = 0x00100000ull + i * 7; // distinct, no options
        ShaderProgram *p = api->create_and_load_new_shader(id);
        CHECK(p != nullptr, "program %u", i);
        if (i < 50) first.push_back(p);
    }
    for (uint32_t i = 0; i < 50; i++) {
        CHECK(api->lookup_shader(0x00100000ull + i * 7) == first[i], "early program %u lost", i);
    }
}

static void test_textures_and_sampler_state() {
    reset_gl();
    GfxRenderingAPI *api = &gfx_opengl_api;
    uint32_t t = api->new_texture();
    CHECK(t != 0, "texture id");
    api->select_texture(1, t);
    uint8_t rgba[4 * 2 * 4] = {0};
    api->upload_texture(rgba, 4, 2);
    CHECK(g_teximage_w == 4 && g_teximage_h == 2 && g_teximage_count == 1, "glTexImage2D %dx%d", g_teximage_w, g_teximage_h);
    // G_TX_CLAMP (2) -> CLAMP_TO_EDGE, G_TX_MIRROR (1) -> MIRRORED_REPEAT, 0 -> REPEAT, clamp wins over mirror.
    api->set_sampler_parameters(1, true, 2, 1);
    api->set_sampler_parameters(1, false, 0, 3);
    CHECK(g_active_texture == 0x84C0 + 1, "sampler params apply to the selected unit");
    CHECK(g_tex_wrap_s.size() == 2 && g_tex_wrap_s[0] == 0x812F && g_tex_wrap_s[1] == 0x2901, "wrap S");
    CHECK(g_tex_wrap_t.size() == 2 && g_tex_wrap_t[0] == 0x8370 && g_tex_wrap_t[1] == 0x812F, "wrap T");
    CHECK(g_tex_filter.size() == 2 && g_tex_filter[0] == 0x2601 && g_tex_filter[1] == 0x2600, "filter");
}

// Core and backend together: a shaded triangle and a textured one reach glDrawArrays and glTexImage2D.
static void test_core_with_real_backend() {
    reset_gl();
    rdram = (uint8_t *)std::calloc(1, 8 * 1024 * 1024);
    gfx_init_embedded(&gfx_opengl_api, rdram);
    gfx_set_dimensions(640, 480);
    const uint32_t MTX = 0x1000, VTX = 0x1100, TEX = 0x3000, DL = 0x2000;
    put_matrix_identity(MTX);
    put_vtx(VTX + 0, -1, -1, 0, 0, 0, 255, 0, 0, 255);
    put_vtx(VTX + 16, 1, -1, 0, 128, 0, 0, 255, 0, 255);
    put_vtx(VTX + 32, 0, 1, 0, 0, 128, 0, 0, 255, 255);
    for (int i = 0; i < 16; i++) h16(TEX + i * 2, 0xF801);
    dl_begin(DL);
    g_mtx(MTX, 0x04 | 0x02);
    g_mtx(MTX, 0x02);
    g_geometry_mode(0xFFFFFF, 0x4);
    g_combine_shade();
    g_vertex(VTX, 3, 0);
    g_tri1(0, 1, 2);
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
    CHECK(g_draw_counts.size() == 2, "draw calls %zu", g_draw_counts.size());
    for (int c : g_draw_counts) CHECK(c == 3, "one triangle per draw, got %d vertices", c);
    CHECK(g_teximage_count == 1 && g_teximage_w == 4 && g_teximage_h == 4, "texture upload %d (%dx%d)", g_teximage_count, g_teximage_w, g_teximage_h);
    CHECK(g_last_fs.find("texture(uTex0") != std::string::npos, "textured shader samples uTex0");

    // After the UI drew with the same context: restore + reapply must rebuild the program, attribute layout and VAO, and
    // the next display list must still draw.
    g_calls.clear();
    g_attrib_ptrs.clear();
    g_used_program = 0;
    gfx_opengl_restore_state();
    gfx_reapply_state();
    bool rebound_vao = false;
    for (auto &c : g_calls) rebound_vao |= c == "glBindVertexArray";
    CHECK(rebound_vao, "restore_state rebinds the vertex array");
    CHECK(g_used_program != 0, "reapply_state selects the current program again");
    CHECK(!g_attrib_ptrs.empty(), "reapply_state sets the attribute pointers again");
    g_draw_counts.clear();
    gfx_begin_frame();
    gfx_run_task(DL);
    gfx_end_frame();
    CHECK(g_draw_counts.size() == 2, "draws after reapply: %zu", g_draw_counts.size());
    std::free(rdram);
}

int main() {
    test_combiner_sources();
    test_loader();
    test_glsl_layout_matches_core_vertex_format();
    test_shader_create_lookup_and_attribs();
    test_failed_shader_is_skipped_not_fatal();
    test_legacy_gl21_flavor();
    test_textures_and_sampler_state();
    test_core_with_real_backend();
    test_shader_table_overflow(); // last: it fills the global shader table
    if (g_failures == 0) {
        std::printf("fast3d opengl backend: all checks passed\n");
        return 0;
    }
    std::printf("fast3d opengl backend: %d check(s) failed\n", g_failures);
    return 1;
}
