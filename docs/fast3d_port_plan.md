# Replacing RT64 with Fast3D

Goal: render Rush 2 with a Fast3D-style renderer instead of RT64. This is a multi-phase port. Phase 1 (the interpreter
core) is done and tested offline; nothing in the game uses it yet, and RT64 is still the renderer that runs.

## Status

| Phase | What | State |
| --- | --- | --- |
| 1 | `lib/fast3d`: RDRAM-aware F3DEX2 interpreter, tested against a null backend | Done, `tests/fast3d/test_core.cpp` passes |
| 2 | OpenGL rendering backend (`lib/fast3d/gfx_opengl.c`): 3.3 core, plus an OpenGL 2.1 / GLSL 120 flavor | Done and tested against a fake GL and a real GLSL compiler; never run on a real GPU |
| 3 | `Fast3DContext : ultramodern::renderer::RendererContext` (`src/fast3d_context.{h,cpp}`), GL context, present, selected with `RUSH2_RENDERER=fast3d` | Written, syntax-checked against stub SDL headers only; never built with real SDL or run |
| 4 | UI: an OpenGL RmlUi renderer (`src/ui_gl_renderer.cpp`) and external-renderer hooks in RecompFrontend | Written, syntax-checked against RmlUi 6.0 headers; never linked or run |
| 5 | Remove or guard every Rush 2 feature that reaches into RT64 internals; CMake switch | Not started |

Nothing from phases 3-5 can be built or run without the game's generated sources (which need the ROM), a GPU and a
window, so each phase needs an in-game check by someone with those.

## Running the tests

```
cmake -S lib/fast3d -B tmp/fast3d && cmake --build tmp/fast3d && ctest --test-dir tmp/fast3d --output-on-failure
```

or configure the main project with `-DRUSH2_FAST3D=ON`. `fast3d_core` builds a fake RDRAM, hand-encodes F3DEX2 display lists
and checks the triangles and texture uploads a null backend receives. It covers vertex, matrix and light decoding,
segment addressing, nested display lists, G_TRI2 and G_QUAD, 16-bit texture decoding from an unaligned address, the
content-keyed texture cache, and malformed lists (self-branching, runaway recursion, corrupt vertex counts).

`fast3d_opengl` runs the OpenGL backend against a fake GL that records calls (`tests/fast3d/test_opengl.cpp`) and also
drives the real core and backend together. It covers the function loader, that the vertex attribute layout matches the
vertex the core writes, shader caching, a shader that fails to compile (logged and skipped, not fatal), table overflow,
texture upload and wrap/filter mapping.

The generated GLSL is checked by a real compiler, outside ctest because it needs node and playwright:

```
cmake --build tmp/fast3d --target fast3d_dump_glsl
./tmp/fast3d/fast3d_dump_glsl 4000 300 > tmp/shaders.jsonl
NODE_PATH=$(npm root -g) node tests/fast3d/validate_glsl.js tmp/shaders.jsonl
```

This compiles and links 4000 combiner shaders (every option bit combination plus a pseudo-random spread of mux values) in
WebGL2 on Chromium's SwiftShader. It uses the `#version 300 es` flavor, which differs from the shipping `#version 330 core`
only in the version line and precision qualifiers, so it checks the shader bodies and attribute layout but not the 330
dialect itself. A real GPU driver may still reject something a software compiler accepts.

## The OpenGL backend

`lib/fast3d/gfx_opengl.{c,h}` adapts upstream's OpenGL backend:

- **GL 3.3 core profile**, GLSL 330 with fixed attribute locations (position, texcoord, fog, combiner inputs) and one VAO.
  Upstream used GLSL 110, which a core context rejects.
- **No link-time GL.** The window layer creates the context and passes a proc-address function to
  `gfx_opengl_load_functions()` (for example `SDL_GL_GetProcAddress`). On Windows it must also resolve the GL 1.1 entry
  points, which `SDL_GL_GetProcAddress` does and a bare `wglGetProcAddress` does not.
- **No `abort()`.** A shader that fails to compile or link is logged to stderr once and skipped at draw time.
- Shader programs sit in a hash table (3072 programs) instead of upstream's 64-entry pool, which overflowed silently.
  Past the limit a combiner is logged and not drawn.
- Bounds-checked source building, and a default return in the combiner lookup (upstream fell off the end of it).

What the backend leaves to phase 3: it draws into whatever framebuffer is bound, clears color and depth at
`gfx_begin_frame()`, and does not create the window, swap buffers, handle the default framebuffer's size, or letterbox
the 4:3 image. The viewport and scissor are in window pixels, as the core computes them from `gfx_set_dimensions()`.

## RDRAM layout (why the core is not just upstream Fast3D)

N64Recomp stores RDRAM as 32-bit words that already hold the N64's value in host byte order:

- a word at N64 address A (4-byte aligned) reads natively as `*(uint32_t *)(rdram + A)`;
- a byte at A is `rdram[A ^ 3]`; a halfword at A is the two bytes at `A ^ 3` and `(A + 1) ^ 3`, high byte first.

RT64 relies on the same layout (`RDRAM[addr ^ 3]` in `hle/rt64_rdp.cpp`). So `Gfx` commands and fixed-point matrices
are read as words directly, while vertices, lights, viewports, texture bytes and palettes go through the accessors in
`lib/fast3d/f3d_gbi.h`. Textures and palettes are copied out of RDRAM into canonical byte order when the game loads
them (like TMEM), and the texture cache is keyed by content, because the game rewrites textures in place.

## What changed from upstream Fast3D

`lib/fast3d/gfx_pc.c` is Emill/n64-fast3d-engine's `gfx_pc.c` with these edits (the script that applied them was a one-off;
the diff against upstream is the record):

- own `f3d_gbi.h` (F3DEX2 constants checked against RT64's `rt64_gbi_f3dex2.cpp` and `rt64_f3d_defines.h`) instead of `<PR/gbi.h>`;
- segment table (`G_MW_SEGMENT`) and `seg_addr()` resolve into RDRAM, masked to 64 MB like RT64;
- RDRAM readers for vertices, lights and viewports; TMEM-style texture and palette snapshots; content-keyed cache;
- F3DEX2 `G_TRI2` and `G_QUAD` (upstream only handled them for F3DEX1);
- hardening: no `abort()` or `assert()` on unsupported formats (a 2x2 magenta texture instead), vertex and triangle index
  bounds, display list depth limit of 18 and a command budget so a self-branching list ends;
- no window manager API and no clock: embedded entry points `gfx_init_embedded`, `gfx_set_dimensions`, `gfx_begin_frame`,
  `gfx_run_task(dl_addr)` and `gfx_end_frame` (`gfx_pc.h`).

## The seam RT64 plugs into

`ultramodern::renderer::RendererContext` (N64ModernRuntime, `ultramodern/renderer_context.hpp`) is what the runtime talks to:
`send_dl(const OSTask *)`, `send_dummy_workload(fb_address)`, `update_screen()`, `update_config()`, `shutdown()`,
`get_display_framerate()`, `get_resolution_scale()`. RecompFrontend implements it as `recompui::renderer::RT64Context`
(`recompui/src/renderer/rt64_render_context.cpp`) and `src/main.cpp` installs it through
`ultramodern::renderer::callbacks_t::create_render_context`.

A `Fast3DContext` implements the same interface: `send_dl` calls `gfx_run_task(task->t.data_ptr & 0x3FFFFFF)` (after
`gfx_begin_frame` on the first task of a frame), `update_screen` ends the frame and presents. `send_dummy_workload` must
clear the frame the way RT64's fill-rect does. The game also needs `get_vi_regs()` data for the presented size.

## Phase 3: the runtime context

Build with `-DRUSH2_FAST3D=ON` and launch with `RUSH2_RENDERER=fast3d`; without the variable RT64 runs as before.
`create_window` adds `SDL_WINDOW_OPENGL` instead of the Vulkan flag, and `renderer_callbacks.create_render_context`
returns a `Fast3DContext`. The context makes a GL 3.3 core context on the render thread, loads GL through
`SDL_GL_GetProcAddress`, and per frame: first `send_dl` (or `send_dummy_workload`) clears and begins the frame,
each task runs `gfx_run_task(data_ptr & 0x3FFFFFF)`, `update_screen` ends the frame and swaps. Not done yet: the 4:3
image is stretched to the window (no letterbox or aspect options), config options other than fullscreen are ignored,
the selection is an environment variable rather than a UI toggle (CLAUDE.md wants one; phase 5), and the menus are drawn by the phase 4 renderer below.

## OpenGL 2.1

`gfx_opengl_set_glsl_flavor(GFX_GLSL_120)` (before `gfx_opengl_load_functions`) makes the backend emit GLSL 1.20
(`attribute`/`varying`, `texture2D`, `gl_FragColor`, no `layout`) and bind attribute locations with
`glBindAttribLocation` before linking. The vertex array object is optional in that flavor: used when the driver has
one, otherwise the attribute pointers are set per shader (they are global state in a 2.1 context). `Fast3DContext`
asks for a 3.3 core context first and falls back to 2.1; `RUSH2_GL=21` forces the fallback for testing. The 120 shaders
are compiled in WebGL 1 (GLSL ES 1.00, a little stricter than 1.20) by `validate_glsl.js ... webgl1`:
`fast3d_dump_glsl 4000 120 > tmp/shaders120.jsonl`. A real 2.1 driver may still differ.

## Phase 4: the UI

RecompFrontend runs its whole UI (launcher, menus, input events) from RT64's render hooks, so with another renderer
nothing would ever start the launcher. `lib/patches/RecompFrontend.patch` now adds `recompui::ExternalRenderer`,
`init_external_renderer()`, `draw_external(width, height)` and `deinit_external_renderer()`: the same UI code path with
a caller-supplied RmlUi render interface instead of the plume one. The patch was made in a scratch copy of the pinned
RecompFrontend commit and applies cleanly to it; `python tools/lib_patch.py save` should reproduce it.

`src/ui_gl_renderer.cpp` is that render interface: a legacy (`RenderInterfaceCompatibility`) interface like the RT64
one, so RmlUi hands it unpremultiplied colors and textures and it blends with `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` (alpha:
`ONE`/`ONE_MINUS_SRC_ALPHA`) as the RT64 one does. Vertices are transformed on the CPU (no matrix layout assumptions),
the scissor is flipped to GL's origin, and images the game names (`?/rush2/launcher_logo`, thumbnails) are queued from
any thread and decoded with `lib/stb/stb_image.h` (v2.30, public domain / MIT; PNG, JPEG, TGA, BMP) on first use. Two
built-in pixels (handle 0 white, 1 transparent) match the RT64 renderer.

`Fast3DContext::update_screen()` ends the game frame, draws the UI over it, then calls `gfx_opengl_restore_state()` and
`gfx_reapply_state()`. The UI shares the GL context, and the core caches depth, blend, viewport, shader and texture
state, so both rebuild what they believe is set (covered by `test_opengl`).

Limits: the UI only draws on frames the game presented (30 fps in game; before the game starts the runtime's dummy
workload gives one per VI), so UI input is handled at that rate; the Graphics tab still shows RT64 options that do
nothing; RT64 is still compiled and linked, it is just never started. Texture packs, font packs and other Rush 2 code
that call into RT64 (`texture_upscale.cpp`, `fonts.cpp`, `rush2049_dc_pack.cpp`) are phase 5 and may misbehave or crash
under Fast3D.

## Things that make this bigger than the renderer

- **The UI drew through RT64 (phase 4 adds an OpenGL path, below).** `recompui/src/renderer/ui_renderer.cpp` (727 lines) is an RmlUi render interface built on
  RT64's plume RHI and hooked in through `rt64_render_hooks.h`. Without RT64 it needs a new RmlUi renderer (RmlUi ships
  a GL3 one) and the hook replaced. The graphics tab (`ui_config_tab_graphics.cpp`) and `recompui/renderer.h` use RT64
  types (`RT64::UserConfiguration`, MSAA, high-precision framebuffer) and need a Fast3D-side equivalent.
- **Submodule patches.** RT64 changes live in `lib/patches/rt64.patch` (about 5,900 lines) and RecompFrontend's in
  `lib/patches/RecompFrontend.patch`; both are applied at configure time. Removing RT64 means changing the RecompFrontend
  patch, which is regenerated with `python tools/lib_patch.py save`.
- **Rush 2 features built on RT64 internals** (39 source files reference it): frame interpolation and widescreen through
  RT64's extended GBI (`rt64_extended_gbi.h`, 12 uses), texture packs and replacement (`texture_upscale*.cpp`,
  `texture_origin.cpp`, `texture_derive.cpp`, the live-texture and TMEM-hash headers), the shader cache and display-list
  fault reporting in `main.cpp`, and the compute-shader effects (car reflections, ambient occlusion, sky fog). Per
  CLAUDE.md, anything that changes base behavior needs a toggle, so the Fast3D renderer should be a selectable backend
  (default RT64) until it reaches parity.

## Known limits of upstream Fast3D that Rush 2 will hit

- Single-cycle color combiner only; the second cycle of 2-cycle modes is ignored (TEXEL1, LOD and chained effects).
- No TMEM emulation: `G_LOADTILE` with a non-zero origin and `G_LOADBLOCK` with a non-zero start are approximated
  (`SUPPORT_CHECK` is a no-op here so they degrade instead of crashing); 4-bit palette selection is unsupported.
- No framebuffer effects: reading the color or depth image back, copying the framebuffer to a texture, and so on.
- No `G_BRANCH_Z`, `G_CULLDL`, `G_MODIFYVTX`, `G_RDPSETOTHERMODE`, blend color, primitive depth or key color.
- Texture sizes derive from the tile's line size and the load size, not from `G_SETTILESIZE`.
- Rush 2's own display lists (HUD, battle renderer, the custom tracks) were only ever checked against RT64.

## Licensing

Upstream Fast3D's `LICENSE.txt` (kept in `lib/fast3d`) is a custom BSD-style text whose second clause disallows binary
redistribution except for binaries that contain no assets you do not have the right to distribute. A recompiled Rush 2
build contains no game assets (it reads the user's ROM at run time), but check that clause before shipping binaries.
Project-Midas ships a copy of Fast3D under an MIT license; that is a different, GoldenEye-specific fork and is not used here.

## Bring-up fixes after the first in-game run

First screenshots showed black/speckled ground, white cars, magenta noise on billboards. Causes found offline:

- **LOADTLUT count**: bits 14..23 of word 1 hold `count - 1` as a plain integer (gDPLoadTLUT_pal16 = 15, pal256 = 255).
  The core shifted it down by 2 again, so only a quarter of every palette loaded. The test encoder had the same
  mistake, which is why the tests passed. Both fixed; tests now use real macro encodings.
- **Direct heap pointers**: the recompiled heap starts at 0x01000000, above what a 24-bit segment offset reaches.
  `seg_addr` read `0x81xxxxxx` as segment 1 + offset, returning unrelated low memory. Addresses with bit 31 set are now
  physical (`& 0x1FFFFFFF`); the old 64 MB mask became 512 MB.
- **Lights**: `MAX_LIGHTS` was 2 (SM64); F3DEX2 allows 7. With 3+ lights the ambient colour was read out of bounds
  (white cars). Count is clamped, and G_MOVEMEM of a light now recomputes direction coefficients.
- G_SETTIMG width is 12 bits (was 10). `tile_dims` applies the mask after the no-size fallback.
- stderr lines prefixed `[fast3d]` list each distinct combiner/render mode and any unsupported texture, once each.
  Still open: only cycle 0 of the combiner is used (2-cycle modes); check the `cycle=` field in those lines.

## Second round (after the first in-game screenshots with the fixes above)

Nearly every 3D draw is in 2-cycle mode with G_FOG on, so the combiner was the next big gap:

- **Combiner rewritten** (`gfx_cc.{h,c}`, `gfx_opengl.c`): the shader id is now 64 bits holding both cycles' raw
  G_SETCOMBINE fields plus option bits. Every slot is resolved with the RDP's own per-slot numbering. The old
  3-bit-per-slot scheme turned SHADE/PRIM/ENV alpha, constant 1, COMBINED and TEXEL1 alpha into 0, so translucent
  parts (car windows) and alpha-driven HUD sprites vanished, and cycle 1 was dropped. Cycle 1's TEXEL0/TEXEL1 are the
  next tile's texels (swapped relative to cycle 0), COMBINED in cycle 1 is cycle 0's result. Not modelled: CENTER,
  SCALE, K4, K5, noise (read as 0), LOD (distance approximation, as before).
- **Blend**: framebuffer blending is only enabled for the XLU blender form (CLR_IN * A_IN + CLR_MEM * (1 - A_IN), read
  from the final blender cycle). Opaque modes no longer take combiner alpha as translucency. Alpha cutouts
  (CVG_X_ALPHA or G_AC_THRESHOLD) discard below 0.3.
- G_TEXRECT now samples the tile named in the command (it used the last G_TEXTURE tile).
- TLUT type IA16 (G_SETOTHERMODE_H bits 14-15 = 3) is decoded as intensity + alpha and is part of the texture cache key.
- Test encoders now emit real hardware values (constant 0 is 31/15/7, not 0).

## Third round: RT64 extended GBI and tile shifts

- Opcode 0x64 is RT64's extended GBI (24-bit id, 1-3 words). Its continuation words are raw data, so `gfx_run_ex` consumes them instead of running them as F3DEX2 commands (this was what hid the HUD and menus). Ids 0x34-0x37 are the recomp's own additions (fog float, car reflection, sky fog, sky draw). Unknown ids are logged once as `unknown RT64 extended command`. Rect alignment and `gEXSetScissor` are ignored: the window is treated as the 4:3 area.
- `G_SETTILE` shiftS/shiftT are now applied to texture coordinates (`tile_shift_scale`): 1-10 divides by 2^n, 11-15 multiplies by 2^(16-n). Needed by the attract screen's scrolling logo background.

## Phase 5, first part: widescreen

The game area (`gfx_set_game_area`, set by `Fast3DContext::refresh_dimensions` every frame) follows the aspect ratio
option: Original 4:3, Manual 16:9, Expand the window (never narrower than 4:3), centered with black bars; the UI still
draws over the whole window. Inside it (`gfx_pc.c`, "widescreen"):

- A view whose viewport spans the screen (12 pixel overscan border allowed) fills the area and keeps its vertical
  field of view: its x is squeezed by 4:3 / aspect, so the clip planes show more scene to the sides. Other viewports
  (split screen, model views) keep the 4:3 scale centered in the area (`view_squeezed`).
- The RT64 extended GBI alignment is implemented: `G_EX_SETRECTALIGN` (rectangles), `G_EX_SETSCISSOR` and the origin of
  `G_EX_SETVIEWPORT`. An edge is placed at `origin / 0x400` of the area's width plus its coordinate (4:3 game pixels)
  times `height / 240`; `G_EX_ORIGIN_NONE` is the plain 4:3 mapping centered in the area. Rectangle offsets are
  added to the coordinates (the hud subtracts the origin's own position itself). Scissor ends at 0 or 320 go to the
  area's edges. The scissor is always clipped to the game area. Not done: the texture rectangle (0x02), viewport align
  (0x07) and scissor align (0x08) commands, which the game does not emit.
- A FILLRECT over the whole 4:3 screen (the frame clear that paints the sky color) fills the whole area.

## Phase 5, second part: choosing the renderer

The Graphics tab's Display section has a "Renderer (restart needed)" option (`renderer` in graphics.json: `RT64` or
`Fast3D`, shown only in builds with `-DRUSH2_FAST3D=ON`). `rush2::fast3d::requested()` decides once, before the window
is created: `RUSH2_RENDERER=fast3d` or `rt64` overrides it, otherwise it reads graphics.json directly (the config system
isn't loaded yet). With Fast3D chosen, `hide_rt64_only_options` (src/config.cpp) hides what only RT64 draws with:
resolution scale, downsampling, refresh rate, API, MSAA, anisotropy, distant textures, ambient occlusion, sky fog, the car
effects, hi-res fonts and the texture pack options. The option is new, so there is nothing to migrate; a missing key is
RT64. The game was not built or run for this change (the submodules are not checked out in the review tree), so the
config edit is untested outside this description.
