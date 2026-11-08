// Fast3D renderer backend for the runtime (docs/fast3d_port_plan.md, phase 3). Implements the same seam RT64Context does
// (ultramodern::renderer::RendererContext) on top of lib/fast3d and its OpenGL 3.3 backend. Selected at run time with
// RUSH2_RENDERER=fast3d in a build configured with -DRUSH2_FAST3D=ON; RT64 stays the default.
#pragma once

#include <memory>

#include "ultramodern/renderer_context.hpp"

struct SDL_Window;

namespace rush2::fast3d {

// True when the environment asks for the Fast3D renderer (and this build includes it). Decides the window flags too:
// the window needs SDL_WINDOW_OPENGL instead of the Vulkan flag RT64 wants.
bool requested();

// `window` is the SDL window the game created (with SDL_WINDOW_OPENGL). The GL context is made on the calling thread,
// which must be the thread that later calls send_dl and update_screen.
std::unique_ptr<ultramodern::renderer::RendererContext> create_context(uint8_t* rdram, SDL_Window* window);

} // namespace rush2::fast3d
