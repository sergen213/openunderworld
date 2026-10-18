// OpenGL 4.6 core backend (DSA), loaded through libepoxy.
#include "render/RenderCommon.hpp"

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

const char* kLineVs = R"(#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 0) uniform mat4 uViewProj;
out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = uViewProj * vec4(aPos, 1.0);
    gl_Position.z -= 1e-4 * gl_Position.w; // win depth ties against the surface the line lies on
})";

const char* kLineFs = R"(#version 460 core
in vec4 vColor;
out vec4 oColor;
void main() { oColor = vec4(vColor.rgb, 1.0); })";

struct LineVertex {
    float pos[3];
    uint32_t color;
};

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

struct GpuMesh {
    VertexArray vao;
    Buffer vbo, ebo;
    std::vector<scene::SubMesh> submeshes;
    glm::mat4 model;
    size_t source = 0, vertexCount = 0; // SceneData::meshes index, vertex count
};

struct Resources {
    Program sceneProgram, textProgram, lineProgram;
    Texture white, font;
    VertexArray textVao, lineVao;
    Buffer textVbo, lineVbo;
    const DebugLine* lineSource = nullptr; // cached upload of FrameOptions::debugLines
    size_t lineCount = 0;
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
        r.lineProgram = linkProgram(kLineVs, kLineFs, error);
        if (!r.lineProgram.get()) return false;
        r.lineVao = newVertexArray();
        attrib(r.lineVao.get(), 0, 3, GL_FLOAT, GL_FALSE, offsetof(LineVertex, pos));
        attrib(r.lineVao.get(), 1, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(LineVertex, color));

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
        for (size_t i = 0; i < scene.meshes.size(); ++i) uploadMesh(scene.meshes[i], i, warnings);
        return warnings;
    }

    bool updateVertices(size_t meshIndex, std::span<const scene::Vertex> vertices) override {
        for (const auto& g : res_->meshes)
            if (g.source == meshIndex) {
                if (g.vertexCount != vertices.size()) return false;
                glNamedBufferSubData(g.vbo.get(), 0, GLsizeiptr(vertices.size_bytes()), vertices.data());
                return true;
            }
        return false;
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
        drawLines(viewProj, options.debugLines);
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
        switch (t.format) {
        case scene::PixelFormat::RGBA8: break;
        case scene::PixelFormat::BGRA8: pixelFormat = GL_BGRA; break;
        case scene::PixelFormat::BC1: internal = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; break;
        case scene::PixelFormat::BC2: internal = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; break;
        case scene::PixelFormat::BC3: internal = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; break;
        }
        const bool compressed = blockBytes(t.format) != 0;
        if (compressed && !s3tc_) { warn("BCn unsupported (no GL_EXT_texture_compression_s3tc)"); return {}; }
        std::string problem;
        const GLsizei levels = GLsizei(usableMips(t, problem));
        if (!problem.empty()) warn(problem);
        if (levels == 0) return {};
        const uint32_t w0 = t.mips[0].width, h0 = t.mips[0].height;

        Texture tex = newTexture();
        GLuint id = tex.get();
        glTextureStorage2D(id, levels, internal, GLsizei(w0), GLsizei(h0));
        for (GLsizei i = 0; i < levels; ++i) {
            const auto& m = t.mips[size_t(i)];
            if (compressed)
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

    void uploadMesh(const scene::MeshData& m, size_t source, std::vector<std::string>& warnings) {
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
        g.source = source;
        g.vertexCount = m.vertices.size();
        g.vao = newVertexArray();
        g.vbo = newBuffer();
        g.ebo = newBuffer();
        glNamedBufferStorage(g.vbo.get(), GLsizeiptr(m.vertices.size() * sizeof(scene::Vertex)), m.vertices.data(),
                             GL_DYNAMIC_STORAGE_BIT); // updateVertices (CPU skinning)
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

    void drawLines(const glm::mat4& viewProj, std::span<const DebugLine> lines) {
        if (lines.empty()) return;
        Resources& r = *res_;
        if (lines.data() != r.lineSource || lines.size() != r.lineCount) {
            std::vector<LineVertex> v;
            v.reserve(2 * lines.size());
            for (const auto& l : lines) {
                v.push_back({{l.a[0], l.a[1], l.a[2]}, l.color});
                v.push_back({{l.b[0], l.b[1], l.b[2]}, l.color});
            }
            r.lineVbo = newBuffer(); // immutable storage: new buffer per change
            glNamedBufferStorage(r.lineVbo.get(), GLsizeiptr(v.size() * sizeof(LineVertex)), v.data(), 0);
            glVertexArrayVertexBuffer(r.lineVao.get(), 0, r.lineVbo.get(), 0, sizeof(LineVertex));
            r.lineSource = lines.data();
            r.lineCount = lines.size();
        }
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glUseProgram(r.lineProgram.get());
        glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(viewProj));
        glBindVertexArray(r.lineVao.get());
        glDrawArrays(GL_LINES, 0, GLsizei(2 * lines.size()));
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
    }

    void drawOverlay(Viewport vp, std::span<const std::string> lines) {
        if (lines.empty()) return;
        std::vector<TextVertex> v = buildOverlayVertices(vp, lines);
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
