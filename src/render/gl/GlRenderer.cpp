// OpenGL 4.6 core backend (DSA), loaded through libepoxy.
#include "render/Renderer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <unordered_map>
#include <utility>

#include <SDL3/SDL.h>
#include <epoxy/gl.h>
#include <glm/gtc/type_ptr.hpp>

namespace ou::render {
namespace {

void delTexture(GLuint id) { glDeleteTextures(1, &id); }
void delBuffer(GLuint id) { glDeleteBuffers(1, &id); }
void delVertexArray(GLuint id) { glDeleteVertexArrays(1, &id); }
void delProgram(GLuint id) { glDeleteProgram(id); }

template <void (*Delete)(GLuint)>
class GlHandle {
public:
    GlHandle() = default;
    explicit GlHandle(GLuint id) : id_(id) {}
    GlHandle(GlHandle&& o) noexcept : id_(std::exchange(o.id_, 0)) {}
    GlHandle& operator=(GlHandle&& o) noexcept {
        if (this != &o) {
            reset();
            id_ = std::exchange(o.id_, 0);
        }
        return *this;
    }
    GlHandle(const GlHandle&) = delete;
    GlHandle& operator=(const GlHandle&) = delete;
    ~GlHandle() { reset(); }
    void reset() {
        if (id_) Delete(id_);
        id_ = 0;
    }
    GLuint get() const { return id_; }

private:
    GLuint id_ = 0;
};
using Texture = GlHandle<delTexture>;
using Buffer = GlHandle<delBuffer>;
using VertexArray = GlHandle<delVertexArray>;
using Program = GlHandle<delProgram>;

Texture newTexture() { GLuint id = 0; glCreateTextures(GL_TEXTURE_2D, 1, &id); return Texture(id); }
Buffer newBuffer() { GLuint id = 0; glCreateBuffers(1, &id); return Buffer(id); }
VertexArray newVertexArray() { GLuint id = 0; glCreateVertexArrays(1, &id); return VertexArray(id); }

const char* kSceneVs = R"(#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
layout(location = 0) uniform mat4 uViewProj;
layout(location = 1) uniform mat4 uModel;
out vec3 vNormal; out vec2 vUv; out vec4 vColor;
void main() {
    vNormal = mat3(uModel) * aNormal; // ponytail: no inverse-transpose; fine without non-uniform scale
    vUv = aUv; vColor = aColor;
    gl_Position = uViewProj * uModel * vec4(aPos, 1.0);
})";

const char* kSceneFs = R"(#version 460 core
in vec3 vNormal; in vec2 vUv; in vec4 vColor;
layout(location = 2) uniform int uLighting;
layout(binding = 0) uniform sampler2D uTex;
out vec4 oColor;
void main() {
    vec4 c = texture(uTex, vUv) * vColor;
    float len = length(vNormal);
    if (uLighting != 0 && len > 1e-6) {
        float ndl = max(dot(vNormal / len, normalize(vec3(-0.4, -0.5, 0.75))), 0.0);
        c.rgb *= 0.45 + 0.55 * ndl; // fixed key light from the default camera side
    }
    oColor = vec4(c.rgb, 1.0); // alpha ignored: cdc alpha channels are often masks, not opacity [INFERENCE]
})";

const char* kTextVs = R"(#version 460 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
layout(location = 0) uniform vec2 uScreen;
out vec2 vUv; out vec4 vColor;
void main() {
    vUv = aUv; vColor = aColor;
    gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0);
})";

const char* kTextFs = R"(#version 460 core
in vec2 vUv; in vec4 vColor;
layout(binding = 0) uniform sampler2D uFont;
out vec4 oColor;
void main() {
    float a = vUv.x < 0.0 ? 1.0 : texture(uFont, vUv).r; // negative uv = solid background quad
    oColor = vec4(vColor.rgb, vColor.a * a);
})";

GLuint compileShader(GLenum type, const char* src, std::string& error) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        error = std::string("shader compile failed: ") + log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

Program linkProgram(const char* vs, const char* fs, std::string& error) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vs, error);
    if (!v) return {};
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs, error);
    if (!f) { glDeleteShader(v); return {}; }
    Program p(glCreateProgram());
    glAttachShader(p.get(), v);
    glAttachShader(p.get(), f);
    glLinkProgram(p.get());
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p.get(), GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(p.get(), sizeof log, nullptr, log);
        error = std::string("program link failed: ") + log;
        return {};
    }
    return p;
}

void GLAPIENTRY onGlDebug(GLenum, GLenum, GLuint id, GLenum severity, GLsizei, const GLchar* msg, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    std::fprintf(stderr, "[gl debug %u] %s\n", id, msg);
}

// 8x8 glyphs for ASCII 32..126 come from SDL3's built-in debug font (SDL_RenderDebugText),
// rasterised once into a 16x6-glyph R8 atlas through a software renderer.
constexpr int kGlyph = 8, kAtlasCols = 16, kAtlasRows = 6;
constexpr int kAtlasW = kGlyph * kAtlasCols, kAtlasH = kGlyph * kAtlasRows;

std::vector<uint8_t> buildFontAtlas() {
    std::vector<uint8_t> out(kAtlasW * kAtlasH, 0);
    SDL_Surface* surf = SDL_CreateSurface(kAtlasW, kAtlasH, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return out;
    if (SDL_Renderer* r = SDL_CreateSoftwareRenderer(surf)) {
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderClear(r);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        for (int c = 32; c < 127; ++c) {
            const char s[2] = {static_cast<char>(c), 0};
            int i = c - 32;
            SDL_RenderDebugText(r, float(i % kAtlasCols * kGlyph), float(i / kAtlasCols * kGlyph), s);
        }
        SDL_RenderPresent(r);
        const auto* px = static_cast<const uint8_t*>(surf->pixels);
        for (int y = 0; y < kAtlasH; ++y)
            for (int x = 0; x < kAtlasW; ++x) out[y * kAtlasW + x] = px[y * surf->pitch + x * 4];
        SDL_DestroyRenderer(r);
    }
    SDL_DestroySurface(surf);
    return out;
}

struct TextVertex {
    float x, y, u, v;
    uint32_t color;
};

struct GpuMesh {
    VertexArray vao;
    Buffer vbo, ebo;
    std::vector<scene::SubMesh> submeshes;
    glm::mat4 model;
};

struct Resources {
    Program sceneProgram, textProgram;
    Texture white, font;
    VertexArray textVao;
    Buffer textVbo;
    std::unordered_map<uint32_t, Texture> textures;
    std::vector<GpuMesh> meshes;
};

class GlRenderer final : public Renderer {
public:
    ~GlRenderer() override { shutdown(); }

    uint64_t windowFlags() const override { return SDL_WINDOW_OPENGL; }

    bool preWindowSetup(std::string& error) override {
        bool ok = SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4) &&
                  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6) &&
                  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE) &&
                  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24) &&
                  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
#ifndef NDEBUG
        ok = ok && SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
#endif
        if (!ok) error = SDL_GetError();
        return ok;
    }

    bool init(SDL_Window* window, std::string& error) override {
        window_ = window;
        context_ = SDL_GL_CreateContext(window);
        if (!context_) {
            error = std::string("SDL_GL_CreateContext: ") + SDL_GetError();
            return false;
        }
        if (epoxy_gl_version() < 45) {
            error = "OpenGL 4.5+ required (DSA), got " + std::to_string(epoxy_gl_version());
            return false;
        }
        s3tc_ = epoxy_has_gl_extension("GL_EXT_texture_compression_s3tc");
        bool aniso = epoxy_gl_version() >= 46 || epoxy_has_gl_extension("GL_ARB_texture_filter_anisotropic") ||
                     epoxy_has_gl_extension("GL_EXT_texture_filter_anisotropic");
        if (aniso) glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso_);

        GLint flags = 0;
        glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
        if (flags & GL_CONTEXT_FLAG_DEBUG_BIT) {
            glEnable(GL_DEBUG_OUTPUT);
            glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            glDebugMessageCallback(onGlDebug, nullptr);
            debug_ = true;
        }

        Resources& r = res_.emplace();
        r.sceneProgram = linkProgram(kSceneVs, kSceneFs, error);
        if (!r.sceneProgram.get()) return false;
        r.textProgram = linkProgram(kTextVs, kTextFs, error);
        if (!r.textProgram.get()) return false;

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);

        r.white = newTexture();
        const uint32_t white = 0xFFFFFFFF;
        glTextureStorage2D(r.white.get(), 1, GL_RGBA8, 1, 1);
        glTextureSubImage2D(r.white.get(), 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &white);

        r.font = newTexture();
        auto atlas = buildFontAtlas();
        glTextureStorage2D(r.font.get(), 1, GL_R8, kAtlasW, kAtlasH);
        glTextureSubImage2D(r.font.get(), 0, 0, 0, kAtlasW, kAtlasH, GL_RED, GL_UNSIGNED_BYTE, atlas.data());
        glTextureParameteri(r.font.get(), GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(r.font.get(), GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        r.textVao = newVertexArray();
        r.textVbo = newBuffer();
        GLuint vao = r.textVao.get();
        glVertexArrayVertexBuffer(vao, 0, r.textVbo.get(), 0, sizeof(TextVertex));
        attrib(vao, 0, 2, GL_FLOAT, GL_FALSE, offsetof(TextVertex, x));
        attrib(vao, 1, 2, GL_FLOAT, GL_FALSE, offsetof(TextVertex, u));
        attrib(vao, 2, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(TextVertex, color));
        return true;
    }

    std::vector<std::string> upload(const scene::SceneData& scene) override {
        std::vector<std::string> warnings;
        Resources& r = *res_;
        r.textures.clear();
        r.meshes.clear();
        for (const auto& t : scene.textures) {
            Texture tex = uploadTexture(t, warnings);
            if (tex.get()) r.textures[t.id] = std::move(tex);
        }
        for (const auto& m : scene.meshes) uploadMesh(m, warnings);
        return warnings;
    }

    void resize(Viewport) override {} // GL default framebuffer follows the window; viewport set per frame

    void drawFrame(const Camera& camera, Viewport vp, const FrameOptions& options) override {
        Resources& r = *res_;
        glViewport(0, 0, vp.width, vp.height);
        glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);

        float aspect = vp.height > 0 ? float(vp.width) / float(vp.height) : 1.0f;
        glm::mat4 viewProj = camera.projection(aspect) * camera.view();
        glUseProgram(r.sceneProgram.get());
        glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(viewProj));
        glUniform1i(2, options.lighting ? 1 : 0);
        if (options.cullBackFaces) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        for (const auto& m : r.meshes) {
            glUniformMatrix4fv(1, 1, GL_FALSE, glm::value_ptr(m.model));
            glBindVertexArray(m.vao.get());
            for (const auto& s : m.submeshes) {
                auto it = r.textures.find(s.textureId);
                glBindTextureUnit(0, it != r.textures.end() ? it->second.get() : r.white.get());
                glDrawElements(GL_TRIANGLES, GLsizei(s.indexCount), GL_UNSIGNED_INT,
                               reinterpret_cast<const void*>(uintptr_t(s.firstIndex) * sizeof(uint32_t)));
            }
        }
        drawOverlay(vp, options.overlay);
    }

    Image readBackbuffer(Viewport vp) override {
        Image img{vp.width, vp.height, std::vector<uint8_t>(size_t(vp.width) * vp.height * 3)};
        std::vector<uint8_t> raw(img.rgb.size());
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, vp.width, vp.height, GL_RGB, GL_UNSIGNED_BYTE, raw.data());
        size_t row = size_t(vp.width) * 3;
        for (int y = 0; y < vp.height; ++y) // GL origin is bottom-left
            std::copy_n(raw.data() + size_t(vp.height - 1 - y) * row, row, img.rgb.data() + size_t(y) * row);
        return img;
    }

    void present() override { SDL_GL_SwapWindow(window_); }

    bool setVsync(bool enabled) override { return SDL_GL_SetSwapInterval(enabled ? 1 : 0); }

    std::string description() const override {
        auto str = [](GLenum e) { auto* s = glGetString(e); return s ? reinterpret_cast<const char*>(s) : "?"; };
        return std::string("OpenGL ") + str(GL_VERSION) + " | " + str(GL_RENDERER) + " | s3tc " +
               (s3tc_ ? "yes" : "NO") + " | aniso " + std::to_string(int(maxAniso_)) + "x" +
               (debug_ ? " | KHR_debug" : "");
    }

    void shutdown() override {
        if (!context_) return;
        SDL_GL_MakeCurrent(window_, context_);
        res_.reset(); // GL objects must die while the context is current
        SDL_GL_DestroyContext(context_);
        context_ = nullptr;
    }

private:
    static void attrib(GLuint vao, GLuint index, GLint size, GLenum type, GLboolean norm, size_t offset) {
        glEnableVertexArrayAttrib(vao, index);
        glVertexArrayAttribFormat(vao, index, size, type, norm, GLuint(offset));
        glVertexArrayAttribBinding(vao, index, 0);
    }

    Texture uploadTexture(const scene::TextureData& t, std::vector<std::string>& warnings) {
        auto warn = [&](const std::string& w) {
            warnings.push_back("texture " + std::to_string(t.id) + " (" + t.origin + "): " + w);
        };
        GLenum internal = GL_RGBA8, pixelFormat = GL_RGBA;
        size_t blockBytes = 0; // 0 = uncompressed
        switch (t.format) {
        case scene::PixelFormat::RGBA8: break;
        case scene::PixelFormat::BGRA8: pixelFormat = GL_BGRA; break;
        case scene::PixelFormat::BC1: internal = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; blockBytes = 8; break;
        case scene::PixelFormat::BC2: internal = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; blockBytes = 16; break;
        case scene::PixelFormat::BC3: internal = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; blockBytes = 16; break;
        }
        if (blockBytes && !s3tc_) { warn("BCn unsupported (no GL_EXT_texture_compression_s3tc)"); return {}; }
        if (t.mips.empty() || t.mips[0].width == 0 || t.mips[0].height == 0) { warn("no mip 0"); return {}; }

        const uint32_t w0 = t.mips[0].width, h0 = t.mips[0].height;
        GLsizei levels = 0;
        for (const auto& m : t.mips) {
            uint32_t w = std::max(1u, w0 >> levels), h = std::max(1u, h0 >> levels);
            size_t expect = blockBytes ? size_t((w + 3) / 4) * ((h + 3) / 4) * blockBytes : size_t(w) * h * 4;
            if (m.width != w || m.height != h || m.data.size() != expect) break;
            ++levels;
        }
        if (levels == 0) { warn("mip 0 size does not match its dimensions"); return {}; }
        if (size_t(levels) < t.mips.size())
            warn("using " + std::to_string(levels) + " of " + std::to_string(t.mips.size()) + " mips (bad chain)");

        Texture tex = newTexture();
        GLuint id = tex.get();
        glTextureStorage2D(id, levels, internal, GLsizei(w0), GLsizei(h0));
        for (GLsizei i = 0; i < levels; ++i) {
            const auto& m = t.mips[size_t(i)];
            if (blockBytes)
                glCompressedTextureSubImage2D(id, i, 0, 0, GLsizei(m.width), GLsizei(m.height), internal,
                                              GLsizei(m.data.size()), m.data.data());
            else
                glTextureSubImage2D(id, i, 0, 0, GLsizei(m.width), GLsizei(m.height), pixelFormat, GL_UNSIGNED_BYTE,
                                    m.data.data());
        }
        glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_REPEAT);
        if (maxAniso_ > 1.0f) glTextureParameterf(id, GL_TEXTURE_MAX_ANISOTROPY, maxAniso_);
        return tex;
    }

    void uploadMesh(const scene::MeshData& m, std::vector<std::string>& warnings) {
        auto warn = [&](const std::string& w) { warnings.push_back("mesh (" + m.origin + "): " + w); };
        if (m.vertices.empty() || m.indices.empty()) { warn("empty"); return; }
        if (std::any_of(m.indices.begin(), m.indices.end(), [&](uint32_t i) { return i >= m.vertices.size(); })) {
            warn("index out of range, skipped");
            return;
        }
        GpuMesh g;
        for (const auto& s : m.submeshes) {
            if (uint64_t(s.firstIndex) + s.indexCount > m.indices.size()) warn("submesh range out of bounds, skipped");
            else g.submeshes.push_back(s);
        }
        if (m.submeshes.empty()) g.submeshes.push_back({0, uint32_t(m.indices.size()), 0});
        g.model = glm::make_mat4(m.transform);
        g.vao = newVertexArray();
        g.vbo = newBuffer();
        g.ebo = newBuffer();
        glNamedBufferStorage(g.vbo.get(), GLsizeiptr(m.vertices.size() * sizeof(scene::Vertex)), m.vertices.data(), 0);
        glNamedBufferStorage(g.ebo.get(), GLsizeiptr(m.indices.size() * sizeof(uint32_t)), m.indices.data(), 0);
        GLuint vao = g.vao.get();
        glVertexArrayVertexBuffer(vao, 0, g.vbo.get(), 0, sizeof(scene::Vertex));
        glVertexArrayElementBuffer(vao, g.ebo.get());
        attrib(vao, 0, 3, GL_FLOAT, GL_FALSE, offsetof(scene::Vertex, position));
        attrib(vao, 1, 3, GL_FLOAT, GL_FALSE, offsetof(scene::Vertex, normal));
        attrib(vao, 2, 2, GL_FLOAT, GL_FALSE, offsetof(scene::Vertex, uv));
        attrib(vao, 3, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(scene::Vertex, color));
        res_->meshes.push_back(std::move(g));
    }

    void drawOverlay(Viewport vp, std::span<const std::string> lines) {
        if (lines.empty()) return;
        const float scale = float(std::max(1, vp.height / 800)); // 2x glyphs on 1440p+ / 4K
        const float g = kGlyph * scale, lineH = g + 2 * scale, pad = 4 * scale;
        std::vector<TextVertex> v;
        auto quad = [&](float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c) {
            v.insert(v.end(), {{x0, y0, u0, v0, c}, {x1, y0, u1, v0, c}, {x1, y1, u1, v1, c},
                               {x0, y0, u0, v0, c}, {x1, y1, u1, v1, c}, {x0, y1, u0, v1, c}});
        };
        float y = pad;
        for (const auto& raw : lines) {
            bool hi = !raw.empty() && raw[0] == '!';
            std::string_view s = hi ? std::string_view(raw).substr(1) : std::string_view(raw);
            uint32_t color = hi ? 0xFF40C0FFu : 0xFFFFFFFFu; // 0xAABBGGRR: amber / white
            quad(pad - 2 * scale, y - scale, pad + s.size() * g + 2 * scale, y + g + scale, -1, -1, -1, -1, 0xA0000000u);
            float x = pad;
            for (char ch : s) {
                int i = (ch >= 32 && ch < 127) ? ch - 32 : '?' - 32;
                float u0 = float(i % kAtlasCols * kGlyph) / kAtlasW, v0 = float(i / kAtlasCols * kGlyph) / kAtlasH;
                quad(x, y, x + g, y + g, u0, v0, u0 + float(kGlyph) / kAtlasW, v0 + float(kGlyph) / kAtlasH, color);
                x += g;
            }
            y += lineH;
        }
        Resources& r = *res_;
        glNamedBufferData(r.textVbo.get(), GLsizeiptr(v.size() * sizeof(TextVertex)), v.data(), GL_STREAM_DRAW);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(r.textProgram.get());
        glUniform2f(0, float(vp.width), float(vp.height));
        glBindTextureUnit(0, r.font.get());
        glBindVertexArray(r.textVao.get());
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(v.size()));
    }

    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    std::optional<Resources> res_;
    bool s3tc_ = false, debug_ = false;
    float maxAniso_ = 1.0f;
};

} // namespace

std::unique_ptr<Renderer> createGlRenderer() { return std::make_unique<GlRenderer>(); }

} // namespace ou::render
