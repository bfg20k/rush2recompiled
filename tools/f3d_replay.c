// Replays a Fast3D frame dump (f3d_frame_<n>.bin, written by the game with RUSH2_F3D_DUMP_FRAMES or the f3d_dump_now
// file, see lib/fast3d/gfx_pc.c "debugging") through the Fast3D core and writes the result as a PPM.
//   xvfb-run -a tmp/f3d-build/f3d_replay f3d_frame_120.bin out.ppm [glsl120|glsl330]
// Needs SDL2 and a GL driver (Mesa llvmpipe is fine). Convert with: python3 -c "from PIL import Image; Image.open('out.ppm').save('out.png')"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "gfx_pc.h"
#include "gfx_rendering_api.h"
#include "gfx_opengl.h"

typedef void (*ReadPixelsFn)(int, int, int, int, unsigned, unsigned, void *);

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s frame.bin out.ppm [glsl120|glsl330]\n", argv[0]);
        return 2;
    }
    const int legacy = argc > 3 && strcmp(argv[3], "glsl120") == 0;
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) { perror(argv[1]); return 1; }
    char magic[8];
    uint32_t dims[2];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "F3DDUMP2", 8) != 0 || fread(dims, 4, 2, f) != 2) {
        fprintf(stderr, "not a frame dump\n");
        return 1;
    }
    const uint32_t w = dims[0], h = dims[1];
    uint8_t *rdram = (uint8_t *)calloc(1, 0x20000000u);
    if (rdram == NULL) return 1;

    SDL_Init(SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, legacy ? 2 : 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, legacy ? 1 : 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, legacy ? 0 : SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win = SDL_CreateWindow("replay", 0, 0, (int)w, (int)h, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext gl = win ? SDL_GL_CreateContext(win) : NULL;
    if (gl == NULL) { fprintf(stderr, "no GL context: %s\n", SDL_GetError()); return 1; }
    gfx_opengl_set_glsl_flavor(legacy ? GFX_GLSL_120 : GFX_GLSL_330_CORE);
    if (!gfx_opengl_load_functions((GfxGLGetProc)SDL_GL_GetProcAddress)) return 1;
    ReadPixelsFn read_pixels = (ReadPixelsFn)SDL_GL_GetProcAddress("glReadPixels");
    gfx_init_embedded(&gfx_opengl_api, rdram);
    gfx_set_dimensions(w, h);
    if (getenv("RUSH2_F3D_HUD_SHARP")) gfx_set_hud_filtering(false); // HUD Filtering = Sharp
    gfx_begin_frame();

    uint32_t head[3];
    int tasks = 0;
    uint8_t page[4096];
    while (fread(head, 4, 3, f) == 3) {
        if (head[0] != 0x4B534154u) { fprintf(stderr, "stray record at the end of the dump ignored\n"); break; } // older dumps
        for (uint32_t i = 0; i < head[2]; i++) {
            uint32_t idx;
            if (fread(&idx, 4, 1, f) != 1 || fread(page, 1, 4096, f) != 4096) { fprintf(stderr, "truncated\n"); return 1; }
            memcpy(rdram + (size_t)idx * 4096, page, 4096);
        }
        fprintf(stderr, "task %d: dl 0x%08x, %u pages\n", tasks++, head[1], head[2]);
        gfx_run_task(head[1]);
    }
    gfx_end_frame();

    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 4);
    read_pixels(0, 0, (int)w, (int)h, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, px);
    FILE *o = fopen(argv[2], "wb");
    fprintf(o, "P6\n%u %u\n255\n", w, h);
    for (int y = (int)h - 1; y >= 0; y--) { // GL rows run bottom to top
        for (uint32_t x = 0; x < w; x++) fwrite(px + ((size_t)y * w + x) * 4, 1, 3, o);
    }
    fclose(o);
    fprintf(stderr, "wrote %s (%ux%u)\n", argv[2], w, h);
    return 0;
}
