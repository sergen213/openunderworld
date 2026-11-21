// OpenGL 4.6 core backend (DSA), loaded through libepoxy.
#include "render/Fluid.hpp"
#include "render/Ocean.hpp"
#include "render/WaterField.hpp"
#include "render/OriginalShading.hpp"
#include "render/RenderCommon.hpp"
#include "render/Water.hpp"
#include "render/TimeOfDay.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <SDL3/SDL.h>
#include <epoxy/gl.h>
#include <glm/gtc/type_ptr.hpp>
#include "render/Cortao.hpp"

namespace ou::render {
namespace {

void delTexture(GLuint id) { glDeleteTextures(1, &id); }
void delBuffer(GLuint id) { glDeleteBuffers(1, &id); }
void delVertexArray(GLuint id) { glDeleteVertexArrays(1, &id); }
void delProgram(GLuint id) { glDeleteProgram(id); }
void delFramebuffer(GLuint id) { glDeleteFramebuffers(1, &id); }
void delRenderbuffer(GLuint id) { glDeleteRenderbuffers(1, &id); }
void delQuery(GLuint id) { glDeleteQueries(1, &id); }

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
using Query = GlHandle<delQuery>;

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
layout(location = 4) uniform vec2 uFade; // instance opacity, 1 = opaque material (alpha = opacity)
layout(binding = 0) uniform sampler2D uTex;
out vec4 oColor;
)";
const char* kSceneFsMain = R"(
void main() {
    vec4 c = texture(uTex, vUv) * vColor;
    if (c.a < uAlphaRef) discard;
    if (uFade.x < 1.0) c.a = (uFade.y > 0.5 ? 1.0 : c.a) * uFade.x;
    float len = length(vNormal);
    if (uLighting != 0 && len > 1e-6) {
        vec3 n = (gl_FrontFacing ? 1.0 : -1.0) * vNormal / len; // two-sided materials light both faces
        float ndl = max(dot(n, ouShadow.sun.xyz), 0.0);
        c.rgb *= 0.45 * ouAo() + 0.55 * ndl * ouSunVisibility(vWorld, n, ndl > 0.0, false); // AO: ambient only
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

// Final pass: resolved scene -> default framebuffer; optional tone mapping (AgX: shaders/tonemap.glsl, or the ACES fit with
// HDR) and FXAA.
const char* kPostVs = R"(#version 460 core
out vec2 vUv;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); // fullscreen triangle
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";

const char* kPostFsHead = R"(#version 460 core
in vec2 vUv;
layout(location = 0) uniform int uFxaa;
layout(location = 1) uniform int uHdr;
layout(location = 2) uniform float uBrightness; // display gamma (1 = neutral), as the original's brightness option
layout(location = 3) uniform float uFade;       // FrameOptions::screenFade (1 = black)
layout(location = 4) uniform int uTonemap;     // FrameOptions::tonemap: 0 original, 1 agx
layout(location = 5) uniform int uUw; // bit 0: uUwParticles holds this frame's under-water particles; bit 1: submerged,
                                      // write the linear HDR frame for the under-water composite (Water.hpp UwGrade)
layout(binding = 0) uniform sampler2D uScene;
layout(binding = 17) uniform sampler2D uUwParticles; // premultiplied, composited over the fogged scene (13-14 shafts, 16 shadow)
out vec4 oColor;
)";
const char* kTonemapGlsl =
#include "tonemap.glsl.inc"
    ;
// After kPostFsHead + kTonemapGlsl.
const char* kPostFs = R"(
vec3 fetch(vec2 uv) {
    vec3 c = textureLod(uScene, uv, 0.0).rgb;
    if ((uUw & 2) != 0) return c; // the under-water composite tone maps (uw_grade.frag tmap)
    if (uTonemap != 0) c = ouTonemapAgx(c);
    else if (uHdr != 0) c = clamp(c * (2.51 * c + 0.03) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0); // ACES (Narkowicz)
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
    vec3 c = pow(ouScreenFilter(uFxaa != 0 ? fxaa(vUv) : fetch(vUv), vUv), vec3(1.0 / uBrightness));
    vec3 u = ouUnderwater(ouAtmosphere(c, vUv), vUv);
    if ((uUw & 3) == 0) {
        oColor = vec4(u * (1.0 - uFade), 1.0);
        return;
    }
    vec3 l = pow(max(u, vec3(0.0)), vec3(2.2)); // linear, as ouUwFog
    if ((uUw & 1) != 0) {
        vec4 p = textureLod(uUwParticles, vUv, 0.0);
        l = l * (1.0 - p.a) + p.rgb;
    }
    // the composite: alpha = view depth in metres (uw_grade.frag's shaft upsample)
    oColor = (uUw & 2) != 0 ? vec4(l, agViewDepth(ivec2(gl_FragCoord.xy))) : vec4(pow(l, vec3(1.0 / 2.2)) * (1.0 - uFade), 1.0);
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
layout(binding = 3) uniform sampler2D uImage2;
out vec4 oColor;
void main() {
    if (vUv.x >= 6.0) { oColor = texture(uImage2, vUv - vec2(6.0, 0.0)) * vColor; return; }
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

// Original particle effects (FrameOptions::fxVertices): texture x colour, colour bytes 0x80 = 1.0.
const char* kFxVs = R"(#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec3 aNormal;
layout(location = 4) in vec3 aTangent;
layout(location = 0) uniform mat4 uViewProj;
out vec2 vUv; out vec4 vColor; out vec4 vRaw; out vec3 vNormal; out vec3 vTangent;
void main() {
    vUv = aUv;
    vColor = min(aColor * 2.0, vec4(2.0, 2.0, 2.0, 1.0));
    vRaw = aColor, vNormal = aNormal, vTangent = aTangent;
    gl_Position = uViewProj * vec4(aPos, 1.0);
})";
// Material particles (FxBatch): uParams x = refraction screen-uv scale (scene copy offset by the normal map's xy x the
// texture's alpha; GL's v points up, D3D's down), y / z = soft fade scale / bias against the depth copy, w > 0 lit
// (uLit0 / uLit1 = light 0 / 1 rgb + litLow / litHigh; ShaderLib 655's math, FxBatch).
const char* kFxFs = R"(#version 460 core
in vec2 vUv; in vec4 vColor; in vec4 vRaw; in vec3 vNormal; in vec3 vTangent;
layout(binding = 0) uniform sampler2D uTex;
layout(binding = 5) uniform sampler2D uScene;
layout(binding = 6) uniform sampler2D uDepth;
layout(binding = 8) uniform sampler2D uNormal;
layout(location = 1) uniform vec4 uParams;
layout(location = 2) uniform vec2 uDepthToW;
layout(location = 3) uniform vec4 uLit0;
layout(location = 4) uniform vec4 uLit1;
out vec4 oColor;
float viewDepth(float d) { return 1.0 / max(d * uDepthToW.x + uDepthToW.y, 1e-6); }
void main() {
    vec4 t = texture(uTex, vUv);
    vec2 suv = gl_FragCoord.xy / vec2(textureSize(uScene, 0));
    if (uParams.w > 0.0) {
        vec4 n = texture(uNormal, vUv);
        vec2 xy = vec2(n.x, n.y * n.w) * 2.0 - 1.0;
        vec3 w = clamp(sqrt(abs(1.0 - dot(xy, xy))), 0.0, 1.0) * vNormal + xy.x * vTangent + xy.y * cross(vTangent, vNormal);
        float f = clamp(-w.z * inversesqrt(max(dot(w, w), 1e-12)), 0.0, 1.0);
        float s = clamp((2.0 * f - uLit0.w) / (uLit1.w - uLit0.w), 0.0, 1.0);
        s = s * s * (3.0 - 2.0 * s);
        oColor = vec4(t.rgb * (mix(uLit1.rgb, uLit0.rgb, s) + vRaw.rgb), t.a * vRaw.a);
        return;
    }
    if (uParams.x > 0.0) {
        vec4 n = texture(uNormal, vUv);
        vec2 off = (vec2(n.x, n.y * n.w) * 2.0 - 1.0) * t.a * uParams.x;
        oColor = vec4(texture(uScene, suv + vec2(off.x, -off.y)).rgb, t.a * vColor.a);
        return;
    }
    float a = 1.0;
    if (uParams.y > 0.0)
        a = clamp((viewDepth(texture(uDepth, suv).r) - viewDepth(gl_FragCoord.z) - (1.0 - t.a) * uParams.z) * uParams.y, 0.0, 1.0);
    oColor = t * vColor * vec4(1.0, 1.0, 1.0, a);
})";

// ---- CORTAO ambient occlusion (docs/RENDERING.md "Ambient occlusion"; third_party/cortao, MIT) ----
const char* kCortaoBase =
#include "gl_cortao/CORTAOBase.h.inc"
    ;
const char* kCortaoPrepassH =
#include "gl_cortao/CORTAOPrepass.h.inc"
    ;
const char* kCortaoMainH =
#include "gl_cortao/CORTAOMainPass.h.inc"
    ;
const char* kCortaoFilterH =
#include "gl_cortao/CORTAOFilterPass.h.inc"
    ;
const char* kCortaoCommon =
#include "gl_cortao/cortao_common.glsl.inc"
    ;
const char* kCortaoPrepassCs =
#include "gl_cortao/cortao_prepass.comp.inc"
    ;
const char* kCortaoMainCs =
#include "gl_cortao/cortao_main.comp.inc"
    ;
const char* kCortaoFilterCs =
#include "gl_cortao/cortao_filter.comp.inc"
    ;
const char* kCortaoTemporalCs =
#include "gl_cortao/cortao_temporal.comp.inc"
    ;
const uint8_t kCortaoOccluders16[] = { // 24x24x16 R16_UINT, 16 rays
#include "gl_cortao/24x24x16x16_R16_UINT.inc"
};
const uint8_t kCortaoOccluders32[] = { // 24x24x16 R32_UINT, 32 rays
#include "gl_cortao/24x24x16x32_R32_UINT.inc"
};
// CORTAO pass source: #version, the shared defines + `defines`, cortao_common.glsl, then `wrapper` with its
// `#include <CORTAO*.h>` replaced by that header, whose own `#include "CORTAOBase.h"` becomes CORTAOBase.h.
std::string cortaoSource(const char* wrapper, const std::string& defines) {
    auto replace = [](std::string s, const std::string& from, const std::string& to) {
        if (const size_t at = s.find(from); at != std::string::npos) s.replace(at, from.size(), to);
        return s;
    };
    std::string body = wrapper;
    for (auto [name, header] : {std::pair{"<CORTAOPrepass.h>", kCortaoPrepassH}, std::pair{"<CORTAOMainPass.h>", kCortaoMainH},
                                std::pair{"<CORTAOFilterPass.h>", kCortaoFilterH}})
        body = replace(body, std::string("#include ") + name, replace(header, "#include \"CORTAOBase.h\"", kCortaoBase));
    // No GL_EXT_control_flow_attributes in desktop GL drivers (Mesa warns): cortao_unroll stays empty, the drivers
    // unroll the constant-count loops themselves.
    return "#version 460 core\n#define CORTAO_GLSL 1\n"
           "#define CORTAO_PERSPECTIVE_PROJECTION 1\n#define CORTAO_16_BIT 0\n#define CORTAO_PACKED_NORMAL_BITS " +
           std::to_string(cortao::kPackedNormalBits) + "\n" + defines + kCortaoCommon + body;
}

// AO depth pre-pass: render-resolution depth and the interpolated view-space vertex normal (octahedral RG16F), facing
// the viewer; alpha-tested like the scene draws. The scene's vertex positions are final (CPU skinning and bends; the
// translated vertex shaders read an identity bone 0), so this matches the translated programs' depth as well. Second
// target: the motion for the temporal pass (last frame's uv - this frame's, last frame's view depth / this frame's) from
// the vertex's previous position (aPrevPos: GpuMesh::prevVbo for meshes updateVertices moved since the last frame, i.e.
// skinned characters and the posed loose bodies such as the bobbing yacht) under last frame's view-projection.
const char* kAoPrepassVs = R"(#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
layout(location = 8) in vec3 aPrevPos;
layout(location = 0) uniform mat4 uViewProj;
layout(location = 1) uniform mat4 uModel;
layout(location = 2) uniform mat4 uView;
layout(location = 4) uniform mat4 uPrevViewProj;
out vec3 vNormal; out vec3 vView; out vec2 vUv; out float vAlpha; out vec4 vCur; out vec4 vPrev;
void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vNormal = mat3(uView) * (mat3(uModel) * aNormal);
    vView = (uView * world).xyz;
    vUv = aUv; vAlpha = aColor.a;
    gl_Position = vCur = uViewProj * world;
    vPrev = uPrevViewProj * (uModel * vec4(aPrevPos, 1.0));
})";
const char* kAoPrepassFsHead = R"(#version 460 core
in vec3 vNormal; in vec3 vView; in vec2 vUv; in float vAlpha; in vec4 vCur; in vec4 vPrev;
layout(location = 3) uniform float uAlphaRef;
layout(binding = 0) uniform sampler2D uTex;
layout(location = 0) out vec2 oNormal;
layout(location = 1) out vec4 oMotion;
)";
const char* kAoPrepassFsMain = R"(
void main() {
    vec3 face = cross(dFdx(vView), dFdy(vView)); // before the alpha test: uniform control flow
    if (texture(uTex, vUv).a * vAlpha < uAlphaRef) discard;
    float len = length(vNormal);
    vec3 n = len > 1e-6 ? vNormal / len : normalize(face);
    if (!gl_FrontFacing) n = -n; // two-sided materials: the visible side
    vec3 v = normalize(-vView);
    float nv = dot(n, v);
    if (nv < 0.02) n = normalize(n + v * (0.02 - nv)); // interpolated normals turned away from the eye
    oNormal = ouOctEncode(n);
    oMotion = vPrev.w > 0.0 ? vec4((vPrev.xy / vPrev.w - vCur.xy / vCur.w) * 0.5, vPrev.w / vCur.w, 1.0) : vec4(0.0);
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

// tcs / tes: optional tessellation stages (both or neither).
Program linkProgram(const char* vs, const char* fs, std::string& error, const char* tcs = nullptr, const char* tes = nullptr) {
    std::array<GLuint, 4> s{};
    const std::pair<GLenum, const char*> stages[4] = {{GL_VERTEX_SHADER, vs}, {GL_FRAGMENT_SHADER, fs},
                                                      {GL_TESS_CONTROL_SHADER, tcs}, {GL_TESS_EVALUATION_SHADER, tes}};
    auto release = [&] { for (GLuint x : s) if (x) glDeleteShader(x); };
    for (size_t i = 0; i < 4; ++i) {
        if (!stages[i].second) continue;
        if (!(s[i] = compileShader(stages[i].first, stages[i].second, error))) { release(); return {}; }
    }
    Program p(glCreateProgram());
    for (GLuint x : s) if (x) glAttachShader(p.get(), x);
    glLinkProgram(p.get());
    release();
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

Program linkCompute(const char* cs, std::string& error) {
    GLuint c = compileShader(GL_COMPUTE_SHADER, cs, error);
    if (!c) return {};
    Program p(glCreateProgram());
    glAttachShader(p.get(), c);
    glLinkProgram(p.get());
    glDeleteShader(c);
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
    // original::screenSamplers per submesh: low 16 bits back buffer, high 16 depth buffer (drawn after the opaque copies).
    std::vector<uint32_t> screenSamplers;
    // original::reflectionSamplers per submesh (the planar reflection, Targets::reflection); reflectZ: the world z of the
    // reflecting submeshes' plane (NaN: none).
    std::vector<uint32_t> reflection;
    float reflectZ = std::numeric_limits<float>::quiet_NaN();
    float waterRestZ = std::numeric_limits<float>::max(); // render::waterRestZ (max: none), the Enhanced water's clamp
    glm::mat4 model;
    glm::vec3 exteriorShift{0.0f};      // exterior terrain: translation added to `model` (followExterior)
    glm::vec3 center{0.0f};             // world-space bounds centre (blended draw sorting; light sets with `radius`)
    float radius = 0.0f;                // world-space bounding-sphere radius (original::lightSet)
    float lodNear = 0, lodFar = std::numeric_limits<float>::max(); // batch LOD (render::setLod)
    glm::vec3 lodOffset{0.0f};
    uint32_t lightMask = ~0u;           // scene::MeshData::lightMask
    // Terrain light groups (scene::MeshData::lightBoxes) and the object slot of the first one (objectSlot).
    std::vector<std::vector<std::array<float, 6>>> lightBoxes;
    uint32_t lightSlot = 0;
    bool water = false;                 // *watersurface* mesh (render/Water.hpp): Enhanced water draws it separately
    bool irradiance = false;            // a submesh's program reads IrradianceState (original::IrradianceSlots)
    std::vector<uint8_t> reflective;    // per submesh: a planar-reflection material (render::reflectsWater candidates)
    size_t source = 0, vertexCount = 0, indexCount = 0; // SceneData::meshes index, vertex / index counts
    // Residency (Renderer::upload): MeshData::key (0: sent each upload), the part holding it (0: none), changed by
    // updateVertices since it was sent (then sent again), world bounds of its vertices under `model`.
    uint64_t key = 0, part = 0;
    bool dirty = false;
    Bounds bounds;
    // CORTAO temporal motion: the vertices before the last updateVertices, copied when it ran for frame `movedFrame`
    // (GlRenderer::drawFrames_ numbering) while AO runs; vertex binding 1 (attribute 8) of `vao`.
    Buffer prevVbo;
    uint64_t movedFrame = 0;
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
    Framebuffer opaqueColorFbo, opaqueDepthFbo;
    // Enhanced water's splash fluid as liquid (render/Water.hpp splash passes): RGBA32F splat target and blur ping-pong.
    std::array<Texture, 2> splash;
    std::array<Framebuffer, 2> splashFbo;
    // Enhanced under-water particles (render/Water.hpp UwParticlePush): RGBA16F premultiplied, composited by the post pass.
    Texture uwParticles;
    Framebuffer uwParticlesFbo;
    // Original screen filters (FrameOptions::screenFilter): the half-size scene and its mip chain (blur levels).
    Texture filterBlur;
    Framebuffer filterFbo;
    // PCFeedbackEffect: the previous frame's final image, half size + mips (tru.exe 0x51A2E0); valid while hasFeedback.
    Texture feedback;
    Framebuffer feedbackFbo;
    bool hasFeedback = false;
    // Planar reflection (original::reflection): the scene seen from the mirrored camera, half size, single sample.
    Texture reflection;
    Renderbuffer reflectionDepth;
    Framebuffer reflectionFbo;
    // Enhanced under-water sun shafts (render/Water.hpp): [0] march, [1] blurred (half size, RGBA16F; units 13, 14).
    std::array<Texture, 2> shaft;
    std::array<Framebuffer, 2> shaftFbo;
    // Enhanced under-water composite (render/Water.hpp UwGrade): the post pass's linear frame (render size) and the bloom
    // chain (W >> (i + 1)), all RGBA16F, linear, clamped.
    Texture uwHdr;
    Framebuffer uwHdrFbo;
    std::array<Texture, kUwBloomLevels> uwBloom;
    std::array<Framebuffer, kUwBloomLevels> uwBloomFbo;
};

struct Resources {
    Program sceneProgram, textProgram, lineProgram, shadowProgram, postProgram;
    Program waterProgram, splashSplatProgram, splashBlurProgram, splashCompositeProgram; // Enhanced water (render/Water.cpp sources)
    Program waterTessProgram; // water.tesc / water.tese (null: no tessellation): draws GL_PATCHES of 3
    std::array<Program, 2> shaftPrograms; // under-water sun shafts: march, blur (render/Water.hpp)
    // Enhanced water's animated water texture (render/Water.hpp kAgWaterSize; abyssal-garden W_WATER): RGBA16F with
    // kAgWaterLevels mips, unit 18 (`uAgWater`: post, water, shafts, particles); program = shaders/uw_water.frag, then
    // agCausticProgram (shaders/uw_caustics.*) adds the floor caustics into g.
    Program agWaterProgram;
    Program agCausticProgram;
    Program skyProgram;                  // time of day backdrop (shaders/sky.*, render/TimeOfDay.hpp), SkyPush at UBO 5
    Buffer skyUbo;
    Texture agWater;
    Framebuffer agWaterFbo;
    Buffer waterUbo;                     // WaterBlock, UBO binding 4 (water, splash, post)
    // Interactive wave field (render/WaterField.hpp, shaders/water_field.comp): state + FFT scratch (SSBO 0 / 1), this
    // frame's kicks (SSBO 2, contract C3), the external kick buffer (SSBO 3: fieldExternal, else the zero-count
    // fieldNoKicks), bodies + wall mask (SSBO 4) and the RGBA16F uRipples (image unit 0; texture units 7 / 9). No program
    // = no interactive waves.
    Program fieldProgram;
    Buffer fieldState, fieldWork, fieldKicks, fieldNoKicks, fieldFrame;
    Texture ripples;
    GLuint fieldExternal = 0;
    waterfield::Stepper fieldStepper;
    std::unique_ptr<waterfield::KickBuffer> fieldKickData = std::make_unique<waterfield::KickBuffer>();
    std::unique_ptr<waterfield::FrameBuffer> fieldFrameData = std::make_unique<waterfield::FrameBuffer>();
    // FFT ocean (render/Ocean.hpp, shaders/ocean.comp): h0 spectra (SSBO 0), FFT scratch (SSBO 1), the RGBA16F array
    // uOcean (128^2 at level 0, 64^2 at level 1; image unit 0 / texture unit 12). No program = afl's waves.
    Program oceanProgram;
    Buffer oceanSpectrum, oceanWork;
    Texture ocean;
    ocean::Clock oceanClock;
    // The splash fluid (render/Fluid.hpp, shaders/fluid.comp; no program = no fluid): the particle pool (SSBO 0, also
    // splash.vert's), the neighbour grid (1), the C3 kicks (2) and this frame's bursts (3). OU_VK_PRESENT_STATS: three
    // timestamps per drawing frame (before its simulation, after it, after its composite), read back next frame.
    Program fluidProgram;
    Buffer fluidParticles, fluidGrid, fluidKicks, fluidBursts;
    fluid::Sim fluidSim;
    std::array<Query, 3> fluidTimes;
    bool fluidTimed = false;
    double fluidSimMs = 0, fluidDrawMs = 0;
    uint64_t fluidTimedFrames = 0;
    // Enhanced under-water particles (render/Water.hpp UwParticlePush): marine snow + bubbles, UBO binding 7.
    Program uwSnowProgram, uwBubbleProgram;
    Buffer uwUbo, uwBubbleVbo;
    VertexArray uwBubbleVao;
    Program uwBloomProgram, uwGradeProgram; // under-water composite (render/Water.hpp UwGrade)
    Program fxProgram; // original effects
    VertexArray fxVao;
    Buffer fxVbo;
    Texture white, font, shadowMap, attenuation, blackCube, cookies, pssmMap;
    struct Streamed { Texture tex; uint64_t version = 0; int width = 0, height = 0; };
    std::array<Streamed, 3> uiImages; // FrameOptions::uiImages
    // Original Fidelity (docs/formats/SHADERS.md): translated programs, register-file UBOs (bindings 0 frame,
    // 1 material, 2 object; ShaderTranslate.hpp), objects rewritten per frame.
    std::vector<GLuint> originals; // per Prepared program (owned by `linked`)
    Buffer frameUbo, materialUbo, objectUbo;
    Buffer shadowUbo; // render/sun.glsl OuShadow (UBO binding 3); the shadow map is texture unit 16
    GLint uboAlign = 256;
    Framebuffer shadowFbo, pssmFbo;
    VertexArray textVao, lineVao, emptyVao;
    Buffer textVbo, lineVbo;
    const DebugLine* lineSource = nullptr; // cached upload of FrameOptions::debugLines
    size_t lineCount = 0;
    // Scene textures by id for the current upload (owned by `resident` / `transientTextures`).
    std::unordered_map<uint32_t, GLuint> textures;
    struct ResidentTexture {
        Texture tex;
        bool cube = false;
        uint64_t part = 0, upload = 0; // the part holding it (0: none); the last upload listing it
    };
    std::unordered_map<uint64_t, ResidentTexture> resident; // TextureData::key -> texture
    std::vector<Texture> transientTextures;                 // unkeyed: the current upload's only
    std::vector<GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuMesh> parked; // keyed meshes held by a part, not in the current upload
    // Linked original programs by (VS, PS) source, kept while an upload uses them; `error` when linking failed.
    struct Linked { Program program; std::string error; uint64_t upload = 0; };
    std::map<std::pair<std::string, std::string>, Linked> linked;
    Bounds bounds;
    Targets targets;
    std::vector<DrawRef> order;
    // CORTAO ambient occlusion (runAmbientOcclusion): programs built on first use (main pass per sample count), the
    // render-resolution targets (rebuilt with the viewport) and the pass parameter UBO (binding 6, one aligned range
    // per pass). `history[current]` is this frame's ouAoMap (sun.glsl, texture unit 17).
    struct Ao {
        bool failed = false; // a program did not build: AO stays off
        Program prepass, cortaoPrepass, filter, temporal;
        std::map<int, Program> main; // by CORTAO_SAMPLE_COUNT
        Texture occluders16, occluders32;
        Buffer params;
        int width = 0, height = 0, mips = 1;
        Texture depth, normal, motion, packed, raw, filtered;
        std::array<Texture, 2> history;
        Framebuffer fbo;
        int current = 0, samples = 0;
        bool historyValid = false;
        uint64_t upload = 0;
        uint32_t frame = 0;
        glm::mat4 prevViewProj{1.0f};
    } ao;
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
        const std::string postFs = std::string(kPostFsHead) + kTonemapGlsl + kPostFs + sunGlsl() + underwaterPostSource(false) +
                                   kPostFsMain; // sun: shafts
        r.postProgram = linkProgram(kPostVs, postFs.c_str(), error);
        if (!r.postProgram.get()) return false;
        // Per vertex, and tessellated where the driver builds it (render/Water.hpp: the field's waves as geometry; the
        // meshes reaching into the field's window; not with OU_WATER_NO_TESS, for comparisons).
        const std::string waterVs = waterVertexSource(false), waterFs = waterFragmentSource(false);
        r.waterProgram = linkProgram(waterVs.c_str(), waterFs.c_str(), error);
        if (!r.waterProgram.get()) return false;
        if (!std::getenv("OU_WATER_NO_TESS")) {
            const std::string vs = waterVertexSource(false, true), tcs = waterTessControlSource(false), tes = waterTessEvalSource(false);
            std::string tessError;
            r.waterTessProgram = linkProgram(vs.c_str(), waterFs.c_str(), tessError, tcs.c_str(), tes.c_str());
            if (!r.waterTessProgram.get()) std::fprintf(stderr, "ou: water tessellation unavailable (%s)\n", tessError.c_str());
        }
        const std::string splashVs = splashVertexSource(false), splatFs = splashSplatSource(false),
                          screenVs = splashScreenVertexSource(false), blurFs = splashBlurSource(false),
                          compositeFs = splashCompositeSource(false);
        r.splashSplatProgram = linkProgram(splashVs.c_str(), splatFs.c_str(), error);
        if (!r.splashSplatProgram.get()) return false;
        r.splashBlurProgram = linkProgram(screenVs.c_str(), blurFs.c_str(), error);
        if (!r.splashBlurProgram.get()) return false;
        r.splashCompositeProgram = linkProgram(screenVs.c_str(), compositeFs.c_str(), error);
        if (!r.splashCompositeProgram.get()) return false;
        for (int i = 0; i < 2; ++i) {
            const std::string fs = underwaterShaftSource(i);
            r.shaftPrograms[size_t(i)] = linkProgram(kPostVs, fs.c_str(), error);
            if (!r.shaftPrograms[size_t(i)].get()) return false;
        }
        r.agWaterProgram = linkProgram(kPostVs, agWaterSource().c_str(), error);
        if (!r.agWaterProgram.get()) return false;
        r.agCausticProgram = linkProgram(agCausticSource(true).c_str(), agCausticSource(false).c_str(), error);
        if (!r.agCausticProgram.get()) return false;
        r.skyProgram = linkProgram(std::string(skySource(true)).c_str(), std::string(skySource(false)).c_str(), error);
        if (!r.skyProgram.get()) return false;
        r.skyUbo = newBuffer();
        glNamedBufferStorage(r.skyUbo.get(), sizeof(SkyPush), nullptr, GL_DYNAMIC_STORAGE_BIT);
        r.agWater = newTexture();
        glTextureStorage2D(r.agWater.get(), kAgWaterLevels, GL_RGBA16F, kAgWaterSize, kAgWaterSize);
        glTextureParameteri(r.agWater.get(), GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(r.agWater.get(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(r.agWater.get(), GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(r.agWater.get(), GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTextureParameteri(r.agWater.get(), GL_TEXTURE_MAX_LEVEL, kAgWaterLevels - 1);
        if (maxAniso_ > 1.0f) glTextureParameterf(r.agWater.get(), GL_TEXTURE_MAX_ANISOTROPY, std::min(maxAniso_, 8.0f)); // the demo's sR
        for (GLint level = 0; level < kAgWaterLevels; ++level) glClearTexImage(r.agWater.get(), level, GL_RGBA, GL_FLOAT, nullptr);
        r.agWaterFbo = newFramebuffer();
        glNamedFramebufferTexture(r.agWaterFbo.get(), GL_COLOR_ATTACHMENT0, r.agWater.get(), 0);
        const std::string bloomFs = uwBloomSource(), gradeFs = uwGradeSource();
        r.uwBloomProgram = linkProgram(kPostVs, bloomFs.c_str(), error);
        if (!r.uwBloomProgram.get()) return false;
        r.uwGradeProgram = linkProgram(kPostVs, gradeFs.c_str(), error);
        if (!r.uwGradeProgram.get()) return false;
        r.waterUbo = newBuffer();
        glNamedBufferStorage(r.waterUbo.get(), sizeof(WaterBlock), nullptr, GL_DYNAMIC_STORAGE_BIT);
        { // FFT ocean; a driver that cannot build ocean.comp keeps afl's waves (WaterBlock::ocean[0].x cleared per frame)
            std::string oceanError;
            r.oceanProgram = linkCompute(ocean::computeSource().c_str(), oceanError);
            if (!r.oceanProgram.get()) std::fprintf(stderr, "ou: FFT ocean unavailable (%s)\n", oceanError.c_str());
            const auto spectrum = ocean::spectrumBuffer();
            r.oceanSpectrum = newBuffer();
            glNamedBufferStorage(r.oceanSpectrum.get(), GLsizeiptr(spectrum.size() * sizeof(glm::vec4)), spectrum.data(), 0);
            r.oceanWork = newBuffer();
            glNamedBufferStorage(r.oceanWork.get(), GLsizeiptr(ocean::kWorkVec4s * sizeof(glm::vec4)), nullptr, 0);
            GLuint id = 0;
            glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &id);
            r.ocean = Texture(id);
            glTextureStorage3D(id, ocean::kLevels, GL_RGBA16F, ocean::kMaxN, ocean::kMaxN, ocean::kLayers);
            for (GLint level = 0; level < ocean::kLevels; ++level) glClearTexImage(id, level, GL_RGBA, GL_FLOAT, nullptr);
            // water.frag filters through the mips (anisotropic: grazing views keep the waves across the line of sight);
            // water.vert and the caustics pick the grid's level
            glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTextureParameteri(id, GL_TEXTURE_MAX_LEVEL, ocean::kLevels - 1);
            if (maxAniso_ > 1.0f) glTextureParameterf(id, GL_TEXTURE_MAX_ANISOTROPY, std::min(maxAniso_, ocean::kMaxAniso));
        }
        { // the splash fluid; a driver that cannot build fluid.comp draws no splashes
            std::string fluidError;
            r.fluidProgram = linkCompute(fluid::computeSource().c_str(), fluidError);
            if (!r.fluidProgram.get()) std::fprintf(stderr, "ou: splash fluid unavailable (%s)\n", fluidError.c_str());
            const GLuint zero = 0;
            r.fluidParticles = newBuffer();
            glNamedBufferStorage(r.fluidParticles.get(), GLsizeiptr(fluid::kParticleBytes), nullptr, GL_DYNAMIC_STORAGE_BIT);
            glClearNamedBufferData(r.fluidParticles.get(), GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero); // all dead
            r.fluidGrid = newBuffer();
            glNamedBufferStorage(r.fluidGrid.get(), GLsizeiptr(fluid::kGridBytes), nullptr, GL_DYNAMIC_STORAGE_BIT);
            r.fluidKicks = newBuffer();
            glNamedBufferStorage(r.fluidKicks.get(), GLsizeiptr(fluid::kKickBytes), nullptr, GL_DYNAMIC_STORAGE_BIT);
            glClearNamedBufferData(r.fluidKicks.get(), GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
            r.fluidBursts = newBuffer();
            glNamedBufferStorage(r.fluidBursts.get(), GLsizeiptr(fluid::kBurstBytes), nullptr, GL_DYNAMIC_STORAGE_BIT);
            for (Query& q : r.fluidTimes) {
                GLuint id = 0;
                glCreateQueries(GL_TIMESTAMP, 1, &id);
                q = Query(id);
            }
        }
        { // the interactive wave field; a driver that cannot build water_field.comp draws the water without it
            std::string fieldError;
            r.fieldProgram = linkCompute(waterfield::computeSource().c_str(), fieldError);
            if (!r.fieldProgram.get()) std::fprintf(stderr, "ou: interactive wave field unavailable (%s)\n", fieldError.c_str());
            const GLsizeiptr cells = GLsizeiptr(waterfield::kN) * waterfield::kN;
            r.fieldState = newBuffer();
            glNamedBufferStorage(r.fieldState.get(), cells * GLsizeiptr(sizeof(glm::vec4)), nullptr, 0); // h, v, whitewater, spare
            r.fieldWork = newBuffer();
            glNamedBufferStorage(r.fieldWork.get(), cells * GLsizeiptr(sizeof(glm::vec2)), nullptr, 0);
            r.fieldKicks = newBuffer();
            glNamedBufferStorage(r.fieldKicks.get(), sizeof(waterfield::KickBuffer), nullptr, GL_DYNAMIC_STORAGE_BIT);
            r.fieldNoKicks = newBuffer();
            const waterfield::KickBuffer none{};
            glNamedBufferStorage(r.fieldNoKicks.get(), sizeof none, &none, 0);
            r.fieldFrame = newBuffer();
            glNamedBufferStorage(r.fieldFrame.get(), sizeof(waterfield::FrameBuffer), nullptr, GL_DYNAMIC_STORAGE_BIT);
            r.ripples = newTexture();
            glTextureStorage2D(r.ripples.get(), 1, GL_RGBA16F, waterfield::kN, waterfield::kN);
            glClearTexImage(r.ripples.get(), 0, GL_RGBA, GL_FLOAT, nullptr);
            glTextureParameteri(r.ripples.get(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(r.ripples.get(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(r.ripples.get(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(r.ripples.get(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            // contract C3: the splash fluid's particles falling back kick the waves (the next frame's field takes them)
            if (r.fluidProgram.get()) r.fieldExternal = r.fluidKicks.get();
        }
        const std::string uwSnowVs = uwSnowVertexSource(false), uwSnowFs = uwSnowFragmentSource(false),
                          uwBubbleVs = uwBubbleVertexSource(false), uwBubbleFs = uwBubbleFragmentSource(false);
        r.uwSnowProgram = linkProgram(uwSnowVs.c_str(), uwSnowFs.c_str(), error);
        if (!r.uwSnowProgram.get()) return false;
        r.uwBubbleProgram = linkProgram(uwBubbleVs.c_str(), uwBubbleFs.c_str(), error);
        if (!r.uwBubbleProgram.get()) return false;
        r.uwUbo = newBuffer();
        glNamedBufferStorage(r.uwUbo.get(), sizeof(UwParticlePush), nullptr, GL_DYNAMIC_STORAGE_BIT);
        r.uwBubbleVao = newVertexArray();
        r.uwBubbleVbo = newBuffer();
        glVertexArrayVertexBuffer(r.uwBubbleVao.get(), 0, r.uwBubbleVbo.get(), 0, sizeof(Bubble));
        glVertexArrayBindingDivisor(r.uwBubbleVao.get(), 0, 1); // one Bubble per instance
        attrib(r.uwBubbleVao.get(), 0, 4, GL_FLOAT, GL_FALSE, offsetof(Bubble, pos));   // pos + radius
        attrib(r.uwBubbleVao.get(), 1, 4, GL_FLOAT, GL_FALSE, offsetof(Bubble, alpha)); // alpha, seed, rise, wobble
        r.fxProgram = linkProgram(kFxVs, kFxFs, error);
        if (!r.fxProgram.get()) return false;
        r.fxVao = newVertexArray();
        r.fxVbo = newBuffer();
        glVertexArrayVertexBuffer(r.fxVao.get(), 0, r.fxVbo.get(), 0, sizeof(FxVertex));
        attrib(r.fxVao.get(), 0, 3, GL_FLOAT, GL_FALSE, offsetof(FxVertex, pos));
        attrib(r.fxVao.get(), 1, 2, GL_FLOAT, GL_FALSE, offsetof(FxVertex, uv));
        attrib(r.fxVao.get(), 2, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(FxVertex, rgba));
        attrib(r.fxVao.get(), 3, 3, GL_BYTE, GL_TRUE, offsetof(FxVertex, normal));
        attrib(r.fxVao.get(), 4, 3, GL_BYTE, GL_TRUE, offsetof(FxVertex, tangent));
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
        // tru.exe's PSSM atlas (original::Pssm, s14 of the PSSM programs): always allocated for the same reason; the area
        // outside the split tiles stays at depth 1 (the ShadowLights' lit texel).
        r.pssmMap = newTexture();
        GLuint pm = r.pssmMap.get();
        glTextureStorage2D(pm, 1, GL_DEPTH_COMPONENT32F, original::kPssmWidth, original::kPssmHeight);
        glTextureParameteri(pm, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(pm, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(pm, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(pm, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(pm, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTextureParameteri(pm, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        r.pssmFbo = newFramebuffer();
        glNamedFramebufferTexture(r.pssmFbo.get(), GL_DEPTH_ATTACHMENT, pm, 0);
        glNamedFramebufferDrawBuffer(r.pssmFbo.get(), GL_NONE);
        const float one = 1.0f;
        glClearNamedFramebufferfv(r.pssmFbo.get(), GL_DEPTH, 0, &one);

        const auto lut = original::attenuationLut();
        r.attenuation = newTexture();
        glTextureStorage2D(r.attenuation.get(), 1, GL_RGBA8, lut.width, lut.height);
        glTextureSubImage2D(r.attenuation.get(), 0, 0, 0, lut.width, lut.height, GL_RGBA, GL_UNSIGNED_BYTE, lut.rgba.data());
        glTextureParameteri(r.attenuation.get(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(r.attenuation.get(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(r.attenuation.get(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(r.attenuation.get(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        GLuint cookies = 0; // s12/s13 spot cookies (original::cookieArray), filled per scene in upload()
        glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &cookies);
        r.cookies = Texture(cookies);
        glTextureStorage3D(cookies, 1, GL_RGBA8, original::kCookieSize, original::kCookieSize, original::kCookieLayers);
        glTextureParameteri(cookies, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(cookies, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(cookies, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(cookies, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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

    // Keyed content stays resident (Renderer::upload): textures by TextureData::key, meshes by MeshData::key (their
    // buffers; the per-upload state, programs, material blocks and samplers, is rebuilt for every mesh). Linked original
    // programs are kept by source. GL deletes in order with the frames using them, so dropped objects go at once.
    std::vector<std::string> upload(const scene::SceneData& scene) override {
        std::vector<std::string> warnings;
        Resources& r = *res_;
        pumpParts(SIZE_MAX, warnings); // parts still in flight: the scene may list them
        ++uploads_;
        r.textures.clear();
        cubes_.clear();
        std::vector<Texture> transient;
        for (const auto& t : scene.textures) {
            if (r.textures.contains(t.id)) { // first id wins, as formats::loadTextures and the Vulkan backend
                warnings.push_back("texture " + std::to_string(t.id) + " (" + t.origin + "): duplicate texture id, first one wins");
                continue;
            }
            GLuint id = 0;
            if (t.key) {
                auto it = r.resident.find(t.key);
                if (it == r.resident.end()) {
                    Texture tex = uploadTexture(t, warnings);
                    if (!tex.get()) continue;
                    it = r.resident.emplace(t.key, Resources::ResidentTexture{std::move(tex), t.cube}).first;
                }
                it->second.upload = uploads_;
                id = it->second.tex.get();
            } else {
                Texture tex = uploadTexture(t, warnings);
                if (!tex.get()) continue;
                id = tex.get();
                transient.push_back(std::move(tex));
            }
            r.textures[t.id] = id;
            if (t.cube) cubes_.insert(t.id);
        }
        std::erase_if(r.resident, [&](const auto& kv) { return kv.second.upload != uploads_ && !parts_.contains(kv.second.part); });
        r.transientTextures = std::move(transient);
        environment_ = scene.environment;
        uploadSun_ = original::sunFields(environment_);
        irradiance_.reset(); // mesh indices and probes change with the scene
        // AttenuationSampler light rows follow the scene's light falloff curves (SHADERS.md "Engine inputs").
        const auto lut = original::attenuationLut(environment_);
        glTextureSubImage2D(r.attenuation.get(), 0, 0, 0, lut.width, lut.height, GL_RGBA, GL_UNSIGNED_BYTE, lut.rgba.data());
        fogRow_ = original::fogLutRow(nullptr);
        cookieIds_ = original::cookieIds(environment_);
        const auto cookies = original::cookieArray(cookieIds_, scene.textures);
        glTextureSubImage3D(r.cookies.get(), 0, 0, 0, 0, cookies.width, cookies.height, cookies.layers, GL_RGBA, GL_UNSIGNED_BYTE,
                            cookies.rgba.data());
        prepareOriginals(scene, warnings);
        std::unordered_map<uint64_t, GpuMesh> pool = std::move(r.parked);
        r.parked.clear();
        for (auto& g : r.meshes)
            if (g.key && !g.dirty) pool.emplace(g.key, std::move(g));
        r.meshes.clear();
        std::unordered_set<uint64_t> listed; // a key twice: the copies are sent each upload
        r.bounds = {};
        for (size_t i = 0; i < scene.meshes.size(); ++i) {
            const scene::MeshData& m = scene.meshes[i];
            const bool keyed = m.key && listed.insert(m.key).second;
            const auto it = keyed ? pool.find(m.key) : pool.end();
            GpuMesh g;
            if (it != pool.end() && it->second.vertexCount == m.vertices.size() && it->second.indexCount == m.indices.size()) {
                g = std::move(it->second);
                pool.erase(it);
                place(g, m);
            } else if (!createMesh(m, g, warnings)) {
                continue;
            }
            g.key = keyed ? m.key : 0;
            g.source = i;
            g.lightMask = m.lightMask;
            bindOriginals(g, m);
            r.bounds.lo = glm::min(r.bounds.lo, g.bounds.lo), r.bounds.hi = glm::max(r.bounds.hi, g.bounds.hi);
            r.meshes.push_back(std::move(g));
        }
        for (auto& [key, g] : pool)
            if (parts_.contains(g.part)) r.parked.emplace(key, std::move(g)); // the rest is freed here
        finishOriginals();
        std::erase_if(r.linked, [&](const auto& kv) { return kv.second.upload != uploads_; });
        prepared_ = {};
        return warnings;
    }

    void uploadPart(uint64_t id, std::shared_ptr<const scene::SceneData> part) override { parts_[id] = {std::move(part)}; }

    void releasePart(uint64_t id) override {
        Resources& r = *res_;
        if (!parts_.erase(id)) return;
        std::erase_if(r.resident, [&](auto& kv) {
            if (kv.second.part != id) return false;
            kv.second.part = 0;
            return kv.second.upload != uploads_; // listed by the current upload: until the next one
        });
        std::erase_if(r.parked, [&](const auto& kv) { return kv.second.part == id; });
        for (auto& g : r.meshes)
            if (g.part == id) g.part = 0;
    }

    size_t residentCount() const override {
        size_t n = res_->resident.size() + res_->parked.size();
        for (const auto& g : res_->meshes) n += g.key != 0;
        return n;
    }

    bool updateVertices(size_t meshIndex, std::span<const scene::Vertex> vertices) override {
        for (auto& g : res_->meshes)
            if (g.source == meshIndex) {
                if (g.vertexCount != vertices.size()) return false;
                if (res_->ao.historyValid && g.movedFrame != drawFrames_ + 1) { // the pose the last frame drew: AO motion
                    if (!g.prevVbo.get()) {
                        g.prevVbo = newBuffer();
                        glNamedBufferStorage(g.prevVbo.get(), GLsizeiptr(vertices.size_bytes()), nullptr, 0);
                    }
                    glCopyNamedBufferSubData(g.vbo.get(), g.prevVbo.get(), 0, 0, GLsizeiptr(vertices.size_bytes()));
                    g.movedFrame = drawFrames_ + 1;
                }
                glNamedBufferSubData(g.vbo.get(), 0, GLsizeiptr(vertices.size_bytes()), vertices.data());
                const glm::vec4 sphere = original::boundingSphere(vertices, g.model); // skinned: follows the pose
                g.center = glm::vec3(sphere), g.radius = sphere.w;
                g.dirty = true; // the next upload sends the scene's vertices again
                return true;
            }
        return false;
    }

    void resize(Viewport) override {} // GL default framebuffer follows the window; viewport set per frame

    void drawFrame(const Camera& camera, Viewport vp, const FrameOptions& options) override {
        Resources& r = *res_;
        if (vp.width <= 0 || vp.height <= 0) return;
        ++drawFrames_;
        for (size_t i = 0; i < options.dynamicLights.size() && i < environment_.lights.size(); ++i) environment_.lights[i] = options.dynamicLights[i];
        if (options.unitLook) original::applyUnitLook(environment_, *options.unitLook);
        original::applySky(environment_, uploadSun_, options.sky); // time of day: its sun, else the scene's
        {
            std::vector<std::string> ignored; // the upload listing them reports them
            pumpParts(kPartBytesPerFrame, ignored);
        }
        followExterior(r.meshes, camera.position, options.exteriorOrigin);
        ensureTargets(vp, options);
        const float aspect = float(vp.width) / float(vp.height);
        const glm::mat4 viewProj = camera.projection(aspect) * camera.view();
        const bool enhancedWater = options.water == WaterQuality::Enhanced;
        waterSurfaces_ = 0; // drawWater counts this frame's
        WaterBlock wb = waterBlock(camera, vp, viewProj, environment_, options);
        if (!enhancedWater) wb.fogColor.w = 0.0f; // Original: no underwater post either
        if (wb.ocean[0].x > 0.5f) { // FFT ocean this frame (render/Ocean.hpp)
            if (r.oceanProgram.get()) runOcean(options);
            else wb.ocean[0].x = 0.0f; // afl's waves
        }
        if (!runWaterField(options, wb)) wb.rippleInfo.y = 0.0f; // the interactive waves (render/WaterField.hpp)
        const bool feedback = options.screenFilter && options.screenFilter->feedback();
        wb.filter[7].w = feedback && r.targets.hasFeedback ? 1.0f : 0.0f; // last frame's copy exists (+0x3D)
        glNamedBufferSubData(r.waterUbo.get(), 0, sizeof wb, &wb);
        glBindBufferBase(GL_UNIFORM_BUFFER, 4, r.waterUbo.get());
        meshOpacities(options.meshOpacity, r.meshes.size(), opacity_);
        sea_.assign(enhancedWater ? r.meshes.size() : 0, 0);
        for (size_t i = 0; i < sea_.size(); ++i)
            sea_[i] = reflectsWater(r.meshes[i].center, r.meshes[i].radius, r.meshes[i].reflectZ, options.waterVolumes);
        drawOrder(r.meshes, camera.position, r.order, opacity_);
        lodEye_ = camera.position;
        unitClips_ = options.unitClips;
        meshClip_.resize(r.meshes.size());
        for (size_t i = 0; i < r.meshes.size(); ++i) meshClip_[i] = clipIndex(unitClips_, r.meshes[i].source);
        std::erase_if(r.order, [&](const DrawRef& d) { return clippedAway(d.mesh); });
        // No ray tracing here: a requested RT mode renders with the shadow map (as Vulkan without RT support).
        const bool shadows = options.shadows || options.rt != RayTracing::Off;
        const SunShadow sun = sunShadow(camera, vp, r.bounds, sunToward(environment_), false, shadows, 0, 0);
        if (shadows) drawShadowMap(sun);
        glNamedBufferSubData(r.shadowUbo.get(), 0, sizeof sun.block, &sun.block);
        glBindBufferBase(GL_UNIFORM_BUFFER, 3, r.shadowUbo.get());
        glBindTextureUnit(16, r.shadowMap.get());
        // Original preset: tru.exe's own sun shadows for the translated programs (they read lit otherwise).
        pssm_ = {};
        pssmCamera_ = camera;
        if (options.pssm && options.originalShaders && !r.originals.empty()) {
            pssm_ = original::pssm(camera, float(vp.width) / float(std::max(vp.height, 1)), environment_);
            if (pssm_.active) drawPssm(pssm_);
        }
        const auto reflection = drawReflection(camera, vp, options);
        // Screen-space AO for the main scene pass only (the reflection above saw screen.w = 0): ouAoMap on unit 17.
        aoDrawn_ = runAmbientOcclusion(camera, vp, viewProj, options);
        if (aoDrawn_) {
            SunShadowBlock block = sun.block;
            block.screen.w = 4.0f; // sun.glsl ouAo: map valid
            glNamedBufferSubData(r.shadowUbo.get(), 0, sizeof block, &block);
            glBindTextureUnit(17, r.ao.history[size_t(r.ao.current)].get());
        }

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
        if (originals) beginOriginals(camera, vp, options, reflection ? &*reflection : nullptr);
        uint32_t boundMesh = UINT32_MAX, boundObject = UINT32_MAX;
        int state = -1, program = -1; // -1 = generic scene program
        deferred_.clear();
        const size_t exterior = exteriorCount(r.meshes, std::span<const DrawRef>(r.order));
        // The backdrop tint after the opaque exterior, its stars after the blended exterior (dome cards cover them).
        const size_t skyAt = exteriorBlendStart(r.meshes, std::span<const DrawRef>(r.order), exterior,
                                                [&](uint32_t m) { return m < opacity_.size() ? opacity_[m] : 1.0f; });
        if (!exterior && drawSky(camera, vp, options, 3, false)) program = -3, boundMesh = UINT32_MAX;
        for (const DrawRef& d : r.order) {
            const size_t i = size_t(&d - r.order.data());
            if (exterior && i == skyAt && skyAt < exterior) {
                scissorFor(UINT32_MAX, vp);
                if (drawSky(camera, vp, options, 1, true)) program = -3, boundMesh = UINT32_MAX;
                state = -1;
            }
            if (exterior && i == exterior) { // exterior drawn: behind everything else
                scissorFor(UINT32_MAX, vp);
                glDepthMask(GL_TRUE);
                glClear(GL_DEPTH_BUFFER_BIT);
                if (drawSky(camera, vp, options, skyAt < exterior ? 2 : 3, true)) program = -3, boundMesh = UINT32_MAX;
                state = -1;
            }
            const GpuMesh& m = r.meshes[d.mesh];
            const scene::SubMesh& s = m.submeshes[d.sub];
            const float op = d.mesh < opacity_.size() ? opacity_[d.mesh] : 1.0f;
            const scene::Blend blend = drawBlend(s, op);
            if (enhancedWater && waterDraw(d.mesh, d.sub)) continue; // drawn by drawWater with the opaque copies
            scissorFor(d.mesh, vp);
            const int p = originals ? m.program[d.sub] : -1;
            if (p != program) {
                glUseProgram(p < 0 ? r.sceneProgram.get() : r.originals[size_t(p)]);
                if (p < 0) boundMesh = UINT32_MAX; // re-send uModel to the generic program
                program = p;
            }
            if (d.mesh != boundMesh && p < 0) glUniformMatrix4fv(1, 1, GL_FALSE, glm::value_ptr(m.model));
            if (d.mesh != boundMesh) {
                glBindVertexArray(m.vao.get());
                boundMesh = d.mesh;
            }
            if (int(blend) != state) setBlend(state = int(blend));
            if (blend == scene::Blend::Alpha) glDepthMask(drawDepthWrite(s, op) ? GL_TRUE : GL_FALSE);
            if (p >= 0) {
                if (m.screenSamplers[d.sub]) { // reads the scene drawn so far: after the loop, from the copies
                    deferred_.push_back(d);
                    continue;
                }
                if (const uint32_t slot = objectSlot(m, d); slot != boundObject) {
                    glBindBufferRange(GL_UNIFORM_BUFFER, 2, r.objectUbo.get(), GLintptr(slot) * objectStride(), sizeof(sm3::ObjectBlock));
                    boundObject = slot;
                }
                glBindBufferRange(GL_UNIFORM_BUFFER, 1, r.materialUbo.get(), m.materialOffset[d.sub], sizeof(sm3::MaterialBlock));
                glBindTextures(0, 16, originalTextures(m, d).data());
                drawSides(s, op, options.cullBackFaces, [&] { drawElements(s); });
                continue;
            }
            glUniform1f(3, s.alphaRef / 255.0f);
            glUniform2f(4, op, s.blend == scene::Blend::Opaque ? 1.0f : 0.0f);
            drawSides(s, op, options.cullBackFaces, [&] { drawSubmesh(s); });
        }
        scissorFor(UINT32_MAX, vp); // off: blits obey the scissor
        if (originals && !deferred_.empty()) drawScreenReaders(vp, options.cullBackFaces);
        setBlend(int(scene::Blend::Opaque));
        if (enhancedWater) {
            drawAgWater(wb.eye.w);
            drawWater(camera, vp, options);
        } else if (wb.haze.w > 0.5f || wb.weather.x > 0.5f || wb.waterPass[2].x > 0) // post-pass depth readers
            glBlitNamedFramebuffer(r.targets.scene.get(), r.targets.opaqueDepthFbo.get(), 0, 0, vp.width, vp.height, 0, 0,
                                   vp.width, vp.height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        drawFx(camera, vp, viewProj, options);
        drawLines(viewProj, options.debugLines);
        const bool particles = enhancedWater && uwParticlesOn(wb);
        if (particles) drawUwParticles(camera, vp, wb, options);

        Targets& t = r.targets;
        if (t.msaa > 1)
            glBlitNamedFramebuffer(t.scene.get(), t.resolve.get(), 0, 0, vp.width, vp.height, 0, 0, vp.width, vp.height,
                                   GL_COLOR_BUFFER_BIT, GL_NEAREST);
        // Half-size scene + mips: the screen filters' blur levels.
        const ScreenFilter* sf = options.screenFilter && options.screenFilter->active() ? options.screenFilter : nullptr;
        if (sf) {
            glBlitNamedFramebuffer(t.msaa > 1 ? t.resolve.get() : t.scene.get(), t.filterFbo.get(), 0, 0, vp.width, vp.height,
                                   0, 0, std::max(1, vp.width / 2), std::max(1, vp.height / 2), GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glGenerateTextureMipmap(t.filterBlur.get());
            if (sf && sf->dof) // the DOF curve reads the scene depth
                glBlitNamedFramebuffer(t.scene.get(), t.opaqueDepthFbo.get(), 0, 0, vp.width, vp.height, 0, 0, vp.width,
                                       vp.height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        }
        // Submerged in Enhanced water (UwGrade): the post pass writes the linear frame into uwHdr, then the bloom chain and
        // the grade (abyssal-garden W_BLIT + W_POST) write the default framebuffer; the grade adds the shafts.
        const UwGrade grade = enhancedWater ? uwGrade(wb) : UwGrade{};
        const bool shafts = grade.on && options.lightShafts && underwaterShaftsActive(wb);
        if (shafts) { // half-size march, blur (uw_grade.frag upsamples)
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glBindTextureUnit(6, t.opaqueDepth.get());
            glBindTextureUnit(9, r.ripples.get());
            glBindTextureUnit(12, r.ocean.get());
            glBindTextureUnit(13, t.shaft[0].get());
            glBindVertexArray(r.emptyVao.get());
            glViewport(0, 0, (vp.width + 1) / 2, (vp.height + 1) / 2);
            for (size_t i = 0; i < 2; ++i) { // output texture of march, blur
                glBindFramebuffer(GL_FRAMEBUFFER, t.shaftFbo[i].get());
                glUseProgram(r.shaftPrograms[i].get());
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, grade.on ? t.uwHdrFbo.get() : 0);
        glViewport(0, 0, vp.width, vp.height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glUseProgram(r.postProgram.get());
        glUniform1i(0, options.fxaa ? 1 : 0);
        glUniform1i(1, options.hdr ? 1 : 0);
        glUniform1f(2, options.brightness);
        glUniform1f(3, std::clamp(options.screenFade, 0.0f, 1.0f));
        glUniform1i(4, options.tonemap == Tonemap::Agx ? 1 : 0);
        glUniform1i(5, (particles ? 1 : 0) | (grade.on ? 2 : 0));
        glBindTextureUnit(17, r.targets.uwParticles.get()); // the under-water particles (unused unless submerged)
        glBindTextureUnit(0, t.color.get());
        glBindTextureUnit(6, t.opaqueDepth.get()); // underwater post (unused unless submerged)
        glBindTextureUnit(7, t.filterBlur.get());  // screen filters (unused unless one runs)
        glBindTextureUnit(11, t.feedback.get());   // the feedback effect's previous frame
        glBindTextureUnit(9, r.ripples.get()); // underwater caustics' wave field
        glBindTextureUnit(12, r.ocean.get());                                     // ... and the FFT ocean
        glBindVertexArray(r.emptyVao.get());
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (grade.on) drawUwComposite(vp, grade, shafts, std::clamp(options.screenFade, 0.0f, 1.0f));
        if (feedback) { // 0x509630 -> 0x51A2E0: this frame's image (before the HUD) for the next frame's feedback
            glBlitNamedFramebuffer(0, t.feedbackFbo.get(), 0, 0, vp.width, vp.height, 0, 0, std::max(1, vp.width / 2),
                                   std::max(1, vp.height / 2), GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glGenerateTextureMipmap(t.feedback.get());
        }
        t.hasFeedback = feedback;
        drawOverlay(vp, options.overlay, options.ui, options.uiImages);
    }

    // (Re)creates the texture on a size change and uploads when the version changes; empty images keep the old one.
    static void upload(Resources::Streamed& s, const RgbaImage& img) {
        if (img.rgba.empty()) return;
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
        glReadPixels(0, 0, vp.width, vp.height, GL_RGB, GL_UNSIGNED_BYTE, raw.data());
        size_t row = size_t(vp.width) * 3;
        for (int y = 0; y < vp.height; ++y) // GL origin is bottom-left
            std::copy_n(raw.data() + size_t(vp.height - 1 - y) * row, row, img.rgb.data() + size_t(y) * row);
        return img;
    }

    void present() override {
        SDL_GL_SwapWindow(window_);
        if (presentStats_) presentTimes_.push_back(std::chrono::steady_clock::now());
    }

    bool setVsync(bool enabled) override { return SDL_GL_SetSwapInterval(enabled ? 1 : 0); }
    // No ray tracing on GL; reports the AO the last frame drew (RenderCommon rtSummary).
    RtStatus rtStatus() const override {
        return {.ao = aoDrawn_ ? AmbientOcclusion::Cortao : AmbientOcclusion::Off, .waterSurfaces = waterSurfaces_};
    }
    bool aoDrawn_ = false;
    uint32_t waterSurfaces_ = 0; // the last frame's Enhanced water surface draws (RtStatus::waterSurfaces)

    std::string description() const override {
        auto str = [](GLenum e) { auto* s = glGetString(e); return s ? reinterpret_cast<const char*>(s) : "?"; };
        return std::string("OpenGL ") + str(GL_VERSION) + " | " + str(GL_RENDERER) + " | s3tc " +
               (s3tc_ ? "yes" : "NO") + " | aniso " + std::to_string(int(maxAniso_)) + "x" +
               (debug_ ? " | KHR_debug" : "");
    }

    // Contract C3: the particles' wave kicks (fluid::kKickBytes, std430 OuWaveKick buffer), written by simulateFluid's
    // dispatches (count zeroed at their start) and followed by glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT). 0 without
    // GL resources.
    GLuint fluidKickBuffer() const { return res_ ? res_->fluidKicks.get() : 0; }

    void shutdown() override {
        if (!context_) return;
        // OU_VK_PRESENT_STATS (also GL): presented frames per second after the first (load) frame.
        if (presentTimes_.size() > 2) {
            const double s = std::chrono::duration<double>(presentTimes_.back() - presentTimes_[1]).count();
            std::fprintf(stderr, "ou: presents %zu in %.2f s = %.1f/s (gl)\n", presentTimes_.size() - 2, s,
                         double(presentTimes_.size() - 2) / s);
        }
        if (res_ && res_->fluidTimedFrames) {
            const double n = double(res_->fluidTimedFrames);
            std::fprintf(stderr, "ou: splash fluid GPU ms per drawing frame (%llu frames, gl): simulation %.3f, drawing %.3f, "
                         "total %.3f\n", (unsigned long long)res_->fluidTimedFrames, res_->fluidSimMs / n,
                         res_->fluidDrawMs / n, (res_->fluidSimMs + res_->fluidDrawMs) / n);
        }
        SDL_GL_MakeCurrent(window_, context_);
        if (fieldTimed_) std::fprintf(stderr, "ou: GPU ms per frame: wave field %.3f (gl, %zu frames)\n", fieldNs_ * 1e-6 / double(fieldTimed_), fieldTimed_);
        if (surfaceTimed_)
            std::fprintf(stderr, "ou: GPU ms per frame: water surface %.3f (gl, %s, %zu frames)\n", surfaceNs_ * 1e-6 / double(surfaceTimed_),
                         res_ && res_->waterTessProgram.get() ? "tessellated" : "per vertex", surfaceTimed_);
        if (fieldQueries_[0]) glDeleteQueries(GLsizei(fieldQueries_.size()), fieldQueries_.data());
        if (surfaceQueries_[0]) glDeleteQueries(GLsizei(surfaceQueries_.size()), surfaceQueries_.data());
        fieldQueries_ = {}, surfaceQueries_ = {};
        parts_.clear();
        res_.reset(); // GL objects must die while the context is current
        SDL_GL_DestroyContext(context_);
        context_ = nullptr;
    }

private:
    bool presentStats_ = std::getenv("OU_VK_PRESENT_STATS") != nullptr;
    std::vector<std::chrono::steady_clock::time_point> presentTimes_;
    std::array<GLuint, 3> fieldQueries_{}; // OU_VK_PRESENT_STATS: the wave field's GPU time (runWaterField)
    size_t fieldFrame_ = 0, fieldTimed_ = 0;
    double fieldNs_ = 0.0;
    std::array<GLuint, 3> surfaceQueries_{}; // OU_VK_PRESENT_STATS: the Enhanced water surface draws (drawWater)
    size_t surfaceFrame_ = 0, surfaceTimed_ = 0;
    double surfaceNs_ = 0.0;

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
        for (int i = 0; i < 2; ++i) {
            t.splash[i] = newTexture();
            glTextureStorage2D(t.splash[i].get(), 1, GL_RGBA32F, vp.width, vp.height);
            glTextureParameteri(t.splash[i].get(), GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTextureParameteri(t.splash[i].get(), GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTextureParameteri(t.splash[i].get(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(t.splash[i].get(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            t.splashFbo[i] = newFramebuffer();
            glNamedFramebufferTexture(t.splashFbo[i].get(), GL_COLOR_ATTACHMENT0, t.splash[i].get(), 0);
        }
        t.uwParticles = newTexture();
        glTextureStorage2D(t.uwParticles.get(), 1, GL_RGBA16F, vp.width, vp.height);
        glTextureParameteri(t.uwParticles.get(), GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(t.uwParticles.get(), GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(t.uwParticles.get(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(t.uwParticles.get(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        t.uwParticlesFbo = newFramebuffer();
        glNamedFramebufferTexture(t.uwParticlesFbo.get(), GL_COLOR_ATTACHMENT0, t.uwParticles.get(), 0);
        const int bw = std::max(1, vp.width / 2), bh = std::max(1, vp.height / 2);
        for (auto [tex, fbo] : {std::pair{&t.filterBlur, &t.filterFbo}, std::pair{&t.feedback, &t.feedbackFbo}}) {
            *tex = newTexture();
            glTextureStorage2D(tex->get(), std::min(6, 1 + int(std::log2(float(std::max(bw, bh))))), format, bw, bh);
            glTextureParameteri(tex->get(), GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTextureParameteri(tex->get(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(tex->get(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(tex->get(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            *fbo = newFramebuffer();
            glNamedFramebufferTexture(fbo->get(), GL_COLOR_ATTACHMENT0, tex->get(), 0);
        }
        t.reflection = newTexture();
        glTextureStorage2D(t.reflection.get(), 1, format, bw, bh);
        glTextureParameteri(t.reflection.get(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(t.reflection.get(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(t.reflection.get(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(t.reflection.get(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        t.reflectionDepth = newRenderbuffer(1, GL_DEPTH_COMPONENT32F, bw, bh);
        t.reflectionFbo = newFramebuffer();
        glNamedFramebufferTexture(t.reflectionFbo.get(), GL_COLOR_ATTACHMENT0, t.reflection.get(), 0);
        glNamedFramebufferRenderbuffer(t.reflectionFbo.get(), GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.reflectionDepth.get());
        for (size_t i = 0; i < 2; ++i) {
            t.shaft[i] = newTexture();
            glTextureStorage2D(t.shaft[i].get(), 1, GL_RGBA16F, (vp.width + 1) / 2, (vp.height + 1) / 2);
            glTextureParameteri(t.shaft[i].get(), GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTextureParameteri(t.shaft[i].get(), GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTextureParameteri(t.shaft[i].get(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(t.shaft[i].get(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            t.shaftFbo[i] = newFramebuffer();
            glNamedFramebufferTexture(t.shaftFbo[i].get(), GL_COLOR_ATTACHMENT0, t.shaft[i].get(), 0);
        }
        for (int i = -1; i < kUwBloomLevels; ++i) { // -1: uwHdr
            Texture& tex = i < 0 ? t.uwHdr : t.uwBloom[size_t(i)];
            Framebuffer& fbo = i < 0 ? t.uwHdrFbo : t.uwBloomFbo[size_t(i)];
            tex = newTexture();
            glTextureStorage2D(tex.get(), 1, GL_RGBA16F, std::max(1, vp.width >> (i + 1)), std::max(1, vp.height >> (i + 1)));
            glTextureParameteri(tex.get(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(tex.get(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(tex.get(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(tex.get(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            fbo = newFramebuffer();
            glNamedFramebufferTexture(fbo.get(), GL_COLOR_ATTACHMENT0, tex.get(), 0);
        }
    }

    // Opaque: depth write, LESS. Blended (the original's translucent pass 0x5066A0): no depth
    // write, LEQUAL, blend per cdc mode; the draw loops turn depth writes back on for modes 2-3 (SubMesh::depthWrite).
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
        glBindTextureUnit(0, usable ? it->second : res_->white.get());
        drawElements(s);
    }
    static void drawElements(const scene::SubMesh& s) {
        glDrawElements(GL_TRIANGLES, GLsizei(s.indexCount), GL_UNSIGNED_INT,
                       reinterpret_cast<const void*>(uintptr_t(s.firstIndex) * sizeof(uint32_t)));
    }
    // Submesh `s`'s cull state, then `draw`; two-sided blended ones as a back-face then a front-face draw (drawSidesApart).
    // `cullOff`: single draws never cull (screen readers: the sea is also seen from below).
    template <class Draw> static void drawSides(const scene::SubMesh& s, float op, bool cullBackFaces, Draw&& draw, bool cullOff = false) {
        if (drawSidesApart(s, op, cullBackFaces)) {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_FRONT);
            draw();
            glCullFace(GL_BACK);
        } else if (cullBackFaces && !s.doubleSided && !cullOff) {
            glEnable(GL_CULL_FACE);
        } else {
            glDisable(GL_CULL_FACE);
        }
        draw();
    }

    // ---- CORTAO ambient occlusion (docs/RENDERING.md "Ambient occlusion"; third_party/cortao, MIT) ----
    // sun.glsl ouAoMap = the temporal pass output (r AO, g view depth, b history age), render resolution, bottom-up.
    struct AoTemporalBlock { // cortao_temporal.comp OuAoTemporal
        glm::uvec4 info;
        glm::vec4 params;
    };
    GLintptr aoRange() const { return objectStrideFor(256); }

    // The pass programs (first use; the main pass per sample count: 16 rays, 32 at 32 samples, with the matching
    // occluder table), the occluder tables and the parameter UBO. false (once, logged): AO stays off.
    bool aoPrograms(int samples) {
        Resources::Ao& a = res_->ao;
        std::string error;
        auto fail = [&](const char* what) {
            std::fprintf(stderr, "ou: CORTAO ambient occlusion unavailable (%s: %s)\n", what, error.c_str());
            a.failed = true;
            return false;
        };
        if (!a.prepass.get()) {
            const std::string fs = std::string(kAoPrepassFsHead) + kCortaoCommon + kAoPrepassFsMain;
            if (!(a.prepass = linkProgram(kAoPrepassVs, fs.c_str(), error)).get()) return fail("depth pre-pass");
            if (!(a.cortaoPrepass = linkCompute(cortaoSource(kCortaoPrepassCs, "").c_str(), error)).get()) return fail("prepass");
            const std::string filter = cortaoSource(kCortaoFilterCs, "#define CORTAO_LARGE_FILTER 1\n#define CORTAO_FILTER_SYMMETRY 1\n");
            if (!(a.filter = linkCompute(filter.c_str(), error)).get()) return fail("filter pass");
            const std::string temporal = std::string("#version 460 core\n") + kCortaoTemporalCs;
            if (!(a.temporal = linkCompute(temporal.c_str(), error)).get()) return fail("temporal pass");
            static_assert(sizeof kCortaoOccluders16 == 24 * 24 * 16 * 2 && sizeof kCortaoOccluders32 == 24 * 24 * 16 * 4);
            auto table = [](GLenum format, GLenum type, const void* data) {
                GLuint id = 0;
                glCreateTextures(GL_TEXTURE_3D, 1, &id);
                glTextureStorage3D(id, 1, format, 24, 24, 16);
                glTextureSubImage3D(id, 0, 0, 0, 0, 24, 24, 16, GL_RED_INTEGER, type, data);
                glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                for (GLenum w : {GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R}) glTextureParameteri(id, w, GL_CLAMP_TO_EDGE);
                return Texture(id);
            };
            a.occluders16 = table(GL_R16UI, GL_UNSIGNED_SHORT, kCortaoOccluders16);
            a.occluders32 = table(GL_R32UI, GL_UNSIGNED_INT, kCortaoOccluders32);
            a.params = newBuffer();
            glNamedBufferStorage(a.params.get(), 4 * aoRange(), nullptr, GL_DYNAMIC_STORAGE_BIT);
        }
        if (!a.main.contains(samples)) {
            const std::string defines = "#define CORTAO_SAMPLE_COUNT " + std::to_string(samples) + "\n#define CORTAO_RAY_COUNT " +
                                        std::to_string(cortao::rayCount(samples)) + "\n#define CORTAO_OCCLUDER_LOOKUP_TABLE 1\n";
            Program p = linkCompute(cortaoSource(kCortaoMainCs, defines).c_str(), error);
            if (!p.get()) return fail("main pass");
            a.main.emplace(samples, std::move(p));
        }
        return true;
    }

    // Render-resolution AO targets: pre-pass depth + RG16F normal + RGBA16F motion, the R32UI packed buffer with its
    // mips, the R8 main / filter outputs and the RGBA16F history pair. Rebuilt with the viewport (history dropped).
    void ensureAoTargets(Viewport vp) {
        Resources::Ao& a = res_->ao;
        if (a.width == vp.width && a.height == vp.height) return;
        a.width = vp.width, a.height = vp.height;
        a.mips = int(cortao::mipCount(uint32_t(vp.width), uint32_t(vp.height)));
        auto make = [&](GLenum format, int levels = 1) {
            Texture t = newTexture();
            glTextureStorage2D(t.get(), levels, format, vp.width, vp.height);
            glTextureParameteri(t.get(), GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
            glTextureParameteri(t.get(), GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTextureParameteri(t.get(), GL_TEXTURE_MAX_LEVEL, levels - 1);
            glTextureParameteri(t.get(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER); // CORTAO: border "colour" 0 (sky)
            glTextureParameteri(t.get(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
            return t;
        };
        a.depth = make(GL_DEPTH_COMPONENT32F);
        a.normal = make(GL_RG16F);
        a.motion = make(GL_RGBA16F); // uv offset to last frame, last / this view depth, 1 = surface
        a.packed = make(GL_R32UI, a.mips);
        const GLuint zero[4] = {};
        glTextureParameterIuiv(a.packed.get(), GL_TEXTURE_BORDER_COLOR, zero);
        a.raw = make(GL_R8);
        a.filtered = make(GL_R8);
        for (Texture& h : a.history) h = make(GL_RGBA16F);
        a.fbo = newFramebuffer();
        glNamedFramebufferTexture(a.fbo.get(), GL_DEPTH_ATTACHMENT, a.depth.get(), 0);
        glNamedFramebufferTexture(a.fbo.get(), GL_COLOR_ATTACHMENT0, a.normal.get(), 0);
        glNamedFramebufferTexture(a.fbo.get(), GL_COLOR_ATTACHMENT1, a.motion.get(), 0);
        const GLenum targets[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glNamedFramebufferDrawBuffers(a.fbo.get(), 2, targets);
        if (glCheckNamedFramebufferStatus(a.fbo.get(), GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            std::fprintf(stderr, "[gl] AO pre-pass framebuffer incomplete\n");
        a.historyValid = false;
    }

    // FrameOptions::ao (Rt too: no ray tracing on GL): the AO depth pre-pass over the surfaces that write depth in the
    // main pass (opaque and depth-writing alpha-tested submeshes, not the exterior or Enhanced water), then the CORTAO
    // prepass / main / filter passes and the temporal accumulation into history[current]. Full render resolution, one
    // sample per pixel (MSAA draws match it by their pixel's view depth, sun.glsl ouAo). false: no map this frame.
    bool runAmbientOcclusion(const Camera& camera, Viewport vp, const glm::mat4& viewProj, const FrameOptions& o) {
        Resources& r = *res_;
        Resources::Ao& a = r.ao;
        if (o.ao == AmbientOcclusion::Off || a.failed) {
            a.historyValid = false;
            return false;
        }
        const int samples = cortao::sampleCount(o.aoSamples);
        if (!aoPrograms(samples)) return false;
        ensureAoTargets(vp);
        if (samples != a.samples || a.upload != uploads_) a.historyValid = false; // look change / new scene
        a.samples = samples, a.upload = uploads_;
        const int w = vp.width, h = vp.height;
        const glm::mat4 view = camera.view(), proj = camera.projection(float(w) / float(h));

        glBindFramebuffer(GL_FRAMEBUFFER, a.fbo.get());
        glViewport(0, 0, w, h);
        scissorFor(UINT32_MAX, vp);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        const float noNormal[4] = {}, far = 1.0f;
        glClearNamedFramebufferfv(a.fbo.get(), GL_COLOR, 0, noNormal);
        glClearNamedFramebufferfv(a.fbo.get(), GL_COLOR, 1, noNormal);
        glClearNamedFramebufferfv(a.fbo.get(), GL_DEPTH, 0, &far);
        glUseProgram(a.prepass.get());
        glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(viewProj));
        glUniformMatrix4fv(2, 1, GL_FALSE, glm::value_ptr(view));
        glUniformMatrix4fv(4, 1, GL_FALSE, glm::value_ptr(a.historyValid ? a.prevViewProj : viewProj));
        const bool enhancedWater = o.water == WaterQuality::Enhanced;
        uint32_t bound = UINT32_MAX;
        for (size_t i = exteriorCount(r.meshes, std::span<const DrawRef>(r.order)); i < r.order.size(); ++i) {
            const DrawRef& d = r.order[i];
            const GpuMesh& m = r.meshes[d.mesh];
            const scene::SubMesh& s = m.submeshes[d.sub];
            const float op = d.mesh < opacity_.size() ? opacity_[d.mesh] : 1.0f;
            const scene::Blend blend = drawBlend(s, op);
            if (!(blend == scene::Blend::Opaque || (blend == scene::Blend::Alpha && drawDepthWrite(s, op)))) continue;
            if (enhancedWater && waterDraw(d.mesh, d.sub)) continue;
            scissorFor(d.mesh, vp);
            if (d.mesh != bound) {
                glUniformMatrix4fv(1, 1, GL_FALSE, glm::value_ptr(m.model));
                glBindVertexArray(m.vao.get());
                const bool moved = m.movedFrame == drawFrames_ && m.prevVbo.get();
                glVertexArrayVertexBuffer(m.vao.get(), 1, moved ? m.prevVbo.get() : m.vbo.get(), 0, sizeof(scene::Vertex));
                bound = d.mesh;
            }
            if (o.cullBackFaces && !s.doubleSided) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
            glUniform1f(3, s.alphaRef / 255.0f);
            drawSubmesh(s);
        }
        scissorFor(UINT32_MAX, vp);

        uint32_t seed = ++a.frame * 0x9E3779B9u; // a new pattern every frame; the temporal pass averages them
        seed = (seed ^ (seed >> 16)) * 0x7FEB352Du;
        // GL: texture rows and NDC y both point up (flipY false); GL's [-1, 1] clip z gives D3D's window depth
        const cortao::Params cp = cortao::params(uint32_t(w), uint32_t(h), seed ^ (seed >> 15), proj, camera.nearZ, camera.farZ, false);
        const AoTemporalBlock temporal{{uint32_t(w), uint32_t(h), a.historyValid ? 1u : 0u, (1u << cortao::kPackedNormalBits) - 1u},
                                       {0.06f, 0.03f, 1.5f, 40.0f}}; // min blend, depth tolerance, clamp sigmas, max age
        const auto& pre = cp.prepass;
        const auto& main = cp.main;
        const auto& filter = cp.filter;
        const GLintptr range = aoRange();
        glNamedBufferSubData(a.params.get(), 0, sizeof pre, &pre);
        glNamedBufferSubData(a.params.get(), range, sizeof main, &main);
        glNamedBufferSubData(a.params.get(), 2 * range, sizeof filter, &filter);
        glNamedBufferSubData(a.params.get(), 3 * range, sizeof temporal, &temporal);
        auto pass = [&](GLuint program, int block, GLsizeiptr size, GLuint gx, GLuint gy) {
            glUseProgram(program);
            glBindBufferRange(GL_UNIFORM_BUFFER, 6, a.params.get(), block * range, std::max<GLsizeiptr>(size, 16));
            glDispatchCompute(gx, gy, 1);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        };
        auto groups = [](int n, int size) { return GLuint((n + size - 1) / size); };
        glBindTextureUnit(0, a.depth.get());
        glBindTextureUnit(1, a.normal.get());
        for (GLuint i = 0; i < 8; ++i) // levels past mipCount are never written
            glBindImageTexture(i, a.packed.get(), std::min(GLint(i), a.mips - 1), GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
        pass(a.cortaoPrepass.get(), 0, sizeof pre, groups(w, 8), groups(h, 8));
        glBindTextureUnit(0, a.packed.get());
        glBindTextureUnit(2, cortao::rayCount(samples) == 32 ? a.occluders32.get() : a.occluders16.get());
        glBindImageTexture(0, a.raw.get(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
        pass(a.main.at(samples).get(), 1, sizeof main, groups(w, 16), groups(h, 16));
        glBindTextureUnit(1, a.raw.get());
        glBindImageTexture(0, a.filtered.get(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
        pass(a.filter.get(), 2, 16, groups((w + 1) / 2, 8), groups((h + 1) / 2, 8)); // a thread per 2x2 block
        a.current ^= 1;
        glBindTextureUnit(1, a.filtered.get());
        glBindTextureUnit(2, a.history[size_t(a.current ^ 1)].get());
        glBindTextureUnit(3, a.motion.get());
        glBindImageTexture(0, a.history[size_t(a.current)].get(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        pass(a.temporal.get(), 3, sizeof temporal, groups(w, 8), groups(h, 8));
        a.prevViewProj = viewProj;
        a.historyValid = true;
        return true;
    }

    // One atlas tile per cascade. Opaque and alpha-blended surfaces cast (blended ones alpha-tested at 50 %);
    // additive/average effects do not. Both faces, slope-scaled bias. Meshes whose bounds miss a cascade are skipped.
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
                if (!lodShown(m, lodEye_) || outsideOrtho(sun.lightViewProj[c], m.center, m.radius)) continue;
                glUniformMatrix4fv(1, 1, GL_FALSE, glm::value_ptr(m.model));
                glBindVertexArray(m.vao.get());
                for (const auto& s : m.submeshes) {
                    if ((s.blend != scene::Blend::Opaque && s.blend != scene::Blend::Alpha) || !s.castsShadow) continue;
                    glUniform1f(3, (s.blend == scene::Blend::Alpha ? std::max<int>(s.alphaRef, 128) : s.alphaRef) / 255.0f);
                    drawSubmesh(s);
                }
            }
        }
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    // Time of day backdrop (Sky::backdropPass, shaders/sky.*): `modes` bit 0 = the tint (the frame so far: opaque
    // exterior, the clear colour, x the backdrop tint), bit 1 = the stars and the moon, added under the star coverage in
    // alpha (`exterior`: kept from the opaque exterior's painted scenery and the blended exterior drawn between the two
    // passes; else cleared by the tint). Right after the opaque exterior, or first without exterior. Leaves blending,
    // depth writes and the program changed. false = no pass.
    bool drawSky(const Camera& camera, Viewport vp, const FrameOptions& o, int modes, bool exterior) {
        if (!o.sky || !o.sky->backdropPass()) return false;
        Resources& r = *res_;
        glUseProgram(r.skyProgram.get());
        glBindVertexArray(r.emptyVao.get());
        glBindBufferBase(GL_UNIFORM_BUFFER, 5, r.skyUbo.get());
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        const float aspect = float(vp.width) / float(std::max(vp.height, 1));
        for (int mode = 0; mode < 2; ++mode) {
            if (!(modes & (1 << mode)) || (mode == 1 && o.sky->stars <= 0 && o.sky->moon <= 0)) continue;
            SkyPush push = skyPush(camera, aspect, *o.sky, mode, o.time);
            push.stars.w = exterior ? 1.0f : 0.0f; // the tint keeps / clears the coverage (sky.frag)
            glNamedBufferSubData(r.skyUbo.get(), 0, sizeof push, &push);
            if (mode == 0) glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE), glBlendFunc(GL_DST_COLOR, GL_ZERO); // dst x tint
            else glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE), glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE); // + stars, moon
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        return true;
    }

    // tru.exe's PSSM atlas (original::Pssm): one 1024² tile per split, the casters of the shadow map above, D3D9 depth
    // bias (in the split's matrix) and slope bias. D3D clip -> GL: y flipped (rows top-down as tru.exe's lookups count
    // them), z [0, 1] -> [-1, 1].
    void drawPssm(const original::Pssm& p) {
        Resources& r = *res_;
        glBindFramebuffer(GL_FRAMEBUFFER, r.pssmFbo.get());
        glDisable(GL_SCISSOR_TEST);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        const float one = 1.0f;
        glClearNamedFramebufferfv(r.pssmFbo.get(), GL_DEPTH, 0, &one);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(p.slopeBias, 0.0f);
        glUseProgram(r.shadowProgram.get());
        glm::mat4 toGl(1.0f);
        toGl[1][1] = -1.0f, toGl[2][2] = 2.0f, toGl[3][2] = -1.0f;
        for (int k = 0; k < original::kPssmSplits; ++k) {
            const glm::mat4 vp = toGl * p.tileClip[k];
            glViewport(p.tile[k].x, p.tile[k].y, original::kPssmTile, original::kPssmTile);
            glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(vp));
            for (const auto& m : r.meshes) {
                if (!lodShown(m, lodEye_) || outsideOrtho(vp, m.center, m.radius)) continue;
                glUniformMatrix4fv(1, 1, GL_FALSE, glm::value_ptr(m.model));
                glBindVertexArray(m.vao.get());
                for (const auto& s : m.submeshes) {
                    if ((s.blend != scene::Blend::Opaque && s.blend != scene::Blend::Alpha) || !s.castsShadow) continue;
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
        glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(id, GL_TEXTURE_WRAP_S, t.cube ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glTextureParameteri(id, GL_TEXTURE_WRAP_T, t.cube ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        if (maxAniso_ > 1.0f) glTextureParameterf(id, GL_TEXTURE_MAX_ANISOTROPY, maxAniso_);
        return tex;
    }

    // GPU buffers of `m` into `g` (submeshes, bounds, water flag): false = not drawable (warned).
    bool createMesh(const scene::MeshData& m, GpuMesh& g, std::vector<std::string>& warnings) {
        auto warn = [&](const std::string& w) { warnings.push_back("mesh (" + m.origin + "): " + w); };
        if (m.vertices.empty() || m.indices.empty()) { warn("empty"); return false; }
        if (std::any_of(m.indices.begin(), m.indices.end(), [&](uint32_t i) { return i >= m.vertices.size(); })) {
            warn("index out of range, skipped");
            return false;
        }
        for (const auto& s : m.submeshes) {
            if (uint64_t(s.firstIndex) + s.indexCount > m.indices.size()) warn("submesh range out of bounds, skipped");
            else g.submeshes.push_back(s);
        }
        if (m.submeshes.empty()) g.submeshes.push_back({0, uint32_t(m.indices.size()), 0});
        g.lightBoxes = m.lightBoxes;
        g.model = glm::make_mat4(m.transform);
        const glm::vec4 sphere = original::boundingSphere(m.vertices, g.model);
        g.center = glm::vec3(sphere), g.radius = sphere.w;
        setLod(g, m);
        g.bounds = meshBounds(m);
        g.water = isWaterSurface(m.origin);
        g.vertexCount = m.vertices.size();
        g.indexCount = m.indices.size();
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
        // The AO pre-pass's previous position (kAoPrepassVs aPrevPos): binding 1 = this buffer, or prevVbo while moving.
        glVertexArrayVertexBuffer(vao, 1, g.vbo.get(), 0, sizeof(scene::Vertex));
        glEnableVertexArrayAttrib(vao, 8);
        glVertexArrayAttribFormat(vao, 8, 3, GL_FLOAT, GL_FALSE, GLuint(offsetof(scene::Vertex, position)));
        glVertexArrayAttribBinding(vao, 8, 1);
        return true;
    }

    // A resident mesh listed under a new transform (a part sent in its unit frame, then moved by its stream offset).
    static void place(GpuMesh& g, const scene::MeshData& m) {
        const glm::mat4 model = glm::make_mat4(m.transform);
        if (model == g.model) return;
        g.model = model, g.exteriorShift = glm::vec3(0.0f);
        const glm::vec4 sphere = original::boundingSphere(m.vertices, g.model);
        g.center = glm::vec3(sphere), g.radius = sphere.w;
        setLod(g, m);
        g.bounds = meshBounds(m);
    }

    // The per-upload state of a mesh: its submeshes' programs, material blocks and samplers, reflection plane.
    void bindOriginals(GpuMesh& g, const scene::MeshData& m) {
        g.program.clear(), g.materialOffset.clear(), g.samplers.clear(), g.screenSamplers.clear(), g.reflection.clear();
        g.reflective.clear();
        g.irradiance = false;
        for (const auto& s : g.submeshes) addOriginal(g, s);
        g.reflectZ = original::reflectionPlaneZ(m, g.model, prepared_.materials);
        g.waterRestZ = waterRestZ(m, prepared_.materials);
        for (const auto& s : g.submeshes) {
            const auto it = prepared_.materials.find(s.materialId);
            g.reflective.push_back(it != prepared_.materials.end() && original::reflectionSamplers(*it->second) != 0);
        }
    }

    // Sends the parts' keyed content not resident yet, about `budget` bytes per call (drawFrame: spread over frames), and
    // claims what is already resident for the part. A finished part drops its SceneData.
    static constexpr size_t kPartBytesPerFrame = size_t(8) << 20;
    void pumpParts(size_t budget, std::vector<std::string>& warnings) {
        Resources& r = *res_;
        size_t sent = 0;
        for (auto& [id, p] : parts_) {
            if (!p.data) continue;
            const scene::SceneData& s = *p.data;
            for (; p.texture < s.textures.size() && sent < budget; ++p.texture) {
                const scene::TextureData& t = s.textures[p.texture];
                if (!t.key) continue;
                if (const auto it = r.resident.find(t.key); it != r.resident.end()) {
                    it->second.part = id;
                    continue;
                }
                Texture tex = uploadTexture(t, warnings);
                for (const auto& mip : t.mips) sent += mip.data.size();
                if (tex.get()) r.resident.emplace(t.key, Resources::ResidentTexture{std::move(tex), t.cube, id});
            }
            for (; p.texture == s.textures.size() && p.mesh < s.meshes.size() && sent < budget; ++p.mesh) {
                const scene::MeshData& m = s.meshes[p.mesh];
                if (!m.key) continue;
                if (const auto it = r.parked.find(m.key); it != r.parked.end()) {
                    it->second.part = id;
                    continue;
                }
                const auto cur = std::find_if(r.meshes.begin(), r.meshes.end(), [&](const GpuMesh& g) { return g.key == m.key; });
                if (cur != r.meshes.end()) {
                    cur->part = id;
                    continue;
                }
                GpuMesh g;
                if (!createMesh(m, g, warnings)) continue;
                sent += m.vertices.size() * sizeof(scene::Vertex) + m.indices.size() * sizeof(uint32_t);
                g.key = m.key, g.part = id;
                r.parked.emplace(m.key, std::move(g));
            }
            if (p.texture == s.textures.size() && p.mesh == s.meshes.size()) p.data.reset();
        }
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
            // + cascaded sun shadow. Linked once per source pair while uploads use it (a unit joining relinked them all).
            std::string ps = originalPixelSource(p.psGlsl.source, false, false);
            auto& l = r.linked[{p.vsGlsl.source, ps}];
            if (!l.upload) l.program = linkProgram(p.vsGlsl.source.c_str(), ps.c_str(), l.error);
            l.upload = uploads_;
            r.originals.push_back(l.program.get());
            if (!l.program.get()) warnings.push_back("original program vs " + hex(p.vs) + " ps " + hex(p.ps) + ": " + l.error);
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
        if (p >= 0 && !r.originals[size_t(p)]) p = -1;
        g.program.push_back(p);
        g.materialOffset.push_back(0);
        g.samplers.emplace_back();
        g.screenSamplers.push_back(0);
        g.reflection.push_back(0);
        if (p < 0) return;
        const scene::Material& mat = *prepared_.materials.at(s.materialId);
        const auto stride = size_t(objectStrideFor(sizeof(sm3::MaterialBlock)));
        g.materialOffset.back() = GLintptr(materialBlocks_.size());
        materialBlocks_.resize(materialBlocks_.size() + stride);
        original::fillMaterial(*reinterpret_cast<sm3::MaterialBlock*>(&materialBlocks_[size_t(g.materialOffset.back())]), mat, s);
        const auto& prog = prepared_.programs[size_t(p)];
        g.irradiance |= prog.psGlsl.readsIrradiance;
        for (int n = 0; n < 16; ++n) {
            if (!(prog.psGlsl.samplers >> n & 1)) continue;
            // Unbound cube samplers read black (D3D9 without a texture), others white; the type must match the sampler.
            const bool cube = prog.psGlsl.samplerType[n] == 3;
            GLuint tex = cube ? r.blackCube.get() : r.white.get();
            if (n == original::kAttenuationSampler && !cube) tex = r.attenuation.get();
            if (n == original::kSunCookieSampler && !cube && environment_.sunCookie)
                if (auto it = r.textures.find(environment_.sunCookie); it != r.textures.end() && !cubes_.contains(it->first))
                    tex = it->second;
            if (const uint32_t id = original::samplerTexture(mat, s.instanceTextures, n))
                if (auto it = r.textures.find(id); it != r.textures.end() && cubes_.contains(id) == cube) tex = it->second;
            if (prog.psGlsl.samplerType[n] == sm3::kSamplerCookies) tex = r.cookies.get();
            if (prog.psGlsl.samplerType[n] == sm3::kSamplerShadow) tex = r.pssmMap.get();
            g.samplers.back()[size_t(n)] = tex;
        }
        g.screenSamplers.back() = original::screenSamplers(mat, s.instanceTextures, prog.psGlsl.readsDepth);
        g.reflection.back() = original::reflectionSamplers(mat);
    }

    // Submeshes whose materials sample the back / depth buffer (original::screenSamplers; the sea): copy the scene drawn
    // so far into the single-sample textures, then draw them in draw order with those bound.
    void drawScreenReaders(Viewport vp, bool cullBackFaces) {
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
            glUseProgram(r.originals[size_t(m.program[d.sub])]);
            glBindVertexArray(m.vao.get());
            scissorFor(d.mesh, vp);
            const float op = d.mesh < opacity_.size() ? opacity_[d.mesh] : 1.0f;
            const scene::Blend blend = drawBlend(s, op);
            if (int(blend) != state) setBlend(state = int(blend));
            glBindBufferRange(GL_UNIFORM_BUFFER, 2, r.objectUbo.get(), GLintptr(objectSlot(m, d)) * objectStride(), sizeof(sm3::ObjectBlock));
            glBindBufferRange(GL_UNIFORM_BUFFER, 1, r.materialUbo.get(), m.materialOffset[d.sub], sizeof(sm3::MaterialBlock));
            auto tex = originalTextures(m, d);
            for (int n = 0; n < 16; ++n) {
                if (m.screenSamplers[d.sub] >> n & 1) tex[size_t(n)] = t.opaqueColor.get();
                if (m.screenSamplers[d.sub] >> (16 + n) & 1) tex[size_t(n)] = t.opaqueDepth.get();
            }
            glBindTextures(0, 16, tex.data());
            drawSides(s, op, cullBackFaces, [&] { drawElements(s); }, true);
        }
        scissorFor(UINT32_MAX, vp);
    }

    // Object block of an original draw: reflecting submeshes take the second half (beginOriginals) while a reflection
    // was drawn this frame; their reflection samplers read Targets::reflection.
    // Reflecting draws keep the mesh's light set [I: no reflecting terrain with point lights seen].
    uint32_t objectSlot(const GpuMesh& m, const DrawRef& d) const {
        return reflecting_ && m.reflection[d.sub] ? uint32_t(res_->meshes.size()) + d.mesh : litSlot(m, d);
    }
    uint32_t litSlot(const GpuMesh& m, const DrawRef& d) const { // the mesh's block, or its terrain light group's
        const uint16_t g = m.submeshes[d.sub].lightGroup;
        return g < m.lightBoxes.size() && m.lightSlot + g < objectSlots_ ? m.lightSlot + g : d.mesh;
    }
    std::array<GLuint, 16> originalTextures(const GpuMesh& m, const DrawRef& d) const {
        auto tex = m.samplers[d.sub];
        if (reflecting_)
            for (int n = 0; n < 16; ++n)
                if (m.reflection[d.sub] >> n & 1) tex[size_t(n)] = res_->targets.reflection.get();
        return tex;
    }

    // Planar reflection (original::reflection): the plane of the nearest drawn reflecting submesh, when the camera is
    // above it; the opaque and blended scene except the water surfaces and screen readers, from the mirrored camera,
    // into Targets::reflection. Returns the reflection (reflecting_) or nullopt.
    std::optional<original::Reflection> drawReflection(const Camera& camera, Viewport vp, const FrameOptions& options) {
        Resources& r = *res_;
        reflecting_ = false;
        if (!options.originalShaders || r.originals.empty()) return std::nullopt;
        float planeZ = 0, best = std::numeric_limits<float>::max();
        for (const DrawRef& d : r.order) {
            const GpuMesh& m = r.meshes[d.mesh];
            if (m.program[d.sub] < 0 || !m.reflection[d.sub]) continue;
            const float dist = glm::distance(m.center, camera.position) - m.radius;
            if (dist < best) best = dist, planeZ = m.reflectZ;
        }
        if (best == std::numeric_limits<float>::max() || camera.position.z <= planeZ) return std::nullopt;
        Targets& t = r.targets;
        const Viewport half{std::max(1, vp.width / 2), std::max(1, vp.height / 2)};
        const auto refl = original::reflection(camera, float(vp.width) / float(vp.height), planeZ, true);
        const glm::mat4 viewProj = refl.camera.projection(float(vp.width) / float(vp.height)) * refl.camera.view();
        glBindFramebuffer(GL_FRAMEBUFFER, t.reflectionFbo.get());
        glViewport(0, 0, half.width, half.height);
        glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        scissorClip_ = -1;
        beginOriginals(refl.camera, half, options);
        int state = -1, program = -2;
        const size_t exterior = exteriorCount(r.meshes, std::span<const DrawRef>(r.order));
        const size_t skyAt = exteriorBlendStart(r.meshes, std::span<const DrawRef>(r.order), exterior,
                                                [&](uint32_t m) { return m < opacity_.size() ? opacity_[m] : 1.0f; });
        if (!exterior && drawSky(refl.camera, half, options, 3, false)) program = -3;
        for (const DrawRef& d : r.order) {
            const size_t i = size_t(&d - r.order.data());
            if (exterior && i == skyAt && skyAt < exterior) { // the tint after the opaque exterior (drawSky)
                if (drawSky(refl.camera, half, options, 1, true)) program = -3;
                state = -1;
            }
            if (exterior && i == exterior) { // exterior drawn: behind everything else
                glDepthMask(GL_TRUE);
                glClear(GL_DEPTH_BUFFER_BIT);
                if (drawSky(refl.camera, half, options, skyAt < exterior ? 2 : 3, true)) program = -3;
                state = -1;
            }
            const GpuMesh& m = r.meshes[d.mesh];
            const scene::SubMesh& s = m.submeshes[d.sub];
            const int p = m.program[d.sub];
            if (m.water || (p >= 0 && (m.screenSamplers[d.sub] || m.reflection[d.sub]))) continue;
            const float op = d.mesh < opacity_.size() ? opacity_[d.mesh] : 1.0f;
            if (p != program) {
                glUseProgram(p < 0 ? r.sceneProgram.get() : r.originals[size_t(p)]);
                if (p < 0) glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(viewProj)), glUniform1i(2, options.lighting ? 1 : 0);
                program = p;
            }
            glBindVertexArray(m.vao.get());
            const scene::Blend blend = drawBlend(s, op);
            if (int(blend) != state) setBlend(state = int(blend));
            if (blend == scene::Blend::Alpha) glDepthMask(drawDepthWrite(s, op) ? GL_TRUE : GL_FALSE);
            if (p >= 0) {
                glBindBufferRange(GL_UNIFORM_BUFFER, 2, r.objectUbo.get(), GLintptr(litSlot(m, d)) * objectStride(), sizeof(sm3::ObjectBlock));
                glBindBufferRange(GL_UNIFORM_BUFFER, 1, r.materialUbo.get(), m.materialOffset[d.sub], sizeof(sm3::MaterialBlock));
                glBindTextures(0, 16, m.samplers[d.sub].data());
                drawSides(s, op, options.cullBackFaces, [&] { drawElements(s); });
                continue;
            }
            glUniformMatrix4fv(1, 1, GL_FALSE, glm::value_ptr(m.model));
            glUniform1f(3, s.alphaRef / 255.0f);
            glUniform2f(4, op, s.blend == scene::Blend::Opaque ? 1.0f : 0.0f);
            drawSides(s, op, options.cullBackFaces, [&] { drawSubmesh(s); });
        }
        setBlend(int(scene::Blend::Opaque));
        reflecting_ = true;
        return refl;
    }

    // Portal clip of GPU mesh `mesh` (FrameOptions::unitClips via meshClip_) as the GL scissor; UINT32_MAX: off.
    void scissorFor(uint32_t mesh, Viewport vp) {
        const int c = mesh < meshClip_.size() ? meshClip_[mesh] : -1;
        if (c == scissorClip_) return;
        scissorClip_ = c;
        if (c < 0) { glDisable(GL_SCISSOR_TEST); return; }
        const PixelRect p = clipPixels(unitClips_[size_t(c)], vp.width, vp.height, false);
        glEnable(GL_SCISSOR_TEST);
        glScissor(p.x, p.y, p.width, p.height);
    }
    bool clippedAway(size_t mesh) const { return meshClip_[mesh] >= 0 && clipHidden(unitClips_[size_t(meshClip_[mesh])]); }

    GLintptr objectStrideFor(size_t bytes) const {
        const GLintptr a = res_->uboAlign;
        return (GLintptr(bytes) + a - 1) / a * a;
    }

    void finishOriginals() {
        Resources& r = *res_;
        r.materialUbo = {};
        r.objectUbo = {};
        objectSlots_ = 0;
        if (materialBlocks_.empty()) return;
        r.materialUbo = newBuffer();
        glNamedBufferStorage(r.materialUbo.get(), GLsizeiptr(materialBlocks_.size()), materialBlocks_.data(), 0);
        r.objectUbo = newBuffer();
        // Second half: the same blocks with the planar reflection's InstanceParams, bound by the reflecting draws. Then
        // one block per terrain light group (its own light set).
        objectSlots_ = 2 * r.meshes.size();
        for (auto& g : r.meshes) g.lightSlot = uint32_t(objectSlots_), objectSlots_ += g.lightBoxes.size();
        glNamedBufferStorage(r.objectUbo.get(), GLsizeiptr(objectSlots_) * objectStride(), nullptr, GL_DYNAMIC_STORAGE_BIT);
        materialBlocks_.clear();
        materialBlocks_.shrink_to_fit();
    }

    // `reflection`: the planar reflection the reflecting draws sample (their second-half object blocks), or none.
    void beginOriginals(const Camera& camera, Viewport vp, const FrameOptions& o, const original::Reflection* reflection = nullptr) {
        Resources& r = *res_;
        sm3::FrameBlock f;
        original::fillFrame(f, camera, vp, o.time, environment_, true, o.underwater, cookieIds_, o.fog, o.sky); // screen copies are bottom-up
        if (const auto row = original::fogLutRow(o.fog); row != fogRow_) // the fog curve (Fog) changed: its LUT row
            fogRow_ = row, glTextureSubImage2D(r.attenuation.get(), 0, 0, original::kFogLutRow, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, row.data());
        original::fillPssm(f, pssm_, pssmCamera_);
        irradiance_.assign(environment_, r.meshes);
        irradiance_.fill(f, o.sky ? o.sky->probes : nullptr);
        glNamedBufferSubData(r.frameUbo.get(), 0, sizeof f, &f);
        const glm::mat4 vpD3D = original::viewProjD3D(camera, float(vp.width) / float(vp.height));
        const size_t stride = size_t(objectStride()), n = r.meshes.size();
        objectScratch_.assign(std::max(objectSlots_, 2 * n) * stride, 0);
        for (size_t i = 0; i < n; ++i) {
            auto& block = *reinterpret_cast<sm3::ObjectBlock*>(&objectScratch_[i * stride]);
            original::fillObject(block, vpD3D, r.meshes[i].model,
                                 original::lightSet(environment_, r.meshes[i].center, r.meshes[i].radius, r.meshes[i].lightMask, &camera.position),
                                 i < opacity_.size() ? opacity_[i] : 1.0f);
            original::fillWaves(block, o.waterVolumes, r.meshes[i].center, r.meshes[i].radius);
            block.irradiance = irradiance_.slot(i);
            const GpuMesh& g = r.meshes[i];
            for (size_t k = 0; k < g.lightBoxes.size() && g.lightSlot + k < objectSlots_; ++k) {
                auto& group = *reinterpret_cast<sm3::ObjectBlock*>(&objectScratch_[(g.lightSlot + k) * stride]);
                group = block;
                std::memcpy(group.lightSet, original::lightSet(environment_, g.lightBoxes[k], g.model, g.lightMask, &camera.position).packed,
                            sizeof group.lightSet);
            }
            if (!reflection || std::isnan(r.meshes[i].reflectZ)) continue;
            auto& lit = *reinterpret_cast<sm3::ObjectBlock*>(&objectScratch_[(n + i) * stride]);
            lit = block;
            std::memcpy(lit.inst, reflection->rows, sizeof lit.inst);
        }
        if (r.objectUbo.get())
            glNamedBufferSubData(r.objectUbo.get(), 0, GLsizeiptr(objectScratch_.size()), objectScratch_.data());
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, r.frameUbo.get());
    }

    // FFT ocean (render/Ocean.hpp): spectrum, row and column inverse DFTs, assemble into uOcean (texture unit 12), then
    // grid 64's doubling into level 0 and the mip chain below the grid (Ocean.hpp Pass; image unit 0 source, 1 target).
    void runOcean(const FrameOptions& o) {
        Resources& r = *res_;
        ocean::Push p = r.oceanClock.frame(o);
        const int n = p.cfg.y, lod = ocean::gridLod(n);
        glUseProgram(r.oceanProgram.get());
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, r.oceanSpectrum.get(), GLintptr(ocean::spectrumOffset(n) * sizeof(glm::vec4)),
                          GLsizeiptr(size_t(ocean::kCascades) * size_t(n * n) * sizeof(glm::vec4)));
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, r.oceanWork.get());
        glUniform4fv(1, 1, glm::value_ptr(p.par));
        glUniform4fv(2, ocean::kCascades, glm::value_ptr(p.cas[0]));
        auto run = [&](int src, int dst, int pass, int size, bool lines) {
            glBindImageTexture(0, r.ocean.get(), src, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA16F);
            glBindImageTexture(1, r.ocean.get(), dst, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            p.cfg.x = pass, p.cfg.y = size;
            glUniform4iv(0, 1, glm::value_ptr(p.cfg));
            glDispatchCompute(lines ? GLuint(size) : GLuint((size * size + ocean::kThreads - 1) / ocean::kThreads), 1, ocean::kCascades);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
        };
        for (int pass : {ocean::Spectrum, ocean::Rows, ocean::Columns, ocean::Assemble})
            run(lod, lod + 1, pass, n, pass == ocean::Rows || pass == ocean::Columns);
        if (lod > 0) run(1, 0, ocean::Up, ocean::kMaxN, false);
        for (int l = lod; l < ocean::kLevels - 1; ++l) run(l, l + 1, ocean::Mip, ocean::kMaxN >> (l + 1), false);
    }

    // The interactive wave field (WaterField.hpp Pass, one dispatch each) after the ocean (rough-sea damping reads
    // uOcean, unit 12): image unit 0 = uRipples. False: the field rests (or has no program).
    bool runWaterField(const FrameOptions& o, const WaterBlock& wb) {
        Resources& r = *res_;
        std::optional<waterfield::Push> push = r.fieldStepper.frame(o, *r.fieldKickData, *r.fieldFrameData);
        if (!push || !r.fieldProgram.get()) return false;
        if (presentStats_ && !fieldQueries_[0]) glCreateQueries(GL_TIME_ELAPSED, GLsizei(fieldQueries_.size()), fieldQueries_.data());
        const GLuint query = presentStats_ ? fieldQueries_[fieldFrame_ % fieldQueries_.size()] : 0;
        if (query && fieldFrame_ >= fieldQueries_.size()) { // the result of 3 frames ago
            GLuint64 ns = 0;
            glGetQueryObjectui64v(query, GL_QUERY_RESULT, &ns);
            fieldNs_ += double(ns), ++fieldTimed_;
        }
        if (query) glBeginQuery(GL_TIME_ELAPSED, query);
        waterfield::Push p = *push;
        if (wb.ocean[0].x > 0.5f) p.cfg.w |= waterfield::kOcean, p.ocean = glm::vec4(glm::vec3(wb.ocean[1]), wb.ocean[0].y);
        const auto& k = *r.fieldKickData;
        glNamedBufferSubData(r.fieldKicks.get(), 0, GLsizeiptr(16 + size_t(k.count) * sizeof(WaveKick)), &k);
        glNamedBufferSubData(r.fieldFrame.get(), 0, sizeof(waterfield::FrameBuffer), r.fieldFrameData.get());
        glUseProgram(r.fieldProgram.get());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, r.fieldState.get());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, r.fieldWork.get());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, r.fieldKicks.get());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, r.fieldExternal ? r.fieldExternal : r.fieldNoKicks.get());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, r.fieldFrame.get());
        glBindTextureUnit(12, r.ocean.get());
        glBindImageTexture(0, r.ripples.get(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glUniform4fv(1, 1, glm::value_ptr(p.par));
        glUniform4fv(2, 1, glm::value_ptr(p.win));
        glUniform4fv(3, 1, glm::value_ptr(p.phys));
        glUniform4fv(4, 1, glm::value_ptr(p.damp));
        glUniform4fv(5, 1, glm::value_ptr(p.ocean));
        for (waterfield::Pass pass : waterfield::kPasses) {
            p.cfg.x = pass;
            glUniform4iv(0, 1, glm::value_ptr(p.cfg));
            glDispatchCompute(waterfield::groups(pass), 1, 1);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
        }
        if (query) glEndQuery(GL_TIME_ELAPSED);
        ++fieldFrame_;
        return true;
    }
    // Contract C3 (b): a GL buffer with the kick layout (`uint count; uint pad[3]; vec4 kicks[4096]`) that the field
    // consumes each frame it runs (SSBO 3), setting its count back to 0. 0 detaches it.
    void setExternalWaveKicks(GLuint buffer) { res_->fieldExternal = buffer; }

    // Enhanced water's animated water texture: uw_water.frag into mip 0 (time = the demo's tm.x), the caustic mesh added
    // into g only (uw_caustics.*: blend ONE + ONE), the mips box-filtered down (the demo's fsMip), bound to unit 18 for
    // the water, the shafts, the particles and the post pass.
    void drawAgWater(float time) {
        Resources& r = *res_;
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glBindFramebuffer(GL_FRAMEBUFFER, r.agWaterFbo.get());
        glViewport(0, 0, kAgWaterSize, kAgWaterSize);
        glUseProgram(r.agWaterProgram.get());
        glUniform1f(0, time);
        glBindVertexArray(r.emptyVao.get());
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glUseProgram(r.agCausticProgram.get());
        glUniform1f(0, time);
        glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE);
        glDrawArrays(GL_TRIANGLES, 0, kAgCausVertices);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glGenerateTextureMipmap(r.agWater.get());
        glBindTextureUnit(18, r.agWater.get());
    }
    // Enhanced water: resolve the opaque scene into the copy textures, then draw the water surfaces
    // with refraction/reflection/ripples and the splash droplets on top.
    void drawWater(const Camera& camera, Viewport vp, const FrameOptions& options) {
        Resources& r = *res_;
        Targets& t = r.targets;
        glBlitNamedFramebuffer(t.scene.get(), t.opaqueColorFbo.get(), 0, 0, vp.width, vp.height, 0, 0, vp.width, vp.height,
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBlitNamedFramebuffer(t.scene.get(), t.opaqueDepthFbo.get(), 0, 0, vp.width, vp.height, 0, 0, vp.width, vp.height,
                               GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, t.scene.get());
        glViewport(0, 0, vp.width, vp.height);
        glBindTextureUnit(5, t.opaqueColor.get());
        glBindTextureUnit(6, t.opaqueDepth.get());
        glBindTextureUnit(7, r.ripples.get());
        glBindTextureUnit(12, r.ocean.get()); // the FFT ocean (shaders/ocean.comp)
        GLuint program = r.waterProgram.get();
        glUseProgram(program);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE); // the surface must also read from below
        // OU_VK_PRESENT_STATS: the surface draws' GPU time (the result of 3 frames ago).
        if (presentStats_ && !surfaceQueries_[0]) glCreateQueries(GL_TIME_ELAPSED, GLsizei(surfaceQueries_.size()), surfaceQueries_.data());
        const GLuint query = presentStats_ ? surfaceQueries_[surfaceFrame_ % surfaceQueries_.size()] : 0;
        if (query && surfaceFrame_ >= surfaceQueries_.size()) {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(query, GL_QUERY_RESULT, &ns);
            surfaceNs_ += double(ns), ++surfaceTimed_;
        }
        if (query) glBeginQuery(GL_TIME_ELAPSED, query);
        if (r.waterTessProgram.get()) glPatchParameteri(GL_PATCH_VERTICES, 3);
        for (uint32_t i = 0; i < r.meshes.size(); ++i) {
            const GpuMesh& m = r.meshes[i];
            if (!(m.water || sea_[i]) || clippedAway(i)) continue;
            // Tessellated where the mesh reaches into the wave field's window (water.tesc), else per vertex.
            const bool tess = r.waterTessProgram.get() && waterfield::touches(options, m.center, m.radius);
            if (const GLuint want = tess ? r.waterTessProgram.get() : r.waterProgram.get(); want != program) glUseProgram(program = want);
            scissorFor(i, vp);
            glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(m.model));
            const glm::mat4 waves = waterWaves(m.center, m.radius, options.waterVolumes);
            glUniformMatrix4fv(1, 1, GL_FALSE, glm::value_ptr(waves));
            glUniform1f(2, m.waterRestZ); // water.vert uRestZ (render::waterRestZ)
            glBindVertexArray(m.vao.get());
            for (uint32_t s = 0; s < m.submeshes.size(); ++s)
                if (waterDraw(i, s)) {
                    glDrawElements(tess ? GL_PATCHES : GL_TRIANGLES, GLsizei(m.submeshes[s].indexCount), GL_UNSIGNED_INT,
                                   reinterpret_cast<const void*>(uintptr_t(m.submeshes[s].firstIndex) * sizeof(uint32_t)));
                    ++waterSurfaces_;
                }
        }
        if (query) glEndQuery(GL_TIME_ELAPSED), ++surfaceFrame_;
        scissorFor(UINT32_MAX, vp);
        const fluid::Frame ff = r.fluidProgram.get() ? r.fluidSim.frame(options, camera.position) : fluid::Frame{};
        if (ff.active) {
            if (presentStats_) {
                if (r.fluidTimed) { // last drawing frame's timestamps (waits for that frame if the GPU is behind)
                    GLuint64 ts[3];
                    for (int i = 0; i < 3; ++i) glGetQueryObjectui64v(r.fluidTimes[size_t(i)].get(), GL_QUERY_RESULT, &ts[i]);
                    r.fluidSimMs += double(ts[1] - ts[0]) * 1e-6, r.fluidDrawMs += double(ts[2] - ts[1]) * 1e-6;
                    ++r.fluidTimedFrames;
                }
                glQueryCounter(r.fluidTimes[0].get(), GL_TIMESTAMP);
            }
            simulateFluid(ff);
            if (presentStats_) glQueryCounter(r.fluidTimes[1].get(), GL_TIMESTAMP);
            // 1 splat: max-blend each particle's depth / thickness / foam, tested against the opaque depth copy (unit 6).
            glDisable(GL_DEPTH_TEST);
            const float empty[4] = {kSplashEmpty, 0.0f, 0.0f, 0.0f};
            glClearNamedFramebufferfv(t.splashFbo[0].get(), GL_COLOR, 0, empty);
            glBindFramebuffer(GL_FRAMEBUFFER, t.splashFbo[0].get());
            glUseProgram(r.splashSplatProgram.get());
            glEnable(GL_BLEND);
            glBlendEquation(GL_MAX);
            glBlendFunc(GL_ONE, GL_ONE);
            glBindVertexArray(r.emptyVao.get());
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, fluid::kCapacity); // the pool: SSBO 0 (simulateFluid)
            glBlendEquation(GL_FUNC_ADD);
            glDisable(GL_BLEND);
            // 2 blur: horizontal into splash[1], vertical back into splash[0].
            glUseProgram(r.splashBlurProgram.get());
            for (int pass = 0; pass < 2; ++pass) {
                glBindFramebuffer(GL_FRAMEBUFFER, t.splashFbo[1 - pass].get());
                glBindTextureUnit(10, t.splash[pass].get());
                glUniform2f(0, pass == 0 ? 1.0f : 0.0f, pass == 0 ? 0.0f : 1.0f);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
            // 3 composite: premultiplied liquid over the scene, refracting the scene with the water surfaces in it (the
            // colour copy on unit 5, refreshed).
            glBlitNamedFramebuffer(t.scene.get(), t.opaqueColorFbo.get(), 0, 0, vp.width, vp.height, 0, 0, vp.width,
                                   vp.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, t.scene.get());
            glBindTextureUnit(10, t.splash[0].get());
            glUseProgram(r.splashCompositeProgram.get());
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glDisable(GL_BLEND);
            glEnable(GL_DEPTH_TEST);
            if (presentStats_) glQueryCounter(r.fluidTimes[2].get(), GL_TIMESTAMP), r.fluidTimed = true;
        }
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
    }

    // The splash fluid's simulation for this frame (render/Fluid.hpp, shaders/fluid.comp): zero the C3 kick count, kill
    // every particle if asked, emit the bursts, then each fixed step: clear the grid's bucket heads, Predict, kIterations
    // x (Lambda, Delta), Finalize. Reads the opaque depth (unit 6) and ripples (unit 7) drawWater bound; leaves the pool
    // on SSBO 0 for splash.vert.
    void simulateFluid(const fluid::Frame& f) {
        Resources& r = *res_;
        const GLuint zero = 0, none = 0xFFFFFFFFu;
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        glClearNamedBufferSubData(r.fluidKicks.get(), GL_R32UI, 0, 4, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        if (f.clear) glClearNamedBufferData(r.fluidParticles.get(), GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        if (!f.bursts.empty())
            glNamedBufferSubData(r.fluidBursts.get(), 0, GLsizeiptr(f.bursts.size() * sizeof(fluid::GpuBurst)), f.bursts.data());
        glUseProgram(r.fluidProgram.get());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, r.fluidParticles.get());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, r.fluidGrid.get());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, r.fluidKicks.get());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, r.fluidBursts.get());
        auto run = [&](fluid::Pass pass, int source, int threads) {
            const fluid::Push p = r.fluidSim.push(pass, f, source);
            glUniform4iv(0, 1, &p.cfg.x);
            glUniform4fv(1, 1, &p.sim.x);
            glUniform4fv(2, 1, &p.par.x);
            glUniform4fv(3, 1, &p.lim.x);
            glDispatchCompute(GLuint((threads + fluid::kThreads - 1) / fluid::kThreads), 1, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        };
        if (f.emitted) run(fluid::Emit, 0, f.emitted);
        for (int s = 0; s < f.steps; ++s) {
            glClearNamedBufferSubData(r.fluidGrid.get(), GL_R32UI, 0, GLsizeiptr(fluid::kHashSize) * 4, GL_RED_INTEGER,
                                      GL_UNSIGNED_INT, &none);
            run(fluid::Predict, 0, fluid::kCapacity);
            for (int it = 0; it < fluid::kIterations; ++it) {
                run(fluid::Lambda, it & 1, fluid::kCapacity);
                run(fluid::Delta, it & 1, fluid::kCapacity);
            }
            run(fluid::Finalize, 0, fluid::kCapacity);
        }
    }

    // Enhanced under-water composite (render/Water.hpp UwGrade), after the post pass wrote uwHdr: abyssal-garden main.js's
    // bloom chain (down0 uwHdr -> level 0, down level i - 1 -> i, up level i -> i - 1 added), then the grade into the
    // default framebuffer (units 0 / 1 + 14 the blurred shafts, added when `shafts`; uniform location 0 = mode | shafts
    // on, exposure, fade, strength).
    void drawUwComposite(Viewport vp, const UwGrade& grade, bool shafts, float fade) {
        Targets& t = res_->targets;
        auto level = [&](int i) { return glm::ivec2(std::max(1, vp.width >> (i + 1)), std::max(1, vp.height >> (i + 1))); };
        auto blit = [&](int out, float mode, GLuint input) {
            const glm::ivec2 s = level(out);
            glBindFramebuffer(GL_FRAMEBUFFER, t.uwBloomFbo[size_t(out)].get());
            glViewport(0, 0, s.x, s.y);
            glBindTextureUnit(0, input);
            glUniform4f(0, mode, 0.0f, 0.0f, 0.0f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        };
        glUseProgram(res_->uwBloomProgram.get());
        for (int i = 0; i < kUwBloomLevels; ++i) blit(i, i ? 1.0f : 0.0f, i ? t.uwBloom[size_t(i - 1)].get() : t.uwHdr.get());
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        for (int i = kUwBloomLevels - 1; i > 0; --i) blit(i - 1, 2.0f, t.uwBloom[size_t(i)].get());
        glDisable(GL_BLEND);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, vp.width, vp.height);
        glUseProgram(res_->uwGradeProgram.get());
        glBindTextureUnit(0, t.uwHdr.get());
        glBindTextureUnit(1, t.uwBloom[0].get());
        glBindTextureUnit(14, t.shaft[1].get());
        glUniform4f(0, shafts ? 1.0f : 0.0f, grade.exposure, fade, grade.strength);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // Enhanced under-water particles (render/Water.hpp UwParticlePush): marine snow, then the bubbles, premultiplied into
    // their own target (cleared to 0), depth-tested in the shaders against the opaque depth copy (unit 6, drawWater);
    // the post pass composites the target over the fogged scene.
    void drawUwParticles(const Camera& camera, Viewport vp, const WaterBlock& wb, const FrameOptions& options) {
        Resources& r = *res_;
        Targets& t = r.targets;
        const UwParticlePush push = uwParticlePush(camera, wb);
        glNamedBufferSubData(r.uwUbo.get(), 0, sizeof push, &push);
        glBindBufferBase(GL_UNIFORM_BUFFER, 7, r.uwUbo.get());
        const float zero[4] = {0, 0, 0, 0};
        glClearNamedFramebufferfv(t.uwParticlesFbo.get(), GL_COLOR, 0, zero);
        glBindFramebuffer(GL_FRAMEBUFFER, t.uwParticlesFbo.get());
        glViewport(0, 0, vp.width, vp.height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glBindTextureUnit(6, t.opaqueDepth.get());
        glUseProgram(r.uwSnowProgram.get());
        glBindVertexArray(r.emptyVao.get());
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, GLsizei(push.params.x));
        if (!options.bubbles.empty()) {
            glNamedBufferData(r.uwBubbleVbo.get(), GLsizeiptr(options.bubbles.size_bytes()), options.bubbles.data(), GL_STREAM_DRAW);
            glUseProgram(r.uwBubbleProgram.get());
            glBindVertexArray(r.uwBubbleVao.get());
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, GLsizei(options.bubbles.size()));
        }
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, t.scene.get());
    }

    // Original particle effects: depth-tested, no depth writes, batches in the given (back-to-front) order. Material
    // particles that refract or fade softly read single-sample copies of the scene drawn before the effects.
    void drawFx(const Camera& camera, Viewport vp, const glm::mat4& viewProj, const FrameOptions& options) {
        if (options.fxVertices.empty() || options.fxBatches.empty()) return;
        Resources& r = *res_;
        Targets& t = r.targets;
        if (std::any_of(options.fxBatches.begin(), options.fxBatches.end(), [](const FxBatch& b) { return b.screen(); })) {
            glBlitNamedFramebuffer(t.scene.get(), t.opaqueColorFbo.get(), 0, 0, vp.width, vp.height, 0, 0, vp.width,
                                   vp.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBlitNamedFramebuffer(t.scene.get(), t.opaqueDepthFbo.get(), 0, 0, vp.width, vp.height, 0, 0, vp.width,
                                   vp.height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, t.scene.get());
            glBindTextureUnit(5, t.opaqueColor.get());
            glBindTextureUnit(6, t.opaqueDepth.get());
        }
        glNamedBufferData(r.fxVbo.get(), GLsizeiptr(options.fxVertices.size_bytes()), options.fxVertices.data(), GL_STREAM_DRAW);
        glVertexArrayVertexBuffer(r.fxVao.get(), 0, r.fxVbo.get(), 0, sizeof(FxVertex));
        glUseProgram(r.fxProgram.get());
        glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(viewProj));
        const float n = camera.nearZ, f = camera.farZ;
        glUniform2f(2, -(f - n) / (f * n), 1.0f / n); // DepthToW (OriginalShading c77)
        glBindVertexArray(r.fxVao.get());
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        auto tex = [&](uint32_t id) {
            const auto it = r.textures.find(id);
            return it != r.textures.end() ? it->second : r.white.get();
        };
        for (const auto& b : options.fxBatches) {
            glBindTextureUnit(0, tex(b.textureId));
            glBindTextureUnit(8, tex(b.normalId));
            glUniform4f(1, b.distort, b.softScale, b.softBias, b.lit ? 1.0f : 0.0f);
            if (b.lit) {
                glUniform4f(3, b.light0[0], b.light0[1], b.light0[2], b.litLow);
                glUniform4f(4, b.light1[0], b.light1[1], b.light1[2], b.litHigh);
            }
            glBlendEquation(b.blend == 2 ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
            if (b.blend == 0) glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            else if (b.blend == 3) glBlendFunc(GL_DST_COLOR, GL_SRC_ALPHA);
            else glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glDrawArrays(GL_TRIANGLES, GLint(b.first), GLsizei(b.count));
        }
        glBlendEquation(GL_FUNC_ADD);
        glDisable(GL_BLEND);
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
                     const std::array<RgbaImage, 3>& uiImages) {
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
    original::SunFields uploadSun_;       // the uploaded environment's sun (applySky restores it without a time of day)
    std::vector<uint32_t> cookieIds_;     // layers of the spot cookie array (upload)
    std::array<uint8_t, 256 * 4> fogRow_ = original::fogLutRow(nullptr); // the LUT's fog row as uploaded
    original::IrradianceSlots irradiance_; // per frame, cached per mesh between uploads
    original::Pssm pssm_;                  // this frame's splits (inactive: the PSSM programs read lit)
    Camera pssmCamera_;                    // the frame's camera (WorldToPSSM0's view depth; reflections reuse it)
    std::unordered_set<uint32_t> cubes_; // texture ids uploaded as cube maps
    // Renderer::uploadPart: a part's scene until pumpParts sent it all (next texture / mesh to look at), by part id.
    struct Part {
        std::shared_ptr<const scene::SceneData> data;
        size_t texture = 0, mesh = 0;
    };
    std::map<uint64_t, Part> parts_;
    uint64_t uploads_ = 0; // upload() calls
    uint64_t drawFrames_ = 0; // drawFrame calls (GpuMesh::movedFrame)
    std::vector<uint8_t> materialBlocks_; // during upload()
    std::vector<uint8_t> objectScratch_;
    size_t objectSlots_ = 0; // objectUbo blocks: 2 x meshes + terrain light groups (finishOriginals)
    std::vector<DrawRef> deferred_; // per frame: back/depth-buffer readers (drawScreenReaders)
    std::span<const UnitClip> unitClips_; // per frame: FrameOptions::unitClips
    std::vector<int> meshClip_;           // per frame and GPU mesh: index into unitClips_, -1 = unclipped
    int scissorClip_ = -1;
    bool reflecting_ = false;             // drawReflection drew Targets::reflection this frame                // clip the GL scissor is set to, -1 = scissor test off
    std::vector<float> opacity_;    // per frame: FrameOptions::meshOpacity per mesh (empty: all 1)
    glm::vec3 lodEye_{0.0f};        // per frame: the camera batch LODs are measured from (every pass, 0x50F7C0)
    std::vector<uint8_t> sea_;      // per frame (Enhanced): render::reflectsWater per mesh
    // Enhanced water draws this submesh (water pass, not the scene pass).
    bool waterDraw(uint32_t mesh, uint32_t sub) const {
        const GpuMesh& m = res_->meshes[mesh];
        return m.water || (mesh < sea_.size() && sea_[mesh] && m.reflective[sub]);
    }
    bool s3tc_ = false, debug_ = false;
    float maxAniso_ = 1.0f;
};

} // namespace

std::unique_ptr<Renderer> createGlRenderer() { return std::make_unique<GlRenderer>(); }

} // namespace ou::render
