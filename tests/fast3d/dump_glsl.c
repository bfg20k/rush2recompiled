// Dumps generated GLSL for a spread of combiner ids as JSON lines, for compiling in a real GLSL compiler (see
// tests/fast3d/validate_glsl.js). Usage: dump_glsl <count> <330|300> > shaders.jsonl
#include <stdio.h>
#include <stdlib.h>

#include "gfx_opengl.h"

static void json_str(const char *s) {
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            putchar('\\');
            putchar(*s);
        } else if (*s == '\n') {
            fputs("\\n", stdout);
        } else {
            putchar(*s);
        }
    }
    putchar('"');
}

int main(int argc, char **argv) {
    int count = argc > 1 ? atoi(argv[1]) : 2000;
    enum GfxGLSLFlavor flavor = (argc > 2 && atoi(argv[2]) == 330) ? GFX_GLSL_330_CORE : GFX_GLSL_300_ES;
    static struct GfxGLSL glsl;
    uint32_t state = 12345;
    int failed = 0;
    for (int i = 0; i < count; i++) {
        uint32_t id;
        if (i < 16 * 3) {
            // Every option combination with the common "shade only" and "texel0 * shade" mixes.
            const uint32_t base[3] = {0x0 | (4 << 9) | (4 << 21), 0x5 | (4 << 3) | (4 << 12) | (4 << 24) | 0, (5) | (4 << 6) | (0 << 3)};
            id = base[i / 16] | ((uint32_t)(i % 16) << 24);
        } else {
            state = state * 1664525u + 1013904223u;
            id = (state & 0x00FFFFFFu) | (((state >> 24) & 0xF) << 24);
        }
        if (!gfx_opengl_build_glsl(id, flavor, &glsl)) {
            failed++;
            continue;
        }
        printf("{\"id\":%u,\"vs\":", id);
        json_str(glsl.vs);
        printf(",\"fs\":");
        json_str(glsl.fs);
        printf("}\n");
    }
    fprintf(stderr, "dumped %d shaders, %d overflowed\n", count - failed, failed);
    return failed ? 1 : 0;
}
