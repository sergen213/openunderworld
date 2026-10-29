// OpenGL 4.6 core backend (DSA), loaded through libepoxy.
#include "render/OriginalShading.hpp"
#include "render/RenderCommon.hpp"
#include "render/Water.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <unordered_map>
#include <unordered_set>
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
void delFramebuffer(GLuint id) { glDeleteFramebuffers(1, &id); }
void delRenderbuffer(GLuint id) { glDeleteRenderbuffers(1, &id); }

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
using Framebuffer = GlHandle<delFramebuffer>;
using Renderbuffer = GlHandle<delRenderbuffer>;

Texture newTexture() { GLuint id = 0; glCreateTextures(GL_TEXTURE_2D, 1, &id); return Texture(id); }
Buffer newBuffer() { GLuint id = 0; glCreateBuffers(1, &id); return Buffer(id); }
VertexArray newVertexArray() { GLuint id = 0; glCreateVertexArrays(1, &id); return VertexArray(id); }
Framebuffer newFramebuffer() { GLuint id = 0; glCreateFramebuffers(1, &id); return Framebuffer(id); }
Renderbuffer newRenderbuffer(GLsizei samples, GLenum format, int w, int h) {
    GLuint id = 0;
    glCreateRenderbuffers(1, &id);
    glNamedRenderbufferStorageMultisample(id, samples > 1 ? samples : 0, format, w, h);
    return Renderbuffer(id);
}

// Scene shading (kept in step with src/render/vk/shaders/scene.*): texture, alpha test against the
// material threshold, sun lambert with two-sided normals, sun shadow from the cascaded map (render/sun.glsl).
// Vertex colour is still multiplied (placeholder tints; cdc meshes leave it white).
const char* kSceneVs = R"(#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
layout(location = 0) uniform mat4 uViewProj;
layout(location = 1) uniform mat4 uModel;
out vec3 vNormal; out vec2 vUv; out vec4 vColor; out vec3 vWorld;
void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vNormal = mat3(uModel) * aNormal; // ponytail: no inverse-transpose; fine without non-uniform scale
    vUv = aUv; vColor = aColor;
    vWorld = world.xyz;
    gl_Position = uViewProj * world;
})";

// Fragment shader = kSceneFsHead + sunGlsl() + kSceneFsMain.
const char* kSceneFsHead = R"(#version 460 core
in vec3 vNormal; in vec2 vUv; in vec4 vColor; in vec3 vWorld;
layout(location = 2) uniform int uLighting;
layout(location = 3) uniform float uAlphaRef;
layout(binding = 0) uniform sampler2D uTex;
out vec4 oColor;
)";
const char* kSceneFsMain = R"(
void main() {
    vec4 c = texture(uTex, vUv) * vColor;
    if (c.a < uAlphaRef) discard;
    float len = length(vNormal);
    if (uLighting != 0 && len > 1e-6) {
        vec3 n = (gl_FrontFacing ? 1.0 : -1.0) * vNormal / len; // two-sided materials light both faces
        float ndl = max(dot(n, ouShadow.sun.xyz), 0.0);
        c.rgb *= 0.45 + 0.55 * ndl * ouSunVisibility(vWorld, n, ndl > 0.0, false);
    }
    oColor = c; // alpha matters for blended submeshes only
})";

// Shadow map: depth only, alpha-tested like the scene.
const char* kShadowVs = R"(#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUv;
layout(location = 0) uniform mat4 uViewProj;
layout(location = 1) uniform mat4 uModel;
out vec2 vUv;
void main() { vUv = aUv; gl_Position = uViewProj * uModel * vec4(aPos, 1.0); })";

const char* kShadowFs = R"(#version 460 core
in vec2 vUv;
layout(location = 3) uniform float uAlphaRef;
layout(binding = 0) uniform sampler2D uTex;
void main() { if (texture(uTex, vUv).a < uAlphaRef) discard; })";

// Final pass: resolved scene -> default framebuffer; optional ACES tonemap (HDR) and FXAA.
const char* kPostVs = R"(#version 460 core
out vec2 vUv;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); // fullscreen triangle
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";

const char* kPostFs = R"(#version 460 core
in vec2 vUv;
layout(location = 0) uniform int uFxaa;
layout(location = 1) uniform int uHdr;
layout(location = 2) uniform float uBrightness; // display gamma (1 = neutral), as the original's brightness option
layout(location = 3) uniform float uFade;       // FrameOptions::screenFade (1 = black)
layout(binding = 0) uniform sampler2D uScene;
out vec4 oColor;
vec3 fetch(vec2 uv) {
    vec3 c = textureLod(uScene, uv, 0.0).rgb;
    if (uHdr != 0) c = clamp(c * (2.51 * c + 0.03) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0); // ACES (Narkowicz)
    return c;
}
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 fxaa(vec2 uv) { // classic FXAA (Lottes), edge direction from 4 diagonal taps
    vec2 rcp = 1.0 / vec2(textureSize(uScene, 0));
    vec3 m = fetch(uv);
    float nw = luma(fetch(uv + vec2(-1.0, -1.0) * rcp)), ne = luma(fetch(uv + vec2(1.0, -1.0) * rcp));
    float sw = luma(fetch(uv + vec2(-1.0, 1.0) * rcp)), se = luma(fetch(uv + vec2(1.0, 1.0) * rcp)), lm = luma(m);
    float lo = min(lm, min(min(nw, ne), min(sw, se))), hi = max(lm, max(max(nw, ne), max(sw, se)));
    vec2 dir = vec2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    float reduce = max((nw + ne + sw + se) * (0.25 / 8.0), 1.0 / 128.0);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), -8.0, 8.0) * rcp;
    vec3 a = 0.5 * (fetch(uv + dir * (1.0 / 3.0 - 0.5)) + fetch(uv + dir * (2.0 / 3.0 - 0.5)));
    vec3 b = a * 0.5 + 0.25 * (fetch(uv - dir * 0.5) + fetch(uv + dir * 0.5));
    float lb = luma(b);
    return (lb < lo || lb > hi) ? a : b;
}
)";
// Underwater + atmosphere declarations/functions (render/Water.cpp) go between kPostFs and kPostFsMain.
const char* kPostFsMain = R"(
void main() {
    vec3 c = pow(uFxaa != 0 ? fxaa(vUv) : fetch(vUv), vec3(1.0 / uBrightness));
    oColor = vec4(ouUnderwater(ouAtmosphere(c, vUv), vUv) * (1.0 - uFade), 1.0);
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
layout(binding = 1) uniform sampler2D uImage0; // FrameOptions::uiImages
layout(binding = 2) uniform sampler2D uImage1;
out vec4 oColor;
void main() {
    if (vUv.x >= 4.0) { oColor = texture(uImage1, vUv - vec2(4.0, 0.0)) * vColor; return; }
    if (vUv.x >= 2.0) { oColor = texture(uImage0, vUv - vec2(2.0, 0.0)) * vColor; return; }
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
    // Original Fidelity: per submesh translated program (Resources::originals index, -1 = generic), material-block
    // offset in Resources::materialUbo and textures for samplers s0-s15 (0 = engine default).
    std::vector<int> program;
    std::vector<GLintptr> materialOffset;
    std::vector<std::array<GLuint, 16>> samplers;
    glm::mat4 model;
    glm::vec3 center{0.0f};             // world-space bounds centre (blended draw sorting; light sets with `radius`)
    float radius = 0.0f;                // world-space bounding-sphere radius (original::lightSet)
    bool water = false;                 // *watersurface* mesh (render/Water.hpp): Enhanced water draws it separately
    size_t source = 0, vertexCount = 0; // SceneData::meshes index, vertex count
};

// Offscreen scene target (MSAA renderbuffers + single-sample resolve texture), rebuilt on size or
// settings change. msaa == 1 renders straight into `color`.
struct Targets {
    int width = 0, height = 0, msaa = 0;
    bool hdr = false;
    Framebuffer scene, resolve;
    Renderbuffer msColor, depth;
    Texture color; // sampled by the post pass
    // Enhanced water: single-sample copies of the opaque scene (refraction, absorption, reflections,
    // underwater fog). Blitted after the opaque/blended pass, before the water surfaces draw.
    Texture opaqueColor, opaqueDepth;
    // original::screenSamplers per submesh: low 16 bits back buffer, high 16 depth buffer (drawn after the opaque copies).
    std::vector<uint32_t> screenSamplers;
    Framebuffer opaqueColorFbo, opaqueDepthFbo;
};

struct Resources {
    Program sceneProgram, textProgram, lineProgram, shadowProgram, postProgram;
    Program waterProgram, splashProgram; // Enhanced water (render/Water.cpp sources)
    Buffer waterUbo;                     // WaterBlock, UBO binding 4 (water, splash, post)
    Texture ripples;                     // R32F height field, game::WaterSim
    uint64_t rippleVersion = 0;
    int rippleSize = 0;
    VertexArray splashVao;
    Buffer splashVbo;
    Texture white, font, shadowMap, attenuation, blackCube;
    struct Streamed { Texture tex; uint64_t version = 0; int width = 0, height = 0; };
    std::array<Streamed, 2> uiImages; // FrameOptions::uiImages
    // Original Fidelity (docs/formats/SHADERS.md): translated programs, register-file UBOs (bindings 0 frame,
    // 1 material, 2 object; ShaderTranslate.hpp), objects rewritten per frame.
    std::vector<Program> originals;
    Buffer frameUbo, materialUbo, objectUbo;
    Buffer shadowUbo; // render/sun.glsl OuShadow (UBO binding 3); the shadow map is texture unit 16
    GLint uboAlign = 256;
    Framebuffer shadowFbo;
    VertexArray textVao, lineVao, emptyVao;
    Buffer textVbo, lineVbo;
    const DebugLine* lineSource = nullptr; // cached upload of FrameOptions::debugLines
    size_t lineCount = 0;
    std::unordered_map<uint32_t, Texture> textures;
    std::vector<GpuMesh> meshes;
    Bounds bounds;
    Targets targets;
    std::vector<DrawRef> order;
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
        const std::string sceneFs = std::string(kSceneFsHead) + sunGlsl() + kSceneFsMain;
        r.sceneProgram = linkProgram(kSceneVs, sceneFs.c_str(), error);
        if (!r.sceneProgram.get()) return false;
        r.textProgram = linkProgram(kTextVs, kTextFs, error);
        if (!r.textProgram.get()) return false;
        r.lineProgram = linkProgram(kLineVs, kLineFs, error);
        if (!r.lineProgram.get()) return false;
        r.shadowProgram = linkProgram(kShadowVs, kShadowFs, error);
        if (!r.shadowProgram.get()) return false;
        const std::string postFs = std::string(kPostFs) + sunGlsl() + underwaterPostSource(false) + kPostFsMain; // sun: shafts
        r.postProgram = linkProgram(kPostVs, postFs.c_str(), error);
        if (!r.postProgram.get()) return false;
        const std::string waterVs = waterVertexSource(false), waterFs = waterFragmentSource(false);
        r.waterProgram = linkProgram(waterVs.c_str(), waterFs.c_str(), error);
        if (!r.waterProgram.get()) return false;
        const std::string splashVs = splashVertexSource(false), splashFs = splashFragmentSource(false);
        r.splashProgram = linkProgram(splashVs.c_str(), splashFs.c_str(), error);
        if (!r.splashProgram.get()) return false;
        r.waterUbo = newBuffer();
        glNamedBufferStorage(r.waterUbo.get(), sizeof(WaterBlock), nullptr, GL_DYNAMIC_STORAGE_BIT);
        r.splashVao = newVertexArray();
        r.splashVbo = newBuffer();
        glVertexArrayVertexBuffer(r.splashVao.get(), 0, r.splashVbo.get(), 0, sizeof(SplashVertex));
        attrib(r.splashVao.get(), 0, 3, GL_FLOAT, GL_FALSE, offsetof(SplashVertex, pos));
        attrib(r.splashVao.get(), 1, 2, GL_FLOAT, GL_FALSE, offsetof(SplashVertex, uv));
        attrib(r.splashVao.get(), 2, 1, GL_FLOAT, GL_FALSE, offsetof(SplashVertex, alpha));
        r.emptyVao = newVertexArray();
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

        // Sun shadow atlas (kShadowCascades tiles side by side): always allocated so the shadow sampler is complete.
        r.shadowMap = newTexture();
        GLuint sm = r.shadowMap.get();
        glTextureStorage2D(sm, 1, GL_DEPTH_COMPONENT32F, kShadowCascades * kShadowMapSize, kShadowMapSize);
        glTextureParameteri(sm, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(sm, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(sm, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(sm, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(sm, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTextureParameteri(sm, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        r.shadowFbo = newFramebuffer();
        glNamedFramebufferTexture(r.shadowFbo.get(), GL_DEPTH_ATTACHMENT, sm, 0);
        glNamedFramebufferDrawBuffer(r.shadowFbo.get(), GL_NONE);

        const auto lut = original::attenuationLut();
        r.attenuation = newTexture();
        glTextureStorage2D(r.attenuation.get(), 1, GL_RGBA8, lut.width, lut.height);
        glTextureSubImage2D(r.attenuation.get(), 0, 0, 0, lut.width, lut.height, GL_RGBA, GL_UNSIGNED_BYTE, lut.rgba.data());
        glTextureParameteri(r.attenuation.get(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(r.attenuation.get(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(r.attenuation.get(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(r.attenuation.get(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &r.uboAlign);
        GLuint cube = 0;
        glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &cube);
        r.blackCube = Texture(cube);
        const uint32_t black[6] = {0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000};
        glTextureStorage2D(cube, 1, GL_RGBA8, 1, 1);
        glTextureSubImage3D(cube, 0, 0, 0, 0, 1, 1, 6, GL_RGBA, GL_UNSIGNED_BYTE, black);
        r.frameUbo = newBuffer();
        glNamedBufferStorage(r.frameUbo.get(), sizeof(sm3::FrameBlock), nullptr, GL_DYNAMIC_STORAGE_BIT);
        r.shadowUbo = newBuffer();
        glNamedBufferStorage(r.shadowUbo.get(), sizeof(SunShadowBlock), nullptr, GL_DYNAMIC_STORAGE_BIT);
        return true;
    }

    std::vector<std::string> upload(const scene::SceneData& scene) override {
        std::vector<std::string> warnings;
        Resources& r = *res_;
        r.textures.clear();
        r.meshes.clear();
        cubes_.clear();
        for (const auto& t : scene.textures) {
            if (r.textures.contains(t.id)) { // first id wins, as formats::loadTextures and the Vulkan backend
                warnings.push_back("texture " + std::to_string(t.id) + " (" + t.origin + "): duplicate texture id, first one wins");
                continue;
            }
            Texture tex = uploadTexture(t, warnings);
            if (tex.get()) r.textures[t.id] = std::move(tex);
        }
        environment_ = scene.environment;
        // AttenuationSampler light rows follow the scene's light falloff curves (SHADERS.md "Engine inputs").
        const auto lut = original::attenuationLut(environment_);
        glTextureSubImage2D(r.attenuation.get(), 0, 0, 0, lut.width, lut.height, GL_RGBA, GL_UNSIGNED_BYTE, lut.rgba.data());
        prepareOriginals(scene, warnings);
        for (size_t i = 0; i < scene.meshes.size(); ++i) uploadMesh(scene.meshes[i], i, warnings);
        finishOriginals();
        prepared_ = {};
        r.bounds = sceneBounds(scene);
        return warnings;
    }

    bool updateVertices(size_t meshIndex, std::span<const scene::Vertex> vertices) override {
        for (auto& g : res_->meshes)
            if (g.source == meshIndex) {
                if (g.vertexCount != vertices.size()) return false;
                glNamedBufferSubData(g.vbo.get(), 0, GLsizeiptr(vertices.size_bytes()), vertices.data());
                const glm::vec4 sphere = original::boundingSphere(vertices, g.model); // skinned: follows the pose
                g.center = glm::vec3(sphere), g.radius = sphere.w;
                return true;
            }
        return false;
    }

    void resize(Viewport) override {} // GL default framebuffer follows the window; viewport set per frame

    void drawFrame(const Camera& camera, Viewport vp, const FrameOptions& options) override {
        Resources& r = *res_;
        if (vp.width <= 0 || vp.height <= 0) return;
        for (size_t i = 0; i < options.dynamicLights.size() && i < environment_.lights.size(); ++i) environment_.lights[i] = options.dynamicLights[i];
        ensureTargets(vp, options);
        const float aspect = float(vp.width) / float(vp.height);
        const glm::mat4 viewProj = camera.projection(aspect) * camera.view();
        const bool enhancedWater = options.water == WaterQuality::Enhanced;
        WaterBlock wb = waterBlock(camera, vp, viewProj, environment_, options);
        if (!enhancedWater) wb.fogColor.w = 0.0f; // Original: no underwater post either
        glNamedBufferSubData(r.waterUbo.get(), 0, sizeof wb, &wb);
        glBindBufferBase(GL_UNIFORM_BUFFER, 4, r.waterUbo.get());
        drawOrder(r.meshes, camera.position, r.order);
        // No ray tracing here: a requested RT mode renders with the shadow map (as Vulkan without RT support).
        const bool shadows = options.shadows || options.rt != RayTracing::Off;
        const SunShadow sun = sunShadow(camera, vp, r.bounds, sunToward(environment_), false, shadows, 0, 0);
        if (shadows) drawShadowMap(sun);
        glNamedBufferSubData(r.shadowUbo.get(), 0, sizeof sun.block, &sun.block);
        glBindBufferBase(GL_UNIFORM_BUFFER, 3, r.shadowUbo.get());
        glBindTextureUnit(16, r.shadowMap.get());

        glBindFramebuffer(GL_FRAMEBUFFER, r.targets.scene.get());
        glViewport(0, 0, vp.width, vp.height);
        glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glUseProgram(r.sceneProgram.get());
        glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(viewProj));
        glUniform1i(2, options.lighting ? 1 : 0);
        glBlendColor(0.5f, 0.5f, 0.5f, 0.5f); // Blend::Average = D3DBLEND_BLENDFACTOR 0x80808080
        const bool originals = options.originalShaders && !r.originals.empty();
        if (originals) beginOriginals(camera, vp, options);
        uint32_t boundMesh = UINT32_MAX, boundObject = UINT32_MAX;
        int state = -1, program = -1; // -1 = generic scene program
        for (const DrawRef& d : r.order) {
            const GpuMesh& m = r.meshes[d.mesh];
            const scene::SubMesh& s = m.submeshes[d.sub];
            if (enhancedWater && m.water) continue; // drawn by drawWater with the opaque copies
            const int p = originals ? m.program[d.sub] : -1;
            if (p != program) {
                glUseProgram(p < 0 ? r.sceneProgram.get() : r.originals[size_t(p)].get());
                if (p < 0) boundMesh = UINT32_MAX; // re-send uModel to the generic program
                program = p;
            }
            if (d.mesh != boundMesh && p < 0) glUniformMatrix4fv(1, 1, GL_FALSE, glm::value_ptr(m.model));
            if (d.mesh != boundMesh) {
                glBindVertexArray(m.vao.get());
                boundMesh = d.mesh;
            }
            if (int(s.blend) != state) setBlend(state = int(s.blend));
            if (options.cullBackFaces && !s.doubleSided) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
            if (p >= 0) {
                if (d.mesh != boundObject) {
                    glBindBufferRange(GL_UNIFORM_BUFFER, 2, r.objectUbo.get(), GLintptr(d.mesh) * objectStride(), sizeof(sm3::ObjectBlock));
                    boundObject = d.mesh;
                }
                glBindBufferRange(GL_UNIFORM_BUFFER, 1, r.materialUbo.get(), m.materialOffset[d.sub], sizeof(sm3::MaterialBlock));
                glBindTextures(0, 16, m.samplers[d.sub].data());
                glDrawElements(GL_TRIANGLES, GLsizei(s.indexCount), GL_UNSIGNED_INT,
                               reinterpret_cast<const void*>(uintptr_t(s.firstIndex) * sizeof(uint32_t)));
                continue;
            }
            glUniform1f(3, s.alphaRef / 255.0f);
        deferred_.clear();
            drawSubmesh(s);
        }
        setBlend(int(scene::Blend::Opaque));
        if (enhancedWater) drawWater(camera, vp, options);
        else if (wb.haze.w > 0.5f || wb.weather.x > 0.5f) // atmosphere / weather read the scene depth in the post pass
            glBlitNamedFramebuffer(r.targets.scene.get(), r.targets.opaqueDepthFbo.get(), 0, 0, vp.width, vp.height, 0, 0,
                                   vp.width, vp.height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        drawLines(viewProj, options.debugLines);

        Targets& t = r.targets;
        if (t.msaa > 1)
            glBlitNamedFramebuffer(t.scene.get(), t.resolve.get(), 0, 0, vp.width, vp.height, 0, 0, vp.width, vp.height,
                                   GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, vp.width, vp.height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glUseProgram(r.postProgram.get());
                if (m.screenSamplers[d.sub]) { // reads the scene drawn so far: after the loop, from the copies
                    deferred_.push_back(d);
                    continue;
                }
        glUniform1i(0, options.fxaa ? 1 : 0);
        glUniform1i(1, options.hdr ? 1 : 0);
        glUniform1f(2, options.brightness);
        glBindTextureUnit(0, t.color.get());
        glBindTextureUnit(6, t.opaqueDepth.get()); // underwater post (unused unless submerged)
        glBindVertexArray(r.emptyVao.get());
        glDrawArrays(GL_TRIANGLES, 0, 3);
        drawOverlay(vp, options.overlay, options.ui, options.uiImages);
    }

    // (Re)creates the texture on a size change and uploads when the version changes; empty images keep the old one.
    static void upload(Resources::Streamed& s, const RgbaImage& img) {
        if (img.rgba.empty()) return;
        if (originals && !deferred_.empty()) drawScreenReaders(vp);
        if (s.width != img.width || s.height != img.height) {
            s.tex = newTexture();
            glTextureStorage2D(s.tex.get(), 1, GL_RGBA8, img.width, img.height);
            glTextureParameteri(s.tex.get(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(s.tex.get(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(s.tex.get(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(s.tex.get(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            s.width = img.width, s.height = img.height;
            s.version = 0;
        }
        if (s.version != img.version) {
            glTextureSubImage2D(s.tex.get(), 0, 0, 0, img.width, img.height, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
            s.version = img.version;
        }
    }

    Image readBackbuffer(Viewport vp) override {
        Image img{vp.width, vp.height, std::vector<uint8_t>(size_t(vp.width) * vp.height * 3)};
        std::vector<uint8_t> raw(img.rgb.size());
        glReadBuffer(GL_BACK);
        glUniform1f(3, std::clamp(options.screenFade, 0.0f, 1.0f));
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

    void ensureTargets(Viewport vp, const FrameOptions& o) {
        Targets& t = res_->targets;
        const int msaa = std::max(1, o.msaa);
        if (t.width == vp.width && t.height == vp.height && t.msaa == msaa && t.hdr == o.hdr) return;
        t = {};
        t.width = vp.width, t.height = vp.height, t.msaa = msaa, t.hdr = o.hdr;
        const GLenum format = o.hdr ? GL_RGBA16F : GL_RGBA8;
        t.color = newTexture();
        glTextureStorage2D(t.color.get(), 1, format, vp.width, vp.height);
        glTextureParameteri(t.color.get(), GL_TEXTURE_MIN_FILTER, GL_LINEAR); // FXAA taps between texels
        glTextureParameteri(t.color.get(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(t.color.get(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(t.color.get(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        t.depth = newRenderbuffer(msaa, GL_DEPTH_COMPONENT32F, vp.width, vp.height);
        t.scene = newFramebuffer();
        if (msaa > 1) {
            t.msColor = newRenderbuffer(msaa, format, vp.width, vp.height);
            glNamedFramebufferRenderbuffer(t.scene.get(), GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t.msColor.get());
            t.resolve = newFramebuffer();
            glNamedFramebufferTexture(t.resolve.get(), GL_COLOR_ATTACHMENT0, t.color.get(), 0);
        } else {
            glNamedFramebufferTexture(t.scene.get(), GL_COLOR_ATTACHMENT0, t.color.get(), 0);
        }
        glNamedFramebufferRenderbuffer(t.scene.get(), GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.depth.get());
        if (glCheckNamedFramebufferStatus(t.scene.get(), GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            std::fprintf(stderr, "[gl] scene framebuffer incomplete (msaa %d)\n", msaa);
        t.opaqueColor = newTexture();
        glTextureStorage2D(t.opaqueColor.get(), 1, format, vp.width, vp.height);
        glTextureParameteri(t.opaqueColor.get(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(t.opaqueColor.get(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(t.opaqueColor.get(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(t.opaqueColor.get(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        t.opaqueDepth = newTexture();
        glTextureStorage2D(t.opaqueDepth.get(), 1, GL_DEPTH_COMPONENT32F, vp.width, vp.height);
        glTextureParameteri(t.opaqueDepth.get(), GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(t.opaqueDepth.get(), GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(t.opaqueDepth.get(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(t.opaqueDepth.get(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        t.opaqueColorFbo = newFramebuffer();
        glNamedFramebufferTexture(t.opaqueColorFbo.get(), GL_COLOR_ATTACHMENT0, t.opaqueColor.get(), 0);
        t.opaqueDepthFbo = newFramebuffer();
        glNamedFramebufferTexture(t.opaqueDepthFbo.get(), GL_DEPTH_ATTACHMENT, t.opaqueDepth.get(), 0);
        glNamedFramebufferDrawBuffer(t.opaqueDepthFbo.get(), GL_NONE);
    }

    // Opaque: depth write, LESS. Blended (the original's translucent pass 0x5066A0): no depth
    // write, LEQUAL, blend per cdc mode.
    static void setBlend(int blend) {
        const auto b = scene::Blend(blend);
        const bool opaque = b == scene::Blend::Opaque;
        glDepthMask(opaque ? GL_TRUE : GL_FALSE);
        glDepthFunc(opaque ? GL_LESS : GL_LEQUAL);
        if (opaque) { glDisable(GL_BLEND); return; }
        glEnable(GL_BLEND);
        if (b == scene::Blend::Additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        else if (b == scene::Blend::Average) glBlendFunc(GL_CONSTANT_COLOR, GL_CONSTANT_COLOR);
        else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void drawSubmesh(const scene::SubMesh& s) {
        auto it = res_->textures.find(s.textureId);
        const bool usable = it != res_->textures.end() && !cubes_.contains(s.textureId);
        glBindTextureUnit(0, usable ? it->second.get() : res_->white.get());
        glDrawElements(GL_TRIANGLES, GLsizei(s.indexCount), GL_UNSIGNED_INT,
                       reinterpret_cast<const void*>(uintptr_t(s.firstIndex) * sizeof(uint32_t)));
    }

    // One atlas tile per cascade. Opaque and alpha-blended surfaces cast (blended ones alpha-tested at 50 %);
    // additive/average effects do not. Both faces, slope-scaled bias.
    // ponytail: every mesh is drawn into every cascade; cull by mesh bounds per cascade if the GL path gets CPU bound.
    void drawShadowMap(const SunShadow& sun) {
        Resources& r = *res_;
        glBindFramebuffer(GL_FRAMEBUFFER, r.shadowFbo.get());
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(2.0f, 2.0f);
        glUseProgram(r.shadowProgram.get());
        for (int c = 0; c < kShadowCascades; ++c) {
            glViewport(c * kShadowMapSize, 0, kShadowMapSize, kShadowMapSize);
            glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(sun.lightViewProj[c]));
            for (const auto& m : r.meshes) {
                glUniformMatrix4fv(1, 1, GL_FALSE, glm::value_ptr(m.model));
                glBindVertexArray(m.vao.get());
                for (const auto& s : m.submeshes) {
                    if (s.blend != scene::Blend::Opaque && s.blend != scene::Blend::Alpha) continue;
                    glUniform1f(3, (s.blend == scene::Blend::Alpha ? std::max<int>(s.alphaRef, 128) : s.alphaRef) / 255.0f);
                    drawSubmesh(s);
                }
            }
        }
        glDisable(GL_POLYGON_OFFSET_FILL);
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

        GLuint id = 0;
        glCreateTextures(t.cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, 1, &id);
        Texture tex(id);
        glTextureStorage2D(id, levels, internal, GLsizei(w0), GLsizei(h0));
        const GLsizei depth = t.cube ? 6 : 1; // cube faces = layers of a DSA cube map, in the scene's +X -X +Y -Y +Z -Z order
        for (GLsizei i = 0; i < levels; ++i) {
            const auto& m = t.mips[size_t(i)];
            if (t.cube && compressed)
                glCompressedTextureSubImage3D(id, i, 0, 0, 0, GLsizei(m.width), GLsizei(m.height), depth, internal,
                                              GLsizei(m.data.size()), m.data.data());
            else if (t.cube)
                glTextureSubImage3D(id, i, 0, 0, 0, GLsizei(m.width), GLsizei(m.height), depth, pixelFormat, GL_UNSIGNED_BYTE,
                                    m.data.data());
            else if (compressed)
                glCompressedTextureSubImage2D(id, i, 0, 0, GLsizei(m.width), GLsizei(m.height), internal,
                                              GLsizei(m.data.size()), m.data.data());
            else
                glTextureSubImage2D(id, i, 0, 0, GLsizei(m.width), GLsizei(m.height), pixelFormat, GL_UNSIGNED_BYTE,
                                    m.data.data());
        }
        if (t.cube) cubes_.insert(t.id);
        glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(id, GL_TEXTURE_WRAP_S, t.cube ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glTextureParameteri(id, GL_TEXTURE_WRAP_T, t.cube ? GL_CLAMP_TO_EDGE : GL_REPEAT);
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
        const glm::vec4 sphere = original::boundingSphere(m.vertices, g.model);
        g.center = glm::vec3(sphere), g.radius = sphere.w;
        g.water = isWaterSurface(m.origin);
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
        // Translated original vertex shaders (sm3::Attr locations).
        attrib(vao, sm3::kAttrColor0, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(scene::Vertex, color0));
        attrib(vao, sm3::kAttrTexcoord1, 2, GL_FLOAT, GL_FALSE, offsetof(scene::Vertex, uv1));
        attrib(vao, sm3::kAttrTangent, 3, GL_FLOAT, GL_FALSE, offsetof(scene::Vertex, tangent));
        attrib(vao, sm3::kAttrBinormal, 3, GL_FLOAT, GL_FALSE, offsetof(scene::Vertex, binormal));
        for (const auto& s : g.submeshes) addOriginal(g, s);
        res_->meshes.push_back(std::move(g));
    }

    // ---- Original Fidelity (translated TRU shaders, docs/formats/SHADERS.md) ----
    GLintptr objectStride() const {
        const GLintptr a = res_->uboAlign;
        return (GLintptr(sizeof(sm3::ObjectBlock)) + a - 1) / a * a;
    }

    void prepareOriginals(const scene::SceneData& scene, std::vector<std::string>& warnings) {
        Resources& r = *res_;
        r.originals.clear();
        materialBlocks_.clear();
        prepared_ = original::prepare(scene, false);
        for (auto& w : prepared_.warnings) warnings.push_back(std::move(w));
        for (const auto& p : prepared_.programs) {
            std::string error;
            const std::string ps = originalPixelSource(p.psGlsl.source, false, false); // + cascaded sun shadow
            r.originals.push_back(linkProgram(p.vsGlsl.source.c_str(), ps.c_str(), error));
            if (!r.originals.back().get())
                warnings.push_back("original program vs " + hex(p.vs) + " ps " + hex(p.ps) + ": " + error);
        }
    }

    static std::string hex(uint64_t v) {
        char b[24];
        std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
        return b;
    }

    // Per submesh: program, material block (appended to materialBlocks_) and sampler textures.
    void addOriginal(GpuMesh& g, const scene::SubMesh& s) {
        Resources& r = *res_;
        int p = prepared_.program(s.materialId);
        if (p >= 0 && !r.originals[size_t(p)].get()) p = -1;
        g.program.push_back(p);
        g.materialOffset.push_back(0);
        g.samplers.emplace_back();
        if (p < 0) return;
        const scene::Material& mat = *prepared_.materials.at(s.materialId);
        const auto stride = size_t(objectStrideFor(sizeof(sm3::MaterialBlock)));
        g.materialOffset.back() = GLintptr(materialBlocks_.size());
        materialBlocks_.resize(materialBlocks_.size() + stride);
        original::fillMaterial(*reinterpret_cast<sm3::MaterialBlock*>(&materialBlocks_[size_t(g.materialOffset.back())]), mat, s);
        const auto& prog = prepared_.programs[size_t(p)];
        for (int n = 0; n < 16; ++n) {
            if (!(prog.psGlsl.samplers >> n & 1)) continue;
            // Unbound cube samplers read black (D3D9 without a texture), others white; the type must match the sampler.
            const bool cube = prog.psGlsl.samplerType[n] == 3;
            GLuint tex = cube ? r.blackCube.get() : r.white.get();
            if (n == original::kAttenuationSampler && !cube) tex = r.attenuation.get();
            if (n == original::kSunCookieSampler && !cube && environment_.sunCookie)
                if (auto it = r.textures.find(environment_.sunCookie); it != r.textures.end() && !cubes_.contains(it->first))
                    tex = it->second.get();
            if (const uint32_t id = original::samplerTexture(mat, s.instanceTextures, n))
                if (auto it = r.textures.find(id); it != r.textures.end() && cubes_.contains(id) == cube) tex = it->second.get();
            g.samplers.back()[size_t(n)] = tex;
        }
    }
    GLintptr objectStrideFor(size_t bytes) const {
        const GLintptr a = res_->uboAlign;
        return (GLintptr(bytes) + a - 1) / a * a;
    }

    void finishOriginals() {
        Resources& r = *res_;
        r.materialUbo = {};
        r.objectUbo = {};
        g.screenSamplers.push_back(0);
        if (materialBlocks_.empty()) return;
        r.materialUbo = newBuffer();
        glNamedBufferStorage(r.materialUbo.get(), GLsizeiptr(materialBlocks_.size()), materialBlocks_.data(), 0);
        r.objectUbo = newBuffer();
        glNamedBufferStorage(r.objectUbo.get(), GLsizeiptr(r.meshes.size()) * objectStride(), nullptr, GL_DYNAMIC_STORAGE_BIT);
        materialBlocks_.clear();
        materialBlocks_.shrink_to_fit();
    }

    void beginOriginals(const Camera& camera, Viewport vp, const FrameOptions& o) {
        Resources& r = *res_;
        sm3::FrameBlock f;
        original::fillFrame(f, camera, vp, o.time, environment_);
        glNamedBufferSubData(r.frameUbo.get(), 0, sizeof f, &f);
        const glm::mat4 vpD3D = original::viewProjD3D(camera, float(vp.width) / float(vp.height));
        objectScratch_.assign(r.meshes.size() * size_t(objectStride()), 0);
        for (size_t i = 0; i < r.meshes.size(); ++i)
            original::fillObject(*reinterpret_cast<sm3::ObjectBlock*>(&objectScratch_[i * size_t(objectStride())]), vpD3D, r.meshes[i].model,
                                 original::lightSet(environment_, r.meshes[i].center, r.meshes[i].radius));
        if (r.objectUbo.get())
        g.screenSamplers.back() = original::screenSamplers(mat, s.instanceTextures, prog.psGlsl.readsDepth);
            glNamedBufferSubData(r.objectUbo.get(), 0, GLsizeiptr(objectScratch_.size()), objectScratch_.data());

    // Submeshes whose materials sample the back / depth buffer (original::screenSamplers; the sea): copy the scene drawn
    // so far into the single-sample textures, then draw them in draw order with those bound.
    void drawScreenReaders(Viewport vp) {
        Resources& r = *res_;
        Targets& t = r.targets;
        glBlitNamedFramebuffer(t.scene.get(), t.opaqueColorFbo.get(), 0, 0, vp.width, vp.height, 0, 0, vp.width, vp.height,
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBlitNamedFramebuffer(t.scene.get(), t.opaqueDepthFbo.get(), 0, 0, vp.width, vp.height, 0, 0, vp.width, vp.height,
                               GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        int state = -1;
        for (const DrawRef& d : deferred_) {
            const GpuMesh& m = r.meshes[d.mesh];
            const scene::SubMesh& s = m.submeshes[d.sub];
            glUseProgram(r.originals[size_t(m.program[d.sub])].get());
            glBindVertexArray(m.vao.get());
            if (int(s.blend) != state) setBlend(state = int(s.blend));
            glDisable(GL_CULL_FACE); // the sea is also seen from below
            glBindBufferRange(GL_UNIFORM_BUFFER, 2, r.objectUbo.get(), GLintptr(d.mesh) * objectStride(), sizeof(sm3::ObjectBlock));
            glBindBufferRange(GL_UNIFORM_BUFFER, 1, r.materialUbo.get(), m.materialOffset[d.sub], sizeof(sm3::MaterialBlock));
            auto tex = m.samplers[d.sub];
            for (int n = 0; n < 16; ++n) {
                if (m.screenSamplers[d.sub] >> n & 1) tex[size_t(n)] = t.opaqueColor.get();
                if (m.screenSamplers[d.sub] >> (16 + n) & 1) tex[size_t(n)] = t.opaqueDepth.get();
            }
            glBindTextures(0, 16, tex.data());
            glDrawElements(GL_TRIANGLES, GLsizei(s.indexCount), GL_UNSIGNED_INT,
                           reinterpret_cast<const void*>(uintptr_t(s.firstIndex) * sizeof(uint32_t)));
        }
    }

        glBindBufferBase(GL_UNIFORM_BUFFER, 0, r.frameUbo.get());
    }

    // Enhanced water: resolve the opaque scene into the copy textures, then draw the water surfaces
    // with refraction/reflection/ripples and the splash droplets on top.
    void drawWater(const Camera& camera, Viewport vp, const FrameOptions& options) {
        Resources& r = *res_;
        Targets& t = r.targets;
        const auto& rip = options.ripples;
        if (rip.cellSize > 0 && rip.size > 0 && !rip.heights.empty()) {
            if (!r.ripples.get() || r.rippleSize != rip.size) {
                r.ripples = newTexture();
                glTextureStorage2D(r.ripples.get(), 1, GL_R32F, rip.size, rip.size);
                glTextureParameteri(r.ripples.get(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTextureParameteri(r.ripples.get(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTextureParameteri(r.ripples.get(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTextureParameteri(r.ripples.get(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                r.rippleSize = rip.size;
                r.rippleVersion = 0;
            }
            if (r.rippleVersion != rip.version) {
                glTextureSubImage2D(r.ripples.get(), 0, 0, 0, rip.size, rip.size, GL_RED, GL_FLOAT, rip.heights.data());
                r.rippleVersion = rip.version;
            }
        }
        glBlitNamedFramebuffer(t.scene.get(), t.opaqueColorFbo.get(), 0, 0, vp.width, vp.height, 0, 0, vp.width, vp.height,
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBlitNamedFramebuffer(t.scene.get(), t.opaqueDepthFbo.get(), 0, 0, vp.width, vp.height, 0, 0, vp.width, vp.height,
                               GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, t.scene.get());
        glViewport(0, 0, vp.width, vp.height);
        glBindTextureUnit(5, t.opaqueColor.get());
        glBindTextureUnit(6, t.opaqueDepth.get());
        glBindTextureUnit(7, r.ripples.get() ? r.ripples.get() : r.white.get());
        glUseProgram(r.waterProgram.get());
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE); // the surface must also read from below
        for (const auto& m : r.meshes) {
            if (!m.water) continue;
            glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(m.model));
            glBindVertexArray(m.vao.get());
            for (const auto& s : m.submeshes)
                glDrawElements(GL_TRIANGLES, GLsizei(s.indexCount), GL_UNSIGNED_INT,
                               reinterpret_cast<const void*>(uintptr_t(s.firstIndex) * sizeof(uint32_t)));
        }
        if (!options.splashes.empty()) {
            const auto verts = splashVertices(camera, options.splashes);
            if (!verts.empty()) {
                glNamedBufferData(r.splashVbo.get(), GLsizeiptr(verts.size() * sizeof(SplashVertex)), verts.data(),
                                  GL_STREAM_DRAW);
                glUseProgram(r.splashProgram.get());
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glBindVertexArray(r.splashVao.get());
                glDrawArrays(GL_TRIANGLES, 0, GLsizei(verts.size()));
                glDisable(GL_BLEND);
            }
        }
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
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

    void drawOverlay(Viewport vp, std::span<const std::string> lines, std::span<const TextVertex> ui,
                     const std::array<RgbaImage, 2>& uiImages) {
        if (lines.empty() && ui.empty()) return;
        std::vector<TextVertex> v = buildOverlayVertices(vp, lines, ui);
        Resources& r = *res_;
        glNamedBufferData(r.textVbo.get(), GLsizeiptr(v.size() * sizeof(TextVertex)), v.data(), GL_STREAM_DRAW);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(r.textProgram.get());
        glUniform2f(0, float(vp.width), float(vp.height));
        glBindTextureUnit(0, r.font.get());
        for (size_t i = 0; i < r.uiImages.size(); ++i) {
            upload(r.uiImages[i], uiImages[i]);
            glBindTextureUnit(GLuint(1 + i), r.uiImages[i].tex.get() ? r.uiImages[i].tex.get() : r.white.get());
        }
        glBindVertexArray(r.textVao.get());
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(v.size()));
    }

    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    std::optional<Resources> res_;
    original::Prepared prepared_;         // during upload()
    scene::Environment environment_;
    std::unordered_set<uint32_t> cubes_; // texture ids uploaded as cube maps
    std::vector<uint8_t> materialBlocks_; // during upload()
    std::vector<uint8_t> objectScratch_;
    bool s3tc_ = false, debug_ = false;
    float maxAniso_ = 1.0f;
};

} // namespace

std::unique_ptr<Renderer> createGlRenderer() { return std::make_unique<GlRenderer>(); }

} // namespace ou::render
    std::vector<DrawRef> deferred_; // per frame: back/depth-buffer readers (drawScreenReaders)
