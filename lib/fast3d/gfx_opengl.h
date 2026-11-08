// OpenGL backend for the embedded Fast3D core (docs/fast3d_port_plan.md, phase 2).
//
// Targets an OpenGL 3.3 core profile context, or a legacy OpenGL 2.1 context (GLSL 1.20; see gfx_opengl_set_glsl_flavor).
// GLES 3.0 / WebGL 2 is a shader flavor used by the offline shader test. The backend never links against GL: the
// window layer creates the context, then hands over a proc-address function (SDL_GL_GetProcAddress, wglGetProcAddress,
// eglGetProcAddress, ...) through gfx_opengl_load_functions(). On Windows the function must also resolve the OpenGL 1.1
// entry points (SDL_GL_GetProcAddress does; a bare wglGetProcAddress does not).
#ifndef GFX_OPENGL_H
#define GFX_OPENGL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GfxRenderingAPI;

typedef void *(*GfxGLGetProc)(const char *name);

// Resolves every GL function the backend uses. Returns false (and prints the first missing name to stderr) if one is absent.
// Call with the context current, before gfx_init_embedded().
bool gfx_opengl_load_functions(GfxGLGetProc get_proc);

// The rendering backend to pass to gfx_init_embedded(). Its init() creates the vertex array and buffer, so it must run
// on the thread that owns the GL context.
extern struct GfxRenderingAPI gfx_opengl_api;

// Rebinds the backend's vertex array and buffer and re-establishes the fixed state it set at init, after the UI or
// anything else drew with the same context. Follow it with gfx_reapply_state().
void gfx_opengl_restore_state(void);

// GLSL generation, exposed so it can be compiled offline in tests.
enum GfxGLSLFlavor {
    GFX_GLSL_330_CORE, // desktop OpenGL 3.3 core
    GFX_GLSL_300_ES,   // OpenGL ES 3.0 / WebGL 2 (used by the offline shader test)
    GFX_GLSL_120       // desktop OpenGL 2.1: attribute/varying, texture2D, gl_FragColor, no layout qualifiers or VAO
};

// Picks the GLSL flavor the backend compiles with (default GFX_GLSL_330_CORE). Call before gfx_init_embedded(). With
// GFX_GLSL_120 the vertex array object is optional: it is used if the driver exposes one, and attribute pointers are
// set per shader either way.
void gfx_opengl_set_glsl_flavor(enum GfxGLSLFlavor flavor);

#define GFX_GL_MAX_ATTRIBS 7 // position, texcoord, fog, up to four combiner inputs

struct GfxGLSL {
    char vs[4096];
    char fs[8192];
    int num_inputs;
    bool used_textures[2];
    bool used_noise;
    int num_attribs;                          // vertex attributes, at locations 0..num_attribs-1
    uint8_t attrib_sizes[GFX_GL_MAX_ATTRIBS]; // floats per attribute
    int num_floats;                           // floats per vertex, the sum of attrib_sizes
    char attrib_names[GFX_GL_MAX_ATTRIBS][12]; // attribute i's name in the vertex shader (bound to location i before linking)
};

// Builds the vertex and fragment shader for a combiner id (see gfx_cc.h). Returns false if a source buffer overflowed.
bool gfx_opengl_build_glsl(uint32_t shader_id, enum GfxGLSLFlavor flavor, struct GfxGLSL *out);

#ifdef __cplusplus
}
#endif

#endif
