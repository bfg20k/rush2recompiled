# Fast3D debugging

Frame dumps let a frame from the game be replayed offline (Mesa llvmpipe is enough), so renderer bugs can be diagnosed without the game or a GPU.

## Capturing a frame (in the game)
- `RUSH2_F3D_DUMP_DIR=<dir>` output directory (default: current directory).
- `RUSH2_F3D_DUMP_FRAMES=120,600` dumps those frames (counted from 1 at each `gfx_begin_frame`).
- Or, while the game runs on the screen of interest, create an empty file `<dir>/f3d_dump_now`; the next frame (checked every 20 frames) is dumped and the file is deleted.
- `RUSH2_F3D_DUMP_MB=512` RDRAM range scanned (default 512; only lower it if the game crashes while dumping).
- Output: `f3d_frame_<n>.bin` (non-zero RDRAM pages per task; tens of MB, zip it) and `f3d_frame_<n>.txt` (every command, plus a `=> DRAW` / `=> RECT` line per draw with combiner, other-mode, geometry mode and tile 0).
- stderr: `[fast3d] frame N: tasks, cmds, draws, tris, rects, unknown ops, extended cmds` for dumped frames and every 300th frame; `[fast3d] GL error 0x... at a draw` for GL errors after draws.

## Replaying
`cmake -S lib/fast3d -B tmp/f3d-build && cmake --build tmp/f3d-build` builds `f3d_replay` (needs SDL2 dev files).
`xvfb-run -a tmp/f3d-build/f3d_replay f3d_frame_120.bin out.ppm [glsl120|glsl330]` renders the dump (PPM; open with PIL).
Replay starts from default RDP state, so state only set in an earlier frame is missing (the game sets all state per display list).

## File format
See the comment above `dbg` in `lib/fast3d/gfx_pc.c`.

## Bisecting a replay by draw call

`RUSH2_F3D_DRAW_RANGE=a,b f3d_replay frame.bin out.ppm glsl120` draws only flushes a..b-1 of the frame (flush numbers count
every batch sent to the GL backend, in order; the `.txt` trace's `=> DRAW` lines are the same batches, but rect batches
are listed too). Render single flushes (`n,n+1`) and a contact sheet to find which draw makes a wrong pixel, then read that
draw's commands in the trace. This found the texture-window bug below.

## Lessons from dumps

- A tile's `SETTILESIZE` window origin (uls, ult) is subtracted from the texture coordinates and LOADBLOCK/LOADTILE store
  from the tile's TMEM origin, so a texture always reads from TMEM row 0 of its tile. The radar map loads three 96x42
  strips of a 96x96 image and sizes the tile at ult = 54, 12, 0; reading TMEM row 54 gave noise (and garbled tracks).
- Transparent RGBA5551 texels keep no color (bilinear fringes otherwise).
- Rect corners use vertex slots 64..67: the triangle bounds check must allow them.
- Viewports and scissors are signed: the track select's 3D preview has a viewport that starts above the screen (y = -14),
  which wrapped to 65522 as unsigned and hid the whole preview (the "black track select").
- `RUSH2_F3D_FLUSH_VERTS=1` prints each batch's first clip-space vertex plus the viewport and scissor in effect, for telling
  "culled / clipped" from "drawn off-screen".
- TMEM keeps the odd lines of a texture with the 32-bit halves of each 64-bit word swapped (address ^ 4). LOADBLOCK finds
  the line by accumulating dxt, so a load with dxt 0 never swaps; the game ships those textures pre-swapped and the sampler
  swaps odd rows back. Reading odd rows unswapped produced alternate-line garbage on the DANGER sign and billboards that
  only some loads show (dxt 0 ones). Tests that load with `g_loadblock` use dxt 0x800 so they stay unswapped.
- A dump's page list is now built once (the game thread writes RDRAM during the dump); old dumps may end with one stray
  record, which the replay ignores.
- `FILLRECT` only uses the fill color in fill/copy cycle mode. In 1- and 2-cycle mode it runs the combiner and blender
  with no texture and no shade (the car select's color swatches are FILLRECTs colored by PRIMITIVE).

## Transparent texels in opaque draws (the traffic cones)

The RDP ignores the RGBA5551 alpha bit unless the blender or alpha compare uses it, so a texture can be "transparent"
and still be drawn in its stored color. The cones are CI4 with palette entry 0 = `f802` (alpha 0) and every texel index
0: black cones if the decoder zeroes transparent texels. `bleed_transparent_texels` (gfx_pc.c) gives a transparent texel
the average of its opaque neighbors (no fringe, no spots) and keeps the stored color when it has none.
`RUSH2_F3D_FLUSH_VERTS=1` also logs each texture import (format, size, palette, TLUT head, first TMEM row).

`bleed_transparent_texels` spreads the opaque texels' colors outward ring by ring (8 passes) into the transparent ones,
not just one texel: the speedometer digits take their alpha from a second texture (the cross-fade, combiner
`(T1.a - T0.a) * ENV.a + T0.a`) and color from the first, so a transparent texel of the first whose stored mask color
survived showed as a red rectangle wherever the second is opaque.

## Rectangle sampling at tile edges (HUD glyphs and big sprites)

A texture rectangle inside its tile's window is sampled with clamp (never wrap, which bled the opposite edge as a line
around digits and sprites); sides that stop short of the window's edge (atlas glyphs) are pulled in half a texel when
upscaled. Copy-cycle rectangles may read one texel past the window. (The thin lines once seen in the car-select bar were the half-texel shift below, not art.)
the piece textures (152x13 CI8) have an opaque dim last-but-one row and a transparent last row, so they show with
nearest filtering too. `RUSH2_F3D_DUMP_TEX=<flush>` (replay tool) writes that flush's decoded texture to
`tmp/user1/tex.ppm` (`TEXA=1` writes the alpha channel instead).

HUD Filtering option (Graphics > Display, Fast3D only, hot-swappable, default Smooth): Sharp samples 2D texture
rectangles nearest (`gfx_set_hud_filtering`, `rect_point` in gfx_pc.c). The replay tool takes `RUSH2_F3D_HUD_SHARP=1`.

### Rectangles: no half-texel shift, backwards ranges (Midway logo, legal text)
- Triangles add 0.5 texel to bilinear coordinates; texture rectangles must not (`in_rect`): with it every 1:1 pixel
  blended two texels, shifting and blurring sprites and bleeding neighbors (ticks under atlas glyphs, seam lines).
- A rectangle with negative dtdy/dsdx (the Midway logo strips have dtdy=-1024) starts at the texel of its first pixel
  and runs backwards, so the range is moved one texel (`pixel k shows floor(s0 - k)`); without it each strip's rows
  came from one texel off and showed as dark/bright lines at every strip. `RUSH2_F3D_RECTUV=1` prints each rect's
  uls/ult/dsdx/dtdy in the replay tool. Test frame: the user's frame 220 (legal screen).

### Speedometer/score digit boxes: tiles that magnify their coordinates
The big digits are a 2-cycle crossfade whose TEXEL1 is tile 1, the same CI8 data viewed as CI4 with `shifts = 15`
(s' = 2s) and a 32 texel mask over a 16 texel size window. The RDP reads up to the mask, so the CI4 view is 32 texels
wide; limiting it to the window repeated half a row and the mask alpha filled the digit cells with translucent boxes.
`tile_dims` now widens a wrap tile with a magnifying S shift to its mask. Test frame: user frame 2400 (flush 40-42).
`RUSH2_F3D_DUMP_SLOT=0|1` picks which texture slot `RUSH2_F3D_DUMP_TEX` dumps.

## Live texture replacements (mod car textures, derived and upscaled textures)

RT64 swaps textures at run time (`RT64::addLiveReplacement`, `rush2::derive`, `texture_upscale.cpp`): mod cars draw tiny
stand-in textures (CI8 in a Rush 2 palette) and the recomp supplies the mod's full-size image. Fast3D has no such layer, so
before this bridge mod cars looked low resolution under it.

- `include/live_replacements.h`/`src/live_replacements.cpp` (`rush2::live`): every `addLiveReplacement` call site goes
  through it. Under RT64 it forwards; under Fast3D (`use_fast3d(true)`, set by `Fast3DContext`) it keeps decoded RGBA
  images keyed by RT64 texture hash, with a generation counter and per-entry serial.
- `lib/fast3d/gfx_pc.h` `GfxTextureHooks` (`drawn`, `replace`, `generation`), installed by `src/fast3d_context.cpp`.
  `drawn` builds the canonical TMEM (palette words from `tlut` at 0x800 + 8 * index, four copies of each entry), hashes it with
  `RT64::TMEMHasher` and calls `RT64::ActiveTextureObserver->textureDrawn`, so the upscaler/derive code sees the same
  hashes it sees under RT64. RGBA32 and tiles with no line are not hashed (0 = never replaced).
- `import_texture` calls `drawn` once per cached texture (again the first time it is drawn in 3D), then `replace`. The
  texture cache node remembers the hash and the serial shown; when the generation changes a node re-checks, so a
  replacement the derive worker finishes later swaps in, and a removed one decodes the game's texture again.
- A replaced texture gets the same half-texel shift as the game's own (`(s + 0.5) / width`, the middle of each game texel's
  block of the image: RT64's "center" shift). Skipping it (the first version did) put every replacement half a game texel
  off: hairlines where a wrapping texture meets its far edge, shifted signs. At most two replacement uploads happen per
  frame so a burst spreads over a few frames.
- Strip regions (`ReplacementRegion`) are cut from the source image at lookup time using the sampled size recorded by `drawn`.
- Offline check: `RUSH2_F3D_TEST_REPLACE=1 tools/f3d_replay frame.bin out.ppm` replaces every 3D texture with a checker
  (only two per frame because of the upload budget). The real replacements exist only in game, so the final check is
  SELECT CAR with a mod car (Magnum): its paint should be sharp.
- Not verified offline: the hashes against RT64's (the RT64 sources are not in this tree); if replacements never show,
  compare a `drawn` hash with the hash RT64 logs for the same texture.

### Generated texture packs (custom tracks, Dreamcast pack)

Custom track textures (`track_mod.cpp` publish_textures) and the Dreamcast pack (`rush2049_dc_pack.cpp`) reach RT64 as
generated packs (`set_generated_texture_pack`: a folder with an `rt64.json` of RT64 hash -> DDS). Under Fast3D the same
call sites use `rush2::live::set_pack(id, folder)` instead: it parses `rt64.json`, and the first time a texture with a pack
hash is drawn a worker thread reads and decodes the image; the generation then changes and the texture swaps in (so a
track's textures sharpen a moment after they first appear, never stalling the frame). Same hash caveat as above.

## Frame timing log

Every 300 frames the core logs `[fast3d] timing, last N frames, per frame: display lists X ms (draw submits Y ms for F
flushes, texture imports Z ms for I imports, M misses, U replacement uploads)`: the averages over those frames. Display
lists is the whole interpretation of the game's list on the render thread (draw submits and texture imports are inside it,
so the rest is the interpreter itself). A high import time with many misses means the 512-entry texture cache is
thrashing (it empties itself when full), and each miss also pays the texture hook (`drawn`) under the replacement bridge.
The hitch profiler (RUSH2_HITCH_LOG) is Windows only; on Linux this line and `perf record -g` are the tools.

The timing line has a second line breaking the display list time down: vertex loads, triangles (draw submits and texture
imports are inside it) and texture rects. Texture imports on a cache hit cost the hash of the texture's bytes
(`gfx_texture_key`), now eight bytes a step. The texture cache holds 4096 textures; at 512 a 2049 track emptied it every frame.

## Coverage from alpha (car sheen)

A blender ending in `A_MEM` is normally opaque (AA opaque modes), but with `ALPHA_CVG_SEL` set and `FORCE_BL` and
`CVG_X_ALPHA` clear the RDP takes the pixel's coverage from the combiner's alpha, so a texel with alpha below 1 only
partly covers the pixel and blends with what is under it. Rush 2's car sheen layers (CI4 32x32 textures, combine
`001FFE4F020A2D12`, othermode low word `C8112230`: the window glass and the "fake specular" streaks over the body, drawn
after the body) use exactly that. Fast3D treated them as opaque, which painted their dark texels over the car as black
blotches on the windows and lost the highlights; `gfx_pc.c` now alpha-blends that combination (`use_alpha`). Seen in
`f3d_frame_1220` (Compact on Select Car, flushes 43 and 61-68 are the sheen layers).

## BRANCH_Z: the game's level-of-detail switch

Cars (and probably other objects) pick a detail level with F3DEX2 `BRANCH_Z` (opcode 0x04), preceded by `RDPHALF_1` (the
target DL). `gSPBranchLessZraw(dl, vtx, zval)`: if vertex `vtx`'s screen depth is at or below `zval` (16.16, in the
viewport's 0..1023 range: `ndc z * 511 + 511`) the DL jumps (no return) to `dl`; otherwise the following commands run.
A cascade of two (near threshold first, then a farther one) gives three levels: near, medium, and the fall-through far
level. Fast3D skipped the command, so it always drew the farthest, lowest detail level (small textures: the rear banner
of the Exotic was a 32x4 smear instead of the 64x20 "ALL-STICK" decal). Implemented in `gfx_pc.c`;
`RUSH2_F3D_BRANCHZ_DBG=1` prints each branch with the screen depth and threshold. Seen in `f3d_frame_3720` (Exotic on
Select Car: depth 921.5, thresholds 852.7 and 938.0, so the medium level). The screen depth formula is from the
thresholds' scale and has not been checked against hardware beyond that frame.
