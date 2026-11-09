// Dumps generated GLSL for a spread of combiner ids as JSON lines, for compiling in a real GLSL compiler (see
// tests/fast3d/validate_glsl.js). Usage: dump_glsl <count> <330|300|120> > shaders.jsonl
#include <stdio.h>
#include <stdlib.h>

#include "gfx_cc.h"
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
    int ver = argc > 2 ? atoi(argv[2]) : 300;
    enum GfxGLSLFlavor flavor = ver == 330 ? GFX_GLSL_330_CORE : ver == 120 ? GFX_GLSL_120 : GFX_GLSL_300_ES;
    static struct GfxGLSL glsl;
    uint64_t state = 12345;
    int failed = 0;
    for (int i = 0; i < count; i++) {
        uint64_t id;
        if (i < 16 * 3) {
            // Every option combination with the common "shade only" and "texel0 * shade" mixes (raw RDP field numbering).
            const uint64_t base[3] = {gfx_cc_pack_cycle(15, 15, 31, 4, 7, 7, 7, 4),
                                      gfx_cc_pack_cycle(1, 15, 4, 7, 1, 7, 4, 7),
                                      gfx_cc_pack_cycle(1, 2, 3, 4, 1, 2, 3, 4)};
            id = base[i / 16] | ((uint64_t)(i % 16) << 57);
            if (i % 16 >= 8) id |= SHADER_OPT_TWO_CYCLE;
        } else {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            id = (state >> 8) & 0x00FFFFFFFFFFFFFFull;          // both cycles, any raw field values
            id |= ((state >> 3) & 0x1F) << 56;                    // two-cycle, alpha, fog, texture edge, noise
        }
        if (!gfx_opengl_build_glsl(id, flavor, &glsl)) {
            failed++;
            continue;
        }
        printf("{\"id\":\"%016llx\",\"vs\":", (unsigned long long)id);
        json_str(glsl.vs);
        printf(",\"fs\":");
        json_str(glsl.fs);
        printf("}\n");
    }
    fprintf(stderr, "dumped %d shaders, %d overflowed\n", count - failed, failed);
    return failed ? 1 : 0;
}
