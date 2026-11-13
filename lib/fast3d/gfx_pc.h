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

// The tile a draw samples, as the texture hooks see it (the RDP tile registers plus the sampled size in texels).
struct GfxTexInfo {
    uint8_t fmt, siz, palette, cms, cmt, masks, maskt, shifts, shiftt;
    uint16_t line, tmem, uls, ult, lrs, lrt;
    uint32_t tlut_mode; // G_SETOTHERMODE_H's G_MDSFT_TEXTLUT bits, still in place (0, 2 << 14 or 3 << 14)
    bool world;         // drawn as 3D geometry rather than a 2D rectangle
};

// Receives a replacement image (RGBA8) while the hook's call is running; the pointer is not kept.
typedef void (*GfxUploadFn)(const uint8_t *rgba, uint32_t width, uint32_t height, void *ctx);

// How the host swaps textures at run time (the live replacements the recomp's texture upscaling and mod cars use).
// All hooks run on the render thread. Any may be NULL.
struct GfxTextureHooks {
    // A texture is about to be shown: `tmem` is TMEM (4096 bytes, canonical layout, palette words not included) and
    // `tlut` the 256 palette entries. Returns the texture's replacement hash, or 0 for a texture that can't be replaced.
    uint64_t (*drawn)(const struct GfxTexInfo *info, const uint8_t *tmem, const uint16_t *tlut);
    // Looks up the replacement for `hash`. Returns 0 if there is none, 1 if the one with `*serial` is already shown,
    // else calls `upload` with the image, stores its serial in `*serial` and returns 2.
    int (*replace)(uint64_t hash, uint32_t *serial, GfxUploadFn upload, void *ctx);
    // Changes whenever a replacement is added or removed, so a texture already shown knows to look again.
    uint32_t (*generation)(void);
};
void gfx_set_texture_hooks(const struct GfxTextureHooks *hooks);

// rdram is the recompiled game's RDRAM base (N64Recomp word-swapped layout, see f3d_gbi.h). Calls rapi->init().
void gfx_init_embedded(struct GfxRenderingAPI *rapi, uint8_t *rdram);
// The drawable size in pixels; the 320x240 N64 screen is scaled to it.
void gfx_set_dimensions(uint32_t width, uint32_t height);
// Off: 2D texture rectangles (HUD, menus) are sampled nearest instead of bilinear. Callable from any thread.
void gfx_set_hud_filtering(bool on);
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
