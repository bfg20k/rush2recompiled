// OpenGL 3.3 core / 2.1 backend for the embedded Fast3D core. Adapted from the OpenGL backend of Emill/n64-fast3d-engine
// (see LICENSE.txt in this folder). Differences from upstream:
//  - core profile: GLSL 330 with fixed attribute locations and one vertex array object, instead of GLSL 110; a GLSL 120
//    flavor (attribute locations bound before linking, VAO optional) serves OpenGL 2.1 contexts;
//  - no link-time dependency on GL, SDL or GLEW: functions come from a loader passed in by the window layer;
//  - a shader that fails to compile or link is logged once and skipped, instead of abort();
//  - shader programs live in a hash table instead of a 64-entry pool (upstream overflowed it silently);
//  - bounds-checked shader source building and a default return in the combiner item lookup (upstream fell off the end).
#include "gfx_opengl.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "f3d_gbi.h"
#include "gfx_cc.h"
#include "gfx_rendering_api.h"

// ---- Minimal GL declarations ---------------------------------------------------------------------------------------

#ifdef _WIN32
#define GFXGL_APIENTRY __stdcall
#else
#define GFXGL_APIENTRY
#endif

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TRIANGLES 0x0004
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_LEQUAL 0x0203
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84

// X(return type, name, parameter list). Resolved by name in gfx_opengl_load_functions().
#define GFXGL_FUNCTIONS(X)                                                                                        \
    X(void, glGenBuffers, (GLsizei n, GLuint *buffers))                                                           \
    X(void, glBindBuffer, (GLenum target, GLuint buffer))                                                         \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage))                       \
    X(void, glBindAttribLocation, (GLuint program, GLuint index, const GLchar *name))                             \
    X(void, glEnableVertexAttribArray, (GLuint index))                                                            \
    X(void, glDisableVertexAttribArray, (GLuint index))                                                           \
    X(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)) \
    X(GLuint, glCreateShader, (GLenum type))                                                                      \
    X(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length))     \
    X(void, glCompileShader, (GLuint shader))                                                                     \
    X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint *params))                                          \
    X(void, glGetShaderInfoLog, (GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog))               \
    X(void, glDeleteShader, (GLuint shader))                                                                      \
    X(GLuint, glCreateProgram, (void))                                                                            \
    X(void, glAttachShader, (GLuint program, GLuint shader))                                                      \
    X(void, glLinkProgram, (GLuint program))                                                                      \
    X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint *params))                                        \
    X(void, glGetProgramInfoLog, (GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog))             \
    X(void, glDeleteProgram, (GLuint program))                                                                    \
    X(void, glUseProgram, (GLuint program))                                                                       \
    X(GLint, glGetUniformLocation, (GLuint program, const GLchar *name))                                          \
    X(void, glUniform1i, (GLint location, GLint v0))                                                              \
    X(void, glGenTextures, (GLsizei n, GLuint *textures))                                                         \
    X(void, glActiveTexture, (GLenum texture))                                                                    \
    X(void, glBindTexture, (GLenum target, GLuint texture))                                                       \
    X(void, glTexImage2D, (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels)) \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param))                                          \
    X(void, glPixelStorei, (GLenum pname, GLint param))                                                           \
    X(void, glEnable, (GLenum cap))                                                                               \
    X(void, glDisable, (GLenum cap))                                                                              \
    X(void, glDepthFunc, (GLenum func))                                                                           \
    X(void, glDepthMask, (GLboolean flag))                                                                        \
    X(void, glBlendFunc, (GLenum sfactor, GLenum dfactor))                                                        \
    X(void, glPolygonOffset, (GLfloat factor, GLfloat units))                                                     \
    X(void, glViewport, (GLint x, GLint y, GLsizei width, GLsizei height))                                        \
    X(void, glScissor, (GLint x, GLint y, GLsizei width, GLsizei height))                                         \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))                                              \
    X(void, glClearColor, (GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha))                              \
    X(void, glClear, (GLbitfield mask))

// Optional: absent from OpenGL 2.1 drivers without a vertex array object extension. Only the GLSL 120 flavor may go without.
#define GFXGL_OPTIONAL_FUNCTIONS(X)                                                                               \
    X(void, glGenVertexArrays, (GLsizei n, GLuint *arrays))                                                       \
    X(void, glBindVertexArray, (GLuint array))

#define GFXGL_DECLARE(ret, name, params) static ret(GFXGL_APIENTRY *name) params;
GFXGL_FUNCTIONS(GFXGL_DECLARE)
GFXGL_OPTIONAL_FUNCTIONS(GFXGL_DECLARE)
#undef GFXGL_DECLARE

static enum GfxGLSLFlavor selected_flavor = GFX_GLSL_330_CORE;

void gfx_opengl_set_glsl_flavor(enum GfxGLSLFlavor flavor) {
    selected_flavor = flavor;
}

bool gfx_opengl_load_functions(GfxGLGetProc get_proc) {
    if (get_proc == NULL) {
        return false;
    }
#define GFXGL_LOAD(ret, name, params)                                                    \
    name = (ret(GFXGL_APIENTRY *) params)get_proc(#name);                                \
    if (name == NULL) {                                                                  \
        fprintf(stderr, "fast3d: OpenGL function %s is not available\n", #name);         \
        return false;                                                                    \
    }
    GFXGL_FUNCTIONS(GFXGL_LOAD)
#undef GFXGL_LOAD
#define GFXGL_LOAD_OPTIONAL(ret, name, params) name = (ret(GFXGL_APIENTRY *) params)get_proc(#name);
    GFXGL_OPTIONAL_FUNCTIONS(GFXGL_LOAD_OPTIONAL)
#undef GFXGL_LOAD_OPTIONAL
    if (selected_flavor != GFX_GLSL_120 && (glGenVertexArrays == NULL || glBindVertexArray == NULL)) {
        fprintf(stderr, "fast3d: OpenGL function glGenVertexArrays is not available\n");
        return false;
    }
    return true;
}

// ---- GLSL generation -----------------------------------------------------------------------------------------------

struct StrBuf {
    char *data;
    size_t cap;
    size_t len;
    bool overflow;
};

static void sb_append(struct StrBuf *sb, const char *str) {
    size_t n = strlen(str);
    if (sb->len + n + 1 > sb->cap) {
        sb->overflow = true;
        return;
    }
    memcpy(sb->data + sb->len, str, n + 1);
    sb->len += n;
}

static void sb_line(struct StrBuf *sb, const char *str) {
    sb_append(sb, str);
    sb_append(sb, "\n");
}

static void sb_printf(struct StrBuf *sb, const char *fmt, ...) {
    char tmp[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        sb->overflow = true;
        return;
    }
    sb_append(sb, tmp);
}

static const char *shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a" :
                    (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)" : "vec3(texVal0.a, texVal0.a, texVal0.a)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
            default:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
        }
    }
    switch (item) {
        case SHADER_INPUT_1:
            return "vInput1.a";
        case SHADER_INPUT_2:
            return "vInput2.a";
        case SHADER_INPUT_3:
            return "vInput3.a";
        case SHADER_INPUT_4:
            return "vInput4.a";
        case SHADER_TEXEL0:
        case SHADER_TEXEL0A:
            return "texVal0.a";
        case SHADER_TEXEL1:
            return "texVal1.a";
        case SHADER_0:
        default:
            return "0.0";
    }
}

static void append_formula(struct StrBuf *sb, uint8_t c[2][4], bool do_single, bool do_multiply, bool do_mix, bool with_alpha, bool only_alpha, bool opt_alpha) {
    if (do_single) {
        sb_append(sb, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        sb_append(sb, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        sb_append(sb, " * ");
        sb_append(sb, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        sb_append(sb, "mix(");
        sb_append(sb, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        sb_append(sb, ", ");
        sb_append(sb, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        sb_append(sb, ", ");
        sb_append(sb, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        sb_append(sb, ")");
    } else {
        sb_append(sb, "(");
        sb_append(sb, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        sb_append(sb, " - ");
        sb_append(sb, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        sb_append(sb, ") * ");
        sb_append(sb, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        sb_append(sb, " + ");
        sb_append(sb, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    }
}

bool gfx_opengl_build_glsl(uint32_t shader_id, enum GfxGLSLFlavor flavor, struct GfxGLSL *out) {
    struct CCFeatures cc;
    gfx_cc_get_features(shader_id, &cc);

    memset(out, 0, sizeof(*out));
    struct StrBuf vs = {out->vs, sizeof(out->vs), 0, false};
    struct StrBuf fs = {out->fs, sizeof(out->fs), 0, false};
    const bool any_texture = cc.used_textures[0] || cc.used_textures[1];
    const int input_size = cc.opt_alpha ? 4 : 3;
    const bool noise = cc.opt_alpha && cc.opt_noise;

    out->num_inputs = cc.num_inputs;
    out->used_textures[0] = cc.used_textures[0];
    out->used_textures[1] = cc.used_textures[1];
    out->used_noise = noise;

    // Attribute layout, in the order the core writes each vertex: position, texcoord, fog, then the combiner inputs.
    out->attrib_sizes[out->num_attribs++] = 4;
    int tex_loc = -1, fog_loc = -1, input_loc[4] = {-1, -1, -1, -1};
    if (any_texture) {
        tex_loc = out->num_attribs;
        out->attrib_sizes[out->num_attribs++] = 2;
    }
    if (cc.opt_fog) {
        fog_loc = out->num_attribs;
        out->attrib_sizes[out->num_attribs++] = 4;
    }
    for (int i = 0; i < cc.num_inputs; i++) {
        input_loc[i] = out->num_attribs;
        out->attrib_sizes[out->num_attribs++] = (uint8_t)input_size;
    }
    for (int i = 0; i < out->num_attribs; i++) {
        out->num_floats += out->attrib_sizes[i];
    }

    const bool legacy = flavor == GFX_GLSL_120;
    const char *version = flavor == GFX_GLSL_300_ES ? "#version 300 es" : legacy ? "#version 120" : "#version 330 core";
    const char *vs_in = legacy ? "attribute" : "in";
    const char *vs_out = legacy ? "varying" : "out";
    const char *fs_in = legacy ? "varying" : "in";
    const char *tex_fn = legacy ? "texture2D" : "texture";
    // Fixed locations: a layout qualifier in the newer dialects, glBindAttribLocation (names recorded here) in GLSL 120.
    snprintf(out->attrib_names[0], sizeof(out->attrib_names[0]), "aVtxPos");
    if (tex_loc >= 0) {
        snprintf(out->attrib_names[tex_loc], sizeof(out->attrib_names[0]), "aTexCoord");
    }
    if (fog_loc >= 0) {
        snprintf(out->attrib_names[fog_loc], sizeof(out->attrib_names[0]), "aFog");
    }
    for (int i = 0; i < cc.num_inputs; i++) {
        snprintf(out->attrib_names[input_loc[i]], sizeof(out->attrib_names[0]), "aInput%d", i + 1);
    }
    char loc[32];

    // Vertex shader.
    sb_line(&vs, version);
    snprintf(loc, sizeof(loc), legacy ? "" : "layout(location = 0) ");
    sb_printf(&vs, "%s%s vec4 aVtxPos;\n", loc, vs_in);
    if (any_texture) {
        snprintf(loc, sizeof(loc), legacy ? "" : "layout(location = %d) ", tex_loc);
        sb_printf(&vs, "%s%s vec2 aTexCoord;\n", loc, vs_in);
        sb_printf(&vs, "%s vec2 vTexCoord;\n", vs_out);
    }
    if (cc.opt_fog) {
        snprintf(loc, sizeof(loc), legacy ? "" : "layout(location = %d) ", fog_loc);
        sb_printf(&vs, "%s%s vec4 aFog;\n", loc, vs_in);
        sb_printf(&vs, "%s vec4 vFog;\n", vs_out);
    }
    for (int i = 0; i < cc.num_inputs; i++) {
        snprintf(loc, sizeof(loc), legacy ? "" : "layout(location = %d) ", input_loc[i]);
        sb_printf(&vs, "%s%s vec%d aInput%d;\n", loc, vs_in, input_size, i + 1);
        sb_printf(&vs, "%s vec%d vInput%d;\n", vs_out, input_size, i + 1);
    }
    sb_line(&vs, "void main() {");
    if (any_texture) {
        sb_line(&vs, "vTexCoord = aTexCoord;");
    }
    if (cc.opt_fog) {
        sb_line(&vs, "vFog = aFog;");
    }
    for (int i = 0; i < cc.num_inputs; i++) {
        sb_printf(&vs, "vInput%d = aInput%d;\n", i + 1, i + 1);
    }
    sb_line(&vs, "gl_Position = aVtxPos;");
    sb_line(&vs, "}");

    // Fragment shader.
    sb_line(&fs, version);
    if (flavor == GFX_GLSL_300_ES) {
        sb_line(&fs, "precision highp float;");
        sb_line(&fs, "precision highp int;");
    }
    if (any_texture) {
        sb_printf(&fs, "%s vec2 vTexCoord;\n", fs_in);
    }
    if (cc.opt_fog) {
        sb_printf(&fs, "%s vec4 vFog;\n", fs_in);
    }
    for (int i = 0; i < cc.num_inputs; i++) {
        sb_printf(&fs, "%s vec%d vInput%d;\n", fs_in, input_size, i + 1);
    }
    if (!legacy) {
        sb_line(&fs, "out vec4 fragColor;");
    }
    if (cc.used_textures[0]) {
        sb_line(&fs, "uniform sampler2D uTex0;");
    }
    if (cc.used_textures[1]) {
        sb_line(&fs, "uniform sampler2D uTex1;");
    }
    if (noise) {
        sb_line(&fs, "uniform int frame_count;");
        sb_line(&fs, "uniform int window_height;");
        sb_line(&fs, "float random(in vec3 value) {");
        sb_line(&fs, "    float random = dot(sin(value), vec3(12.9898, 78.233, 37.719));");
        sb_line(&fs, "    return fract(sin(random) * 143758.5453);");
        sb_line(&fs, "}");
    }
    sb_line(&fs, "void main() {");
    if (cc.used_textures[0]) {
        sb_printf(&fs, "vec4 texVal0 = %s(uTex0, vTexCoord);\n", tex_fn);
    }
    if (cc.used_textures[1]) {
        sb_printf(&fs, "vec4 texVal1 = %s(uTex1, vTexCoord);\n", tex_fn);
    }
    sb_append(&fs, cc.opt_alpha ? "vec4 texel = " : "vec3 texel = ");
    if (!cc.color_alpha_same && cc.opt_alpha) {
        sb_append(&fs, "vec4(");
        append_formula(&fs, cc.c, cc.do_single[0], cc.do_multiply[0], cc.do_mix[0], false, false, true);
        sb_append(&fs, ", ");
        append_formula(&fs, cc.c, cc.do_single[1], cc.do_multiply[1], cc.do_mix[1], true, true, true);
        sb_append(&fs, ")");
    } else {
        append_formula(&fs, cc.c, cc.do_single[0], cc.do_multiply[0], cc.do_mix[0], cc.opt_alpha, false, cc.opt_alpha);
    }
    sb_line(&fs, ";");
    if (cc.opt_texture_edge && cc.opt_alpha) {
        sb_line(&fs, "if (texel.a > 0.3) texel.a = 1.0; else discard;");
    }
    if (cc.opt_fog) {
        if (cc.opt_alpha) {
            sb_line(&fs, "texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);");
        } else {
            sb_line(&fs, "texel = mix(texel, vFog.rgb, vFog.a);");
        }
    }
    if (noise) {
        sb_line(&fs, "texel.a *= floor(clamp(random(vec3(floor(gl_FragCoord.xy * (240.0 / float(window_height))), float(frame_count))) + texel.a, 0.0, 1.0));");
    }
    sb_printf(&fs, "%s = %s;\n", legacy ? "gl_FragColor" : "fragColor", cc.opt_alpha ? "texel" : "vec4(texel, 1.0)");
    sb_line(&fs, "}");

    return !vs.overflow && !fs.overflow;
}

// ---- Shader programs -----------------------------------------------------------------------------------------------

struct ShaderProgram {
    uint32_t shader_id;
    GLuint program; // 0 when compilation failed: the program is skipped at draw time
    uint8_t num_inputs;
    bool used_textures[2];
    bool used_noise;
    uint8_t num_floats;
    uint8_t num_attribs;
    uint8_t attrib_sizes[GFX_GL_MAX_ATTRIBS];
    GLint frame_count_location;
    GLint window_height_location;
};

#define SHADER_TABLE_SIZE 4096 // power of two; holds up to SHADER_TABLE_MAX programs before the overflow slot is used
#define SHADER_TABLE_MAX 3072

static struct ShaderProgram *shader_table[SHADER_TABLE_SIZE];
static size_t shader_count;
static struct ShaderProgram shader_overflow; // reused when the table is full; never drawn

static GLuint opengl_vbo;
static GLuint opengl_vao;
static struct ShaderProgram *current_program;
static uint32_t frame_count;
static uint32_t current_height = 1;

static size_t shader_slot(uint32_t shader_id) {
    uint32_t h = shader_id * 2654435761u;
    return (size_t)(h >> 20) & (SHADER_TABLE_SIZE - 1);
}

static bool gfx_opengl_z_is_from_0_to_1(void) {
    return false;
}

static void gfx_opengl_set_uniforms(struct ShaderProgram *prg) {
    if (prg->used_noise) {
        glUniform1i(prg->frame_count_location, (GLint)frame_count);
        glUniform1i(prg->window_height_location, (GLint)current_height);
    }
}

static void gfx_opengl_unload_shader(struct ShaderProgram *old_prg) {
    (void)old_prg;
}

static void gfx_opengl_load_shader(struct ShaderProgram *new_prg) {
    current_program = new_prg;
    if (new_prg->program == 0) {
        return;
    }
    glUseProgram(new_prg->program);
    size_t pos = 0;
    for (int i = 0; i < GFX_GL_MAX_ATTRIBS; i++) {
        if (i < new_prg->num_attribs) {
            glEnableVertexAttribArray((GLuint)i);
            glVertexAttribPointer((GLuint)i, new_prg->attrib_sizes[i], GL_FLOAT, GL_FALSE, (GLsizei)(new_prg->num_floats * sizeof(float)),
                                  (const void *)(pos * sizeof(float)));
            pos += new_prg->attrib_sizes[i];
        } else {
            glDisableVertexAttribArray((GLuint)i);
        }
    }
    gfx_opengl_set_uniforms(new_prg);
}

static GLuint compile_shader(GLenum type, const char *source, const char *what, uint32_t shader_id) {
    GLuint shader = glCreateShader(type);
    const GLchar *sources[1] = {source};
    glShaderSource(shader, 1, sources, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(shader, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "fast3d: %s shader for combiner 0x%08x failed to compile:\n%s\n", what, shader_id, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint build_program(const struct GfxGLSL *glsl, uint32_t shader_id) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, glsl->vs, "vertex", shader_id);
    GLuint fs = vs != 0 ? compile_shader(GL_FRAGMENT_SHADER, glsl->fs, "fragment", shader_id) : 0;
    if (vs == 0 || fs == 0) {
        if (vs != 0) {
            glDeleteShader(vs);
        }
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    for (int i = 0; i < glsl->num_attribs; i++) {
        glBindAttribLocation(program, (GLuint)i, glsl->attrib_names[i]); // already fixed by layout() in GLSL 330; needed for 120
    }
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(program, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "fast3d: program for combiner 0x%08x failed to link:\n%s\n", shader_id, log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static struct ShaderProgram *gfx_opengl_create_and_load_new_shader(uint32_t shader_id) {
    struct GfxGLSL glsl;
    bool built = gfx_opengl_build_glsl(shader_id, selected_flavor, &glsl);

    struct ShaderProgram *prg;
    if (shader_count >= SHADER_TABLE_MAX) {
        prg = &shader_overflow;
        memset(prg, 0, sizeof(*prg));
    } else {
        prg = (struct ShaderProgram *)calloc(1, sizeof(*prg));
        if (prg == NULL) {
            prg = &shader_overflow;
            memset(prg, 0, sizeof(*prg));
        }
    }

    prg->shader_id = shader_id;
    prg->num_inputs = (uint8_t)glsl.num_inputs;
    prg->used_textures[0] = glsl.used_textures[0];
    prg->used_textures[1] = glsl.used_textures[1];
    prg->used_noise = glsl.used_noise;
    prg->num_floats = (uint8_t)glsl.num_floats;
    prg->num_attribs = (uint8_t)glsl.num_attribs;
    memcpy(prg->attrib_sizes, glsl.attrib_sizes, sizeof(prg->attrib_sizes));

    if (prg != &shader_overflow) {
        if (built) {
            prg->program = build_program(&glsl, shader_id);
        } else {
            fprintf(stderr, "fast3d: shader source for combiner 0x%08x is too large\n", shader_id);
        }
        if (prg->program != 0) {
            glUseProgram(prg->program);
            if (prg->used_textures[0]) {
                glUniform1i(glGetUniformLocation(prg->program, "uTex0"), 0);
            }
            if (prg->used_textures[1]) {
                glUniform1i(glGetUniformLocation(prg->program, "uTex1"), 1);
            }
            if (prg->used_noise) {
                prg->frame_count_location = glGetUniformLocation(prg->program, "frame_count");
                prg->window_height_location = glGetUniformLocation(prg->program, "window_height");
            }
        }
        size_t slot = shader_slot(shader_id);
        while (shader_table[slot] != NULL) {
            slot = (slot + 1) & (SHADER_TABLE_SIZE - 1);
        }
        shader_table[slot] = prg;
        shader_count++;
    } else {
        fprintf(stderr, "fast3d: too many shader programs, combiner 0x%08x will not draw\n", shader_id);
    }

    gfx_opengl_load_shader(prg);
    return prg;
}

static struct ShaderProgram *gfx_opengl_lookup_shader(uint32_t shader_id) {
    size_t slot = shader_slot(shader_id);
    while (shader_table[slot] != NULL) {
        if (shader_table[slot]->shader_id == shader_id) {
            return shader_table[slot];
        }
        slot = (slot + 1) & (SHADER_TABLE_SIZE - 1);
    }
    return NULL;
}

static void gfx_opengl_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

// ---- Textures and fixed-function state -----------------------------------------------------------------------------

static uint32_t gfx_opengl_new_texture(void) {
    GLuint id = 0;
    glGenTextures(1, &id);
    return id;
}

static void gfx_opengl_select_texture(int tile, uint32_t texture_id) {
    glActiveTexture(GL_TEXTURE0 + (GLenum)tile);
    glBindTexture(GL_TEXTURE_2D, texture_id);
}

static void gfx_opengl_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
}

static GLint gfx_cm_to_opengl(uint32_t val) {
    if (val & G_TX_CLAMP) {
        return GL_CLAMP_TO_EDGE;
    }
    return (val & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static void gfx_opengl_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    glActiveTexture(GL_TEXTURE0 + (GLenum)tile);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gfx_cm_to_opengl(cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gfx_cm_to_opengl(cmt));
}

static void gfx_opengl_set_depth_test(bool depth_test) {
    if (depth_test) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
}

static void gfx_opengl_set_depth_mask(bool z_upd) {
    glDepthMask(z_upd ? GL_TRUE : GL_FALSE);
}

static void gfx_opengl_set_zmode_decal(bool zmode_decal) {
    if (zmode_decal) {
        glPolygonOffset(-2.0f, -2.0f);
        glEnable(GL_POLYGON_OFFSET_FILL);
    } else {
        glPolygonOffset(0.0f, 0.0f);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
}

static void gfx_opengl_set_viewport(int x, int y, int width, int height) {
    glViewport(x, y, width, height);
    current_height = height > 0 ? (uint32_t)height : 1;
}

static void gfx_opengl_set_scissor(int x, int y, int width, int height) {
    glScissor(x, y, width, height);
}

static void gfx_opengl_set_use_alpha(bool use_alpha) {
    if (use_alpha) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
}

static void gfx_opengl_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (current_program == NULL || current_program->program == 0) {
        return; // a shader that failed to build draws nothing
    }
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * buf_vbo_len), buf_vbo, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(3 * buf_vbo_num_tris));
}

static void gfx_opengl_init(void) {
    if (glGenVertexArrays != NULL && glBindVertexArray != NULL) {
        glGenVertexArrays(1, &opengl_vao);
        glBindVertexArray(opengl_vao);
    } // else OpenGL 2.1 without a VAO: attribute state is global, and load_shader sets it for each program
    glGenBuffers(1, &opengl_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
    glDepthFunc(GL_LEQUAL);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void gfx_opengl_restore_state(void) {
    if (glBindVertexArray != NULL && opengl_vao != 0) {
        glBindVertexArray(opengl_vao);
    }
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_SCISSOR_TEST);
    glDepthFunc(GL_LEQUAL);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void gfx_opengl_on_resize(void) {
}

static void gfx_opengl_start_frame(void) {
    frame_count++;
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE); // must be set for the depth clear to apply
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
}

static void gfx_opengl_end_frame(void) {
}

static void gfx_opengl_finish_render(void) {
}

struct GfxRenderingAPI gfx_opengl_api = {
    gfx_opengl_z_is_from_0_to_1,
    gfx_opengl_unload_shader,
    gfx_opengl_load_shader,
    gfx_opengl_create_and_load_new_shader,
    gfx_opengl_lookup_shader,
    gfx_opengl_shader_get_info,
    gfx_opengl_new_texture,
    gfx_opengl_select_texture,
    gfx_opengl_upload_texture,
    gfx_opengl_set_sampler_parameters,
    gfx_opengl_set_depth_test,
    gfx_opengl_set_depth_mask,
    gfx_opengl_set_zmode_decal,
    gfx_opengl_set_viewport,
    gfx_opengl_set_scissor,
    gfx_opengl_set_use_alpha,
    gfx_opengl_draw_triangles,
    gfx_opengl_init,
    gfx_opengl_on_resize,
    gfx_opengl_start_frame,
    gfx_opengl_end_frame,
    gfx_opengl_finish_render,
};
