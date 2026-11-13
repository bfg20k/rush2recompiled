// Live texture replacements: images the recomp supplies at run time for textures the game draws (car paints, mod car
// textures, derived and upscaled textures; src/texture_upscale.cpp, src/texture_derive.cpp). Under RT64 they go straight
// to RT64's texture cache (RT64::addLiveReplacement). Under Fast3D, which has no such layer, they are kept here as RGBA
// images and the Fast3D core looks them up by RT64 texture hash (src/fast3d_context.cpp installs the hooks), so both
// renderers show the same replacements. Callable from any thread.
#pragma once

#include <cstdint>
#include <vector>

#include "common/rt64_live_textures.h"

namespace rush2::live {

// Fast3D is the renderer: replacements are kept here instead of being given to RT64. Set before any is added.
void use_fast3d(bool on);
bool fast3d();

// The same as RT64::addLiveReplacement, hasLiveReplacementContent, removeLiveReplacement and clearLiveReplacements.
bool add(uint64_t hash, uint64_t content, std::vector<uint8_t> file, RT64::ReplacementRegion region = {}, bool centered = false);
bool has_content(uint64_t content);
void remove(uint64_t hash);
void clear();

// Fast3D side. generation() changes whenever a replacement is added or removed. find() gives the image for `hash`: for a
// region (a strip of a bigger image) the strip's part of it, cut for a texture of width x height texels. Returns 0 if
// there is none, 1 if the one with `*serial` is current, else 2 with the image in rgba (RGBA8, w x h) and its serial.
uint32_t generation();
int find(uint64_t hash, uint32_t width, uint32_t height, uint32_t* serial, std::vector<uint8_t>& rgba, uint32_t& w,
         uint32_t& h);

} // namespace rush2::live
