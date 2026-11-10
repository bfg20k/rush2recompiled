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
