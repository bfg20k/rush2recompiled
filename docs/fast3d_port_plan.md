# Replacing RT64 with Fast3D

Goal: render Rush 2 with a Fast3D-style renderer instead of RT64. This is a multi-phase port. Phase 1 (the interpreter
core) is done and tested offline; nothing in the game uses it yet, and RT64 is still the renderer that runs.

## Status

| Phase | What | State |
| --- | --- | --- |
| 1 | `lib/fast3d`: RDRAM-aware F3DEX2 interpreter, tested against a null backend | Done, `tests/fast3d/test_core.cpp` passes |
| 2 | OpenGL 3.3 core rendering backend (`lib/fast3d/gfx_opengl.c`) | Done and tested against a fake GL and a real GLSL compiler; never run on a real GPU |
| 3 | `Fast3DContext : ultramodern::renderer::RendererContext`, window and GL context, VI/present, config mapping | Not started |
| 4 | UI: replace the RT64-based RmlUi renderer | Not started |
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

## Things that make this bigger than the renderer

- **The UI draws through RT64.** `recompui/src/renderer/ui_renderer.cpp` (727 lines) is an RmlUi render interface built on
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
