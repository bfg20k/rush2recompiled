// RmlUi render interface on OpenGL, for the Fast3D renderer (docs/fast3d_port_plan.md, phase 4). RecompFrontend's own
// renderer (recompui/src/renderer/ui_renderer.cpp) is built on RT64's plume RHI, which doesn't exist without RT64.
// This one draws into the bound default framebuffer after the game's frame, in GLSL 330 core or GLSL 120 (OpenGL 2.1)
// to match the context the Fast3D renderer got. Like the RT64 one it is a legacy (RenderInterfaceCompatibility)
// interface, so RmlUi hands it unpremultiplied vertex colors and textures and it blends with SRC_ALPHA.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "RmlUi/Core/RenderInterfaceCompatibility.h"

namespace rush2::ui_gl {

using GetProc = void* (*)(const char* name);

class Renderer : public Rml::RenderInterfaceCompatibility {
public:
    Renderer();
    ~Renderer() override;

    // Needs the GL context current. `legacy` selects GLSL 120 and an optional VAO (an OpenGL 2.1 context).
    bool init(GetProc get_proc, bool legacy);
    // Deletes every GL object; needs the context current. Safe to call twice.
    void reset();

    // Frame bracket: begin() sets the state the UI draws with, end() leaves the context for the caller to restore.
    void begin(int width, int height);
    void end();

    // Images the UI names by string (RmlUi `src` attributes), queued from any thread and decoded when RmlUi first asks
    // for them on the render thread.
    void queue_image_from_bytes_file(const std::string& src, const std::vector<char>& bytes);
    void queue_image_from_bytes_rgba32(const std::string& src, const std::vector<char>& bytes, uint32_t width, uint32_t height);

    void RenderGeometry(Rml::Vertex* vertices, int num_vertices, int* indices, int num_indices, Rml::TextureHandle texture,
                        const Rml::Vector2f& translation) override;
    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(int x, int y, int width, int height) override;
    bool LoadTexture(Rml::TextureHandle& texture_handle, Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
    bool GenerateTexture(Rml::TextureHandle& texture_handle, const Rml::byte* source, const Rml::Vector2i& source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;
    void SetTransform(const Rml::Matrix4f* transform) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace rush2::ui_gl
