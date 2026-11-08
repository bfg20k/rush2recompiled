#include "fast3d_context.h"

#ifdef RUSH2_FAST3D

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "recompui/recompui.h"
#include "ui_gl_renderer.h"

extern "C" {
#include "gfx_pc.h"
#include "gfx_rendering_api.h"
#include "gfx_opengl.h"
}

namespace rush2::fast3d {

namespace {

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
        // Resolution scale, MSAA, aspect modes and the high-precision framebuffer have no Fast3D equivalent yet; the
        // image always fills the window.
        return true;
    }

    void enable_instant_present() override {}

    void send_dl(const OSTask* task) override {
        if (gl == nullptr) {
            return;
        }
        open_frame();
        gfx_run_task(task->t.data_ptr & 0x3FFFFFF);
    }

    void send_dummy_workload(uint32_t) override {
        // RT64 fills the color image here. Opening a frame clears the screen, which is the same result.
        if (gl == nullptr) {
            return;
        }
        open_frame();
    }

    void update_screen() override {
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
        if (w > 0 && h > 0 && ((uint32_t)w != width || (uint32_t)h != height)) {
            width = (uint32_t)w;
            height = (uint32_t)h;
            gfx_set_dimensions(width, height);
        }
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

bool requested() {
    const char* value = std::getenv("RUSH2_RENDERER");
    return value != nullptr && std::strcmp(value, "fast3d") == 0;
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
