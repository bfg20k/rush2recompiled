#include "ui_gl_renderer.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#include "stb_image.h"

namespace rush2::ui_gl {

namespace {

#ifdef _WIN32
#define UIGL_APIENTRY __stdcall
#else
#define UIGL_APIENTRY
#endif

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

constexpr GLenum GL_FALSE_ = 0, GL_TRUE_ = 1, GL_FLOAT_ = 0x1406, GL_UNSIGNED_BYTE_ = 0x1401, GL_UNSIGNED_INT_ = 0x1405,
                 GL_TRIANGLES_ = 4, GL_ARRAY_BUFFER_ = 0x8892, GL_ELEMENT_ARRAY_BUFFER_ = 0x8893, GL_STREAM_DRAW_ = 0x88E0,
                 GL_DEPTH_TEST_ = 0x0B71, GL_BLEND_ = 0x0BE2, GL_SCISSOR_TEST_ = 0x0C11, GL_CULL_FACE_ = 0x0B44,
                 GL_POLYGON_OFFSET_FILL_ = 0x8037, GL_SRC_ALPHA_ = 0x0302, GL_ONE_MINUS_SRC_ALPHA_ = 0x0303, GL_ONE_ = 1,
                 GL_TEXTURE0_ = 0x84C0, GL_TEXTURE_2D_ = 0x0DE1, GL_TEXTURE_MIN_FILTER_ = 0x2801,
                 GL_TEXTURE_MAG_FILTER_ = 0x2800, GL_TEXTURE_WRAP_S_ = 0x2802, GL_TEXTURE_WRAP_T_ = 0x2803,
                 GL_LINEAR_ = 0x2601, GL_CLAMP_TO_EDGE_ = 0x812F, GL_RGBA_ = 0x1908, GL_RGBA8_ = 0x8058,
                 GL_UNPACK_ALIGNMENT_ = 0x0CF5, GL_VERTEX_SHADER_ = 0x8B31, GL_FRAGMENT_SHADER_ = 0x8B30,
                 GL_COMPILE_STATUS_ = 0x8B81, GL_LINK_STATUS_ = 0x8B82;

// X(return type, name, parameter list)
#define UIGL_FUNCTIONS(X)                                                                                              \
    X(void, glGenBuffers, (GLsizei, GLuint*))                                                                          \
    X(void, glDeleteBuffers, (GLsizei, const GLuint*))                                                                 \
    X(void, glBindBuffer, (GLenum, GLuint))                                                                            \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                                   \
    X(void, glEnableVertexAttribArray, (GLuint))                                                                       \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))                           \
    X(GLuint, glCreateShader, (GLenum))                                                                                \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                                     \
    X(void, glCompileShader, (GLuint))                                                                                 \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint*))                                                                   \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                                  \
    X(void, glDeleteShader, (GLuint))                                                                                  \
    X(GLuint, glCreateProgram, (void))                                                                                 \
    X(void, glAttachShader, (GLuint, GLuint))                                                                          \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*))                                                     \
    X(void, glLinkProgram, (GLuint))                                                                                   \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint*))                                                                  \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                                 \
    X(void, glDeleteProgram, (GLuint))                                                                                 \
    X(void, glUseProgram, (GLuint))                                                                                    \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar*))                                                            \
    X(void, glUniform1i, (GLint, GLint))                                                                               \
    X(void, glGenTextures, (GLsizei, GLuint*))                                                                         \
    X(void, glDeleteTextures, (GLsizei, const GLuint*))                                                                \
    X(void, glActiveTexture, (GLenum))                                                                                 \
    X(void, glBindTexture, (GLenum, GLuint))                                                                           \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))               \
    X(void, glTexParameteri, (GLenum, GLenum, GLint))                                                                  \
    X(void, glPixelStorei, (GLenum, GLint))                                                                            \
    X(void, glEnable, (GLenum))                                                                                        \
    X(void, glDisable, (GLenum))                                                                                       \
    X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                                     \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                                              \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei))                                                               \
    X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void*))

#define UIGL_OPTIONAL_FUNCTIONS(X)                                                                                     \
    X(void, glGenVertexArrays, (GLsizei, GLuint*))                                                                     \
    X(void, glBindVertexArray, (GLuint))                                                                               \
    X(void, glDeleteVertexArrays, (GLsizei, const GLuint*))

struct GL {
#define UIGL_MEMBER(ret, name, params) ret(UIGL_APIENTRY* name) params = nullptr;
    UIGL_FUNCTIONS(UIGL_MEMBER)
    UIGL_OPTIONAL_FUNCTIONS(UIGL_MEMBER)
#undef UIGL_MEMBER
};

// One 28-byte vertex: clip-space position (transformed on the CPU, so no matrix layout is assumed), color, texcoord.
struct GpuVertex {
    float x, y, z, w;
    uint8_t color[4];
    float u, v;
};
static_assert(sizeof(GpuVertex) == 28, "GpuVertex layout");
static_assert(sizeof(Rml::ColourbPremultiplied) == 4, "vertex color is 4 bytes");

const char* kVs330 =
    "#version 330 core\n"
    "layout(location = 0) in vec4 aPos;\n"
    "layout(location = 1) in vec4 aColor;\n"
    "layout(location = 2) in vec2 aUV;\n"
    "out vec4 vColor;\nout vec2 vUV;\n"
    "void main() { vColor = aColor; vUV = aUV; gl_Position = aPos; }\n";
const char* kFs330 =
    "#version 330 core\n"
    "in vec4 vColor;\nin vec2 vUV;\nout vec4 fragColor;\nuniform sampler2D uTex;\n"
    "void main() { fragColor = vColor * texture(uTex, vUV); }\n";
const char* kVs120 =
    "#version 120\n"
    "attribute vec4 aPos;\nattribute vec4 aColor;\nattribute vec2 aUV;\n"
    "varying vec4 vColor;\nvarying vec2 vUV;\n"
    "void main() { vColor = aColor; vUV = aUV; gl_Position = aPos; }\n";
const char* kFs120 =
    "#version 120\n"
    "varying vec4 vColor;\nvarying vec2 vUV;\nuniform sampler2D uTex;\n"
    "void main() { gl_FragColor = vColor * texture2D(uTex, vUV); }\n";

enum class ImageType { File, RGBA32 };

struct QueuedImage {
    ImageType type;
    uint32_t width = 0, height = 0; // RGBA32 only; files carry their own size
    std::vector<char> bytes;
};

} // namespace

struct Renderer::Impl {
    GL gl;
    bool ready = false;
    bool legacy = false;
    GLuint program = 0, vbo = 0, ebo = 0, vao = 0;
    int width = 1, height = 1;
    bool scissor_enabled = false;
    int scissor[4] = {0, 0, 0, 0};
    Rml::Matrix4f projection = Rml::Matrix4f::Identity();
    Rml::Matrix4f transform = Rml::Matrix4f::Identity();
    Rml::Matrix4f mvp = Rml::Matrix4f::Identity();
    std::unordered_map<Rml::TextureHandle, GLuint> textures;
    Rml::TextureHandle next_texture = 2; // 0 is a 1x1 white texture, 1 a transparent one
    std::mutex queue_mutex;
    std::unordered_map<std::string, QueuedImage> queued;
    std::vector<GpuVertex> scratch;

    GLuint make_texture(const uint8_t* rgba, int w, int h) {
        GLuint id = 0;
        gl.glGenTextures(1, &id);
        gl.glActiveTexture(GL_TEXTURE0_);
        gl.glBindTexture(GL_TEXTURE_2D_, id);
        gl.glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_LINEAR_);
        gl.glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_LINEAR_);
        gl.glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_S_, GL_CLAMP_TO_EDGE_);
        gl.glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
        gl.glPixelStorei(GL_UNPACK_ALIGNMENT_, 1);
        gl.glTexImage2D(GL_TEXTURE_2D_, 0, GL_RGBA8_, w, h, 0, GL_RGBA_, GL_UNSIGNED_BYTE_, rgba);
        return id;
    }

    GLuint compile(GLenum type, const char* src, const char* what) {
        GLuint sh = gl.glCreateShader(type);
        const GLchar* sources[1] = {src};
        gl.glShaderSource(sh, 1, sources, nullptr);
        gl.glCompileShader(sh);
        GLint ok = 0;
        gl.glGetShaderiv(sh, GL_COMPILE_STATUS_, &ok);
        if (!ok) {
            char log[1024] = {0};
            gl.glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
            std::fprintf(stderr, "[ui_gl] %s shader failed to compile:\n%s\n", what, log);
            gl.glDeleteShader(sh);
            return 0;
        }
        return sh;
    }
};

Renderer::Renderer() : impl(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default; // GL objects are released by reset(), which needs the context

bool Renderer::init(GetProc get_proc, bool legacy) {
    Impl& s = *impl;
    if (get_proc == nullptr) {
        return false;
    }
#define UIGL_LOAD(ret, name, params)                                                                                   \
    s.gl.name = reinterpret_cast<ret(UIGL_APIENTRY*) params>(get_proc(#name));                                         \
    if (s.gl.name == nullptr) {                                                                                        \
        std::fprintf(stderr, "[ui_gl] OpenGL function %s is not available\n", #name);                                  \
        return false;                                                                                                  \
    }
    UIGL_FUNCTIONS(UIGL_LOAD)
#undef UIGL_LOAD
#define UIGL_LOAD_OPT(ret, name, params) s.gl.name = reinterpret_cast<ret(UIGL_APIENTRY*) params>(get_proc(#name));
    UIGL_OPTIONAL_FUNCTIONS(UIGL_LOAD_OPT)
#undef UIGL_LOAD_OPT
    const bool have_vao = s.gl.glGenVertexArrays && s.gl.glBindVertexArray && s.gl.glDeleteVertexArrays;
    if (!legacy && !have_vao) {
        std::fprintf(stderr, "[ui_gl] a core profile context needs vertex arrays\n");
        return false;
    }
    s.legacy = legacy;

    GLuint vs = s.compile(GL_VERTEX_SHADER_, legacy ? kVs120 : kVs330, "vertex");
    GLuint fs = vs ? s.compile(GL_FRAGMENT_SHADER_, legacy ? kFs120 : kFs330, "fragment") : 0;
    if (!vs || !fs) {
        if (vs) s.gl.glDeleteShader(vs);
        return false;
    }
    s.program = s.gl.glCreateProgram();
    s.gl.glAttachShader(s.program, vs);
    s.gl.glAttachShader(s.program, fs);
    s.gl.glBindAttribLocation(s.program, 0, "aPos");
    s.gl.glBindAttribLocation(s.program, 1, "aColor");
    s.gl.glBindAttribLocation(s.program, 2, "aUV");
    s.gl.glLinkProgram(s.program);
    s.gl.glDeleteShader(vs);
    s.gl.glDeleteShader(fs);
    GLint ok = 0;
    s.gl.glGetProgramiv(s.program, GL_LINK_STATUS_, &ok);
    if (!ok) {
        char log[1024] = {0};
        s.gl.glGetProgramInfoLog(s.program, sizeof(log) - 1, nullptr, log);
        std::fprintf(stderr, "[ui_gl] program failed to link:\n%s\n", log);
        s.gl.glDeleteProgram(s.program);
        s.program = 0;
        return false;
    }
    s.gl.glUseProgram(s.program);
    s.gl.glUniform1i(s.gl.glGetUniformLocation(s.program, "uTex"), 0);

    if (have_vao) {
        s.gl.glGenVertexArrays(1, &s.vao);
        s.gl.glBindVertexArray(s.vao);
    }
    s.gl.glGenBuffers(1, &s.vbo);
    s.gl.glGenBuffers(1, &s.ebo);
    s.gl.glBindBuffer(GL_ARRAY_BUFFER_, s.vbo);
    s.gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_, s.ebo);
    if (have_vao) {
        // With a VAO the attribute layout is recorded once. Without one it is set at every draw.
        s.gl.glEnableVertexAttribArray(0);
        s.gl.glEnableVertexAttribArray(1);
        s.gl.glEnableVertexAttribArray(2);
        s.gl.glVertexAttribPointer(0, 4, GL_FLOAT_, GL_FALSE_, sizeof(GpuVertex), (const void*)offsetof(GpuVertex, x));
        s.gl.glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE_, GL_TRUE_, sizeof(GpuVertex), (const void*)offsetof(GpuVertex, color));
        s.gl.glVertexAttribPointer(2, 2, GL_FLOAT_, GL_FALSE_, sizeof(GpuVertex), (const void*)offsetof(GpuVertex, u));
        s.gl.glBindVertexArray(0);
    }

    const uint8_t white[4] = {255, 255, 255, 255};
    const uint8_t clear[4] = {0, 0, 0, 0};
    s.textures[0] = s.make_texture(white, 1, 1);
    s.textures[1] = s.make_texture(clear, 1, 1);
    s.ready = true;
    return true;
}

void Renderer::reset() {
    Impl& s = *impl;
    if (!s.ready) {
        return;
    }
    for (auto& [handle, id] : s.textures) {
        s.gl.glDeleteTextures(1, &id);
    }
    s.textures.clear();
    s.gl.glDeleteBuffers(1, &s.vbo);
    s.gl.glDeleteBuffers(1, &s.ebo);
    if (s.vao) s.gl.glDeleteVertexArrays(1, &s.vao);
    s.gl.glDeleteProgram(s.program);
    s.vao = s.vbo = s.ebo = s.program = 0;
    s.ready = false;
}

void Renderer::begin(int width, int height) {
    Impl& s = *impl;
    if (!s.ready) {
        return;
    }
    s.width = width > 0 ? width : 1;
    s.height = height > 0 ? height : 1;
    s.projection = Rml::Matrix4f::ProjectOrtho(0.0f, float(s.width), float(s.height), 0.0f, -10000.0f, 10000.0f);
    s.mvp = s.projection * s.transform;

    s.gl.glUseProgram(s.program);
    if (s.vao) {
        s.gl.glBindVertexArray(s.vao);
    }
    s.gl.glBindBuffer(GL_ARRAY_BUFFER_, s.vbo);
    s.gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_, s.ebo);
    s.gl.glActiveTexture(GL_TEXTURE0_);
    s.gl.glDisable(GL_DEPTH_TEST_);
    s.gl.glDisable(GL_CULL_FACE_);
    s.gl.glDisable(GL_POLYGON_OFFSET_FILL_);
    s.gl.glEnable(GL_BLEND_);
    // Unpremultiplied alpha (the legacy RmlUi interface), with the output alpha computed as a premultiplied blend would.
    s.gl.glBlendFuncSeparate(GL_SRC_ALPHA_, GL_ONE_MINUS_SRC_ALPHA_, GL_ONE_, GL_ONE_MINUS_SRC_ALPHA_);
    s.gl.glViewport(0, 0, s.width, s.height);
    s.gl.glDisable(GL_SCISSOR_TEST_);
    s.scissor_enabled = false;
}

void Renderer::end() {
    Impl& s = *impl;
    if (!s.ready) {
        return;
    }
    s.gl.glDisable(GL_SCISSOR_TEST_);
    if (s.vao) {
        s.gl.glBindVertexArray(0);
    }
}

void Renderer::SetTransform(const Rml::Matrix4f* transform) {
    Impl& s = *impl;
    s.transform = transform ? *transform : Rml::Matrix4f::Identity();
    s.mvp = s.projection * s.transform;
}

void Renderer::EnableScissorRegion(bool enable) {
    Impl& s = *impl;
    s.scissor_enabled = enable;
    if (!s.ready) {
        return;
    }
    if (enable) {
        s.gl.glEnable(GL_SCISSOR_TEST_);
    } else {
        s.gl.glDisable(GL_SCISSOR_TEST_);
    }
}

void Renderer::SetScissorRegion(int x, int y, int width, int height) {
    Impl& s = *impl;
    if (!s.ready) {
        return;
    }
    // RmlUi's origin is the top left; GL's is the bottom left.
    s.gl.glScissor(x, s.height - (y + height), width, height);
}

void Renderer::RenderGeometry(Rml::Vertex* vertices, int num_vertices, int* indices, int num_indices, Rml::TextureHandle texture,
                              const Rml::Vector2f& translation) {
    Impl& s = *impl;
    if (!s.ready || num_vertices <= 0 || num_indices <= 0) {
        return;
    }
    auto tex = s.textures.find(texture);
    if (tex == s.textures.end()) {
        tex = s.textures.find(1); // an unknown handle draws nothing visible rather than someone else's texture
    }

    s.scratch.resize(size_t(num_vertices));
    for (int i = 0; i < num_vertices; i++) {
        const Rml::Vector4f clip = s.mvp * Rml::Vector4f(vertices[i].position.x + translation.x, vertices[i].position.y + translation.y, 0.0f, 1.0f);
        GpuVertex& out = s.scratch[size_t(i)];
        out.x = clip.x;
        out.y = clip.y;
        out.z = clip.z;
        out.w = clip.w;
        std::memcpy(out.color, &vertices[i].colour, 4);
        out.u = vertices[i].tex_coord.x;
        out.v = vertices[i].tex_coord.y;
    }

    s.gl.glBindBuffer(GL_ARRAY_BUFFER_, s.vbo);
    s.gl.glBufferData(GL_ARRAY_BUFFER_, GLsizeiptr(sizeof(GpuVertex) * size_t(num_vertices)), s.scratch.data(), GL_STREAM_DRAW_);
    s.gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_, s.ebo);
    s.gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER_, GLsizeiptr(sizeof(int) * size_t(num_indices)), indices, GL_STREAM_DRAW_);
    if (!s.vao) {
        s.gl.glEnableVertexAttribArray(0);
        s.gl.glEnableVertexAttribArray(1);
        s.gl.glEnableVertexAttribArray(2);
        s.gl.glVertexAttribPointer(0, 4, GL_FLOAT_, GL_FALSE_, sizeof(GpuVertex), (const void*)offsetof(GpuVertex, x));
        s.gl.glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE_, GL_TRUE_, sizeof(GpuVertex), (const void*)offsetof(GpuVertex, color));
        s.gl.glVertexAttribPointer(2, 2, GL_FLOAT_, GL_FALSE_, sizeof(GpuVertex), (const void*)offsetof(GpuVertex, u));
    }
    s.gl.glActiveTexture(GL_TEXTURE0_);
    s.gl.glBindTexture(GL_TEXTURE_2D_, tex->second);
    s.gl.glDrawElements(GL_TRIANGLES_, num_indices, GL_UNSIGNED_INT_, nullptr);
}

void Renderer::queue_image_from_bytes_file(const std::string& src, const std::vector<char>& bytes) {
    std::lock_guard lock{impl->queue_mutex};
    QueuedImage img;
    img.type = ImageType::File;
    img.bytes = bytes;
    impl->queued[src] = std::move(img);
}

void Renderer::queue_image_from_bytes_rgba32(const std::string& src, const std::vector<char>& bytes, uint32_t width, uint32_t height) {
    std::lock_guard lock{impl->queue_mutex};
    QueuedImage img;
    img.type = ImageType::RGBA32;
    img.width = width;
    img.height = height;
    img.bytes = bytes;
    impl->queued[src] = std::move(img);
}

bool Renderer::LoadTexture(Rml::TextureHandle& texture_handle, Rml::Vector2i& texture_dimensions, const Rml::String& source) {
    Impl& s = *impl;
    QueuedImage img;
    {
        std::lock_guard lock{s.queue_mutex};
        auto it = s.queued.find(source);
        if (it == s.queued.end()) {
            // Like the RT64 interface: an image nobody queued is a transparent pixel, not an error.
            texture_handle = 1;
            texture_dimensions = {1, 1};
            return true;
        }
        img = it->second;
    }

    const uint8_t* pixels = nullptr;
    int w = 0, h = 0;
    uint8_t* decoded = nullptr;
    if (img.type == ImageType::RGBA32) {
        if (img.width == 0 || img.height == 0 || img.bytes.size() < size_t(img.width) * img.height * 4) {
            return false;
        }
        pixels = reinterpret_cast<const uint8_t*>(img.bytes.data());
        w = int(img.width);
        h = int(img.height);
    } else {
        int channels = 0;
        decoded = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(img.bytes.data()), int(img.bytes.size()), &w, &h, &channels, 4);
        if (decoded == nullptr) {
            std::fprintf(stderr, "[ui_gl] could not decode image %s: %s\n", source.c_str(), stbi_failure_reason());
            return false;
        }
        pixels = decoded;
    }

    texture_handle = s.next_texture++;
    s.textures[texture_handle] = s.make_texture(pixels, w, h);
    texture_dimensions = {w, h};
    if (decoded) {
        stbi_image_free(decoded);
    }
    return true;
}

bool Renderer::GenerateTexture(Rml::TextureHandle& texture_handle, const Rml::byte* source, const Rml::Vector2i& source_dimensions) {
    Impl& s = *impl;
    if (source_dimensions.x <= 0 || source_dimensions.y <= 0) {
        texture_handle = 0;
        return true;
    }
    texture_handle = s.next_texture++;
    s.textures[texture_handle] = s.make_texture(source, source_dimensions.x, source_dimensions.y);
    return true;
}

void Renderer::ReleaseTexture(Rml::TextureHandle texture) {
    Impl& s = *impl;
    if (texture < 2) {
        return; // the two built-in pixels stay
    }
    auto it = s.textures.find(texture);
    if (it != s.textures.end()) {
        s.gl.glDeleteTextures(1, &it->second);
        s.textures.erase(it);
    }
}

} // namespace rush2::ui_gl
