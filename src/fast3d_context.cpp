#include "fast3d_context.h"

#ifdef RUSH2_FAST3D

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include <atomic>
#include <unordered_map>

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"
#include "shared/rt64_f3d_defines.h"
#include "common/rt64_load_types.h"
#include "common/rt64_live_textures.h"
#include "common/rt64_tmem_hasher.h"

#include "json/json.hpp"
#include "live_replacements.h"
#include "librecomp/game.hpp"
#include "recompui/recompui.h"
#include "ultramodern/config.hpp"
#include "ui_gl_renderer.h"

extern "C" {
#include "gfx_pc.h"
#include "gfx_rendering_api.h"
#include "gfx_opengl.h"
}

namespace rush2::fast3d {

namespace {

// Live texture replacements (include/live_replacements.h): the Fast3D core calls these when it shows a texture. drawn()
// gives the texture the hash RT64 would (so the recomp's texture tools key their replacements the same way) and tells
// RT64::ActiveTextureObserver, like RT64 does for each draw; replace() hands over the image registered for the hash.
std::atomic<uint64_t> g_frame{1};
std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> g_texture_dims; // render thread only

// The size RT64 samples a tile at (see sample_size in src/asset_catalog_walk.cpp).
void sample_size(const RT64::LoadTile& tile, uint16_t& width, uint16_t& height) {
    const bool clamp_s = (tile.masks == 0) || (tile.cms & G_TX_CLAMP);
    const bool clamp_t = (tile.maskt == 0) || (tile.cmt & G_TX_CLAMP);
    const int32_t tile_width = clamp_s ? std::max((tile.lrs - tile.uls + 4) / 4, 1) : 0xFFFF;
    const int32_t tile_height = clamp_t ? std::max((tile.lrt - tile.ult + 4) / 4, 1) : 0xFFFF;
    const int32_t mask_width = (tile.masks > 0) ? int32_t(1 << tile.masks) : 0xFFFF;
    const int32_t mask_height = (tile.maskt > 0) ? int32_t(1 << tile.maskt) : 0xFFFF;
    width = uint16_t(std::min(tile_width, mask_width));
    height = uint16_t(std::min(tile_height, mask_height));
}

uint64_t hook_drawn(const GfxTexInfo* i, const uint8_t* tmem, const uint16_t* tlut) {
    RT64::LoadTile tile{};
    tile.fmt = i->fmt; tile.siz = i->siz; tile.palette = i->palette;
    tile.cms = i->cms; tile.cmt = i->cmt; tile.masks = i->masks; tile.maskt = i->maskt;
    tile.shifts = i->shifts; tile.shiftt = i->shiftt;
    tile.line = i->line; tile.tmem = i->tmem;
    tile.uls = i->uls; tile.ult = i->ult; tile.lrs = i->lrs; tile.lrt = i->lrt;
    uint16_t width = 0, height = 0;
    sample_size(tile, width, height);
    // RGBA32 keeps its texels unsplit in Fast3D's TMEM (not the hardware's two banks), and a tile with no line has
    // nothing loaded: neither can be hashed the way RT64 does.
    if ((tile.fmt == G_IM_FMT_RGBA && tile.siz == G_IM_SIZ_32b) || tile.line == 0 || width == 0 || height == 0 ||
        width > 1024 || height > 1024) {
        return 0;
    }
    uint8_t canonical[4096];
    std::memcpy(canonical, tmem, sizeof(canonical));
    if (tile.fmt == G_IM_FMT_CI) {
        // A TLUT load leaves each palette entry four times over in its 64-bit word at 0x800 + 8 * index.
        for (int e = 0; e < 256; e++) {
            for (int k = 0; k < 4; k++) {
                canonical[0x800 + e * 8 + k * 2] = uint8_t(tlut[e] >> 8);
                canonical[0x800 + e * 8 + k * 2 + 1] = uint8_t(tlut[e]);
            }
        }
    }
    if (RT64::TMEMHasher::requiresRawTMEM(tile, width, height, i->tlut_mode)) return 0;
    const uint64_t hash = RT64::TMEMHasher::hash(canonical, tile, width, height, i->tlut_mode, RT64::TMEMHasher::CurrentHashVersion);
    g_texture_dims[hash] = { width, height };
    if (RT64::TextureObserver* observer = RT64::ActiveTextureObserver.load()) {
        observer->textureDrawn(hash, canonical, tile, width, height, i->tlut_mode, i->world, g_frame.load(), 0);
    }
    return hash;
}

int hook_replace(uint64_t hash, uint32_t* serial, GfxUploadFn upload, void* ctx) {
    static std::vector<uint8_t> rgba;
    uint32_t dw = 0, dh = 0, w = 0, h = 0;
    auto d = g_texture_dims.find(hash);
    if (d != g_texture_dims.end()) { dw = d->second.first; dh = d->second.second; }
    const int r = rush2::live::find(hash, dw, dh, serial, rgba, w, h);
    if (r == 2) upload(rgba.data(), w, h, ctx);
    return r;
}

uint32_t hook_generation() { return rush2::live::generation(); }

const GfxTextureHooks g_texture_hooks = { hook_drawn, hook_replace, hook_generation };

// recompui::ExternalRenderer takes plain function pointers, so the UI renderer is reached through this.
ui_gl::Renderer* g_ui = nullptr;

class Fast3DContext final : public ultramodern::renderer::RendererContext {
public:
    Fast3DContext(uint8_t* rdram, SDL_Window* win) : window(win) {
        setup_result = ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;

        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

        // Prefer a 3.3 core context (GLSL 330). RUSH2_GL=21 skips it, to test the fallback: a plain 2.1 context with GLSL
        // 120, for GPUs and drivers from before 3.3.
        const char* force = std::getenv("RUSH2_GL");
        const bool skip_core = force != nullptr && std::strcmp(force, "21") == 0;
        if (!skip_core) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            gl = SDL_GL_CreateContext(window);
        }
        bool legacy = false;
        if (gl == nullptr) {
            if (!skip_core) {
                std::fprintf(stderr, "[fast3d] no OpenGL 3.3 core context (%s), trying 2.1\n", SDL_GetError());
            }
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
            gl = SDL_GL_CreateContext(window);
            legacy = true;
        }
        if (gl == nullptr) {
            std::fprintf(stderr, "[fast3d] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
            setup_result = ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
            return;
        }
        SDL_GL_MakeCurrent(window, gl);
        SDL_GL_SetSwapInterval(1);
        std::fprintf(stderr, "[fast3d] using an OpenGL %s context\n", legacy ? "2.1 (GLSL 120)" : "3.3 core (GLSL 330)");

        gfx_opengl_set_glsl_flavor(legacy ? GFX_GLSL_120 : GFX_GLSL_330_CORE);
        if (!gfx_opengl_load_functions([](const char* name) -> void* { return SDL_GL_GetProcAddress(name); })) {
            std::fprintf(stderr, "[fast3d] the OpenGL driver lacks required functions\n");
            SDL_GL_DeleteContext(gl);
            gl = nullptr;
            setup_result = ultramodern::renderer::SetupResult::GraphicsAPINotFound;
            return;
        }

        gfx_init_embedded(&gfx_opengl_api, rdram);
        rush2::live::use_fast3d(true);
        gfx_set_texture_hooks(&g_texture_hooks);
        refresh_dimensions();

        // The menus and launcher: RecompFrontend's UI normally runs from RT64's render hooks, so with another renderer
        // it is started here and drawn from update_screen().
        ui = std::make_unique<ui_gl::Renderer>();
        if (ui->init([](const char* name) -> void* { return SDL_GL_GetProcAddress(name); }, legacy)) {
            g_ui = ui.get();
            recompui::ExternalRenderer external{
                ui->GetAdaptedInterface(),
                [](int w, int h) { g_ui->begin(w, h); },
                []() { g_ui->end(); },
                []() { g_ui->reset(); },
                [](const std::string& src, const std::vector<char>& bytes) { g_ui->queue_image_from_bytes_file(src, bytes); },
                [](const std::string& src, const std::vector<char>& bytes, uint32_t w, uint32_t h) {
                    g_ui->queue_image_from_bytes_rgba32(src, bytes, w, h);
                },
            };
            recompui::init_external_renderer(external);
            ui_started = true;
        } else {
            std::fprintf(stderr, "[fast3d] the UI renderer failed to start; menus will not be drawn\n");
        }
        setup_result = ultramodern::renderer::SetupResult::Success;
    }

    ~Fast3DContext() override { shutdown(); }

    bool valid() override { return gl != nullptr; }

    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override {
        if (old_config == new_config) {
            return false;
        }
        if (new_config.wm_option != old_config.wm_option) {
            SDL_SetWindowFullscreen(window, new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen
                                                ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        }
        // Resolution scale, MSAA and the high-precision framebuffer have no Fast3D equivalent yet. The aspect ratio
        // option is read every frame (refresh_dimensions).
        return true;
    }

    void enable_instant_present() override {}

    void send_dl(const OSTask* task) override {
        if (gl == nullptr) {
            return;
        }
        open_frame();
        gfx_run_task(task->t.data_ptr & 0x1FFFFFFF);
    }

    void send_dummy_workload(uint32_t) override {
        // RT64 fills the color image here. Opening a frame clears the screen, which is the same result.
        if (gl == nullptr) {
            return;
        }
        open_frame();
    }

    void update_screen() override {
        g_frame++;
        if (gl == nullptr || !frame_open) {
            return;
        }
        gfx_end_frame();
        if (ui_started) {
            // The UI shares the GL context, so the backend's and the core's cached state is rebuilt after it.
            recompui::draw_external(int(width), int(height));
            gfx_opengl_restore_state();
            gfx_reapply_state();
        }
        SDL_GL_SwapWindow(window);
        frame_open = false;
    }

    void shutdown() override {
        if (gl != nullptr) {
            SDL_GL_MakeCurrent(window, gl);
            if (ui_started) {
                recompui::deinit_external_renderer(); // frees the UI's GL objects, so the context must still exist
                ui_started = false;
            }
            g_ui = nullptr;
            ui.reset();
            SDL_GL_MakeCurrent(window, nullptr);
            SDL_GL_DeleteContext(gl);
            gl = nullptr;
        }
    }

    uint32_t get_display_framerate() const override {
        SDL_DisplayMode mode;
        int display = window != nullptr ? SDL_GetWindowDisplayIndex(window) : 0;
        if (display >= 0 && SDL_GetCurrentDisplayMode(display, &mode) == 0 && mode.refresh_rate > 0) {
            return (uint32_t)mode.refresh_rate;
        }
        return 60;
    }

    float get_resolution_scale() const override { return 1.0f; }

private:
    void refresh_dimensions() {
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(window, &w, &h);
        if (w > 0 && h > 0) {
            width = (uint32_t)w;
            height = (uint32_t)h;
        }
        if (width == 0 || height == 0) {
            return;
        }
        // The game's image follows the aspect ratio option like the RT64 renderer's: Original is 4:3, Manual 16:9, and
        // Expand fills the window (never narrower than 4:3). It is centered in the window with black bars around it.
        float aspect = 4.0f / 3.0f;
        switch (ultramodern::renderer::get_graphics_config().ar_option) {
            case ultramodern::renderer::AspectRatio::Manual:
                aspect = 16.0f / 9.0f;
                break;
            case ultramodern::renderer::AspectRatio::Expand:
                aspect = std::max(aspect, (float)width / (float)height);
                break;
            default:
                break;
        }
        uint32_t area_w = width, area_h = height;
        if ((float)width / (float)height > aspect) {
            area_w = (uint32_t)std::lround((float)height * aspect);
        } else {
            area_h = (uint32_t)std::lround((float)width / aspect);
        }
        area_w = std::clamp<uint32_t>(area_w, 1, width);
        area_h = std::clamp<uint32_t>(area_h, 1, height);
        gfx_set_game_area((int)((width - area_w) / 2), (int)((height - area_h) / 2), area_w, area_h);
    }

    void open_frame() {
        if (!frame_open) {
            SDL_GL_MakeCurrent(window, gl);
            refresh_dimensions();
            gfx_begin_frame();
            frame_open = true;
        }
    }

    SDL_Window* window = nullptr;
    SDL_GLContext gl = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool frame_open = false;
    bool ui_started = false;
    std::unique_ptr<ui_gl::Renderer> ui;
};

} // namespace

// The Renderer option (src/config.cpp) as graphics.json holds it, read once before the window is created. A missing
// file, key or an unknown value is RT64.
static bool saved_choice_is_fast3d() {
    std::ifstream file(recomp::get_config_path() / "graphics.json");
    if (!file.good()) {
        return false;
    }
    nlohmann::json j = nlohmann::json::parse(file, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("renderer")) {
        return false;
    }
    const nlohmann::json& v = j["renderer"];
    if (v.is_string()) {
        return v.get<std::string>() == "Fast3D";
    }
    return v.is_number_integer() && v.get<int>() == 1;
}

bool requested() {
    // RUSH2_RENDERER=fast3d or rt64 overrides the saved option (the tools and tests set it). Decided once: the window
    // flags and the render context must agree.
    static const bool fast3d = []() {
        const char* value = std::getenv("RUSH2_RENDERER");
        if (value != nullptr && value[0] != '\0') {
            return std::strcmp(value, "fast3d") == 0;
        }
        return saved_choice_is_fast3d();
    }();
    return fast3d;
}

std::unique_ptr<ultramodern::renderer::RendererContext> create_context(uint8_t* rdram, SDL_Window* window) {
    return std::make_unique<Fast3DContext>(rdram, window);
}

} // namespace rush2::fast3d

#else // !RUSH2_FAST3D

namespace rush2::fast3d {
bool requested() { return false; }
std::unique_ptr<ultramodern::renderer::RendererContext> create_context(uint8_t*, SDL_Window*) { return nullptr; }
} // namespace rush2::fast3d

#endif
