// Embedded Fast3D core API (lib/fast3d). The recomp renderer context calls these; see docs/fast3d_port_plan.md.
#ifndef GFX_PC_H
#define GFX_PC_H

#include <stdbool.h>
#include <stdint.h>

struct GfxRenderingAPI;

struct GfxDimensions {
    uint32_t width, height;
    float aspect_ratio;
};

extern struct GfxDimensions gfx_current_dimensions;

#ifdef __cplusplus
extern "C" {
#endif

// rdram is the recompiled game's RDRAM base (N64Recomp word-swapped layout, see f3d_gbi.h). Calls rapi->init().
void gfx_init_embedded(struct GfxRenderingAPI *rapi, uint8_t *rdram);
// The drawable size in pixels; the 320x240 N64 screen is scaled to it.
void gfx_set_dimensions(uint32_t width, uint32_t height);
void gfx_set_game_area(int x, int y, uint32_t width, uint32_t height);
struct GfxRenderingAPI *gfx_get_current_rendering_api(void);
void gfx_begin_frame(void);
// Interprets the F3DEX2 display list at dl_addr (an OSTask data_ptr), then flushes buffered triangles.
void gfx_run_task(uint32_t dl_addr);
void gfx_end_frame(void);
// For after another renderer (the UI) used the GL context: re-applies the state the core believes is set. Call
// gfx_opengl_restore_state() first.
void gfx_reapply_state(void);

#ifdef __cplusplus
}
#endif

#endif
