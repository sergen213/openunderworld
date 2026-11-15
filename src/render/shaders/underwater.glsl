// Underwater post: tru.exe's full-screen water pass and light rays + Enhanced caustics and sun shafts on the submerged scene.
// Included by the post shaders after their own declarations; returns the input colour unchanged
// when the camera is not submerged, so Original output stays byte-identical.
#ifdef VULKAN
layout(set = 1, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 ambient;
    vec4 detail; vec4 lamps[4]; vec4 lampColor[4]; vec4 haze; vec4 mist; vec4 weather; vec4 volumes[16];
    vec4 filters[9]; uvec4 filterLut[640]; // original screen filters (shaders/filter.glsl)
    vec4 waterPass[3]; // tru.exe's full-screen under-water pass (ouWaterPass); [2].y light-ray weight (ouRays)
    uvec4 rays[157];   // tru.exe's under-water light rays, 50 x 50 bytes (ouRays)
    mat4 seaWaves;     // tru.exe's sea waves at the camera (caustics: shaders/waves.glsl)
    vec4 optics[3];    // Enhanced water colour (render::waterOptics; water.frag ouWaterLook): all 0 = the unit's own
    vec4 ocean[2];     // the FFT ocean (caustics: shaders/waves.glsl ouOcean*)
} w;
#ifdef VULKAN
layout(set = 1, binding = 1) uniform sampler2D uDepth;
layout(set = 1, binding = 3) uniform sampler2D uRipples; // the interactive wave field (caustics)
layout(set = 1, binding = 5) uniform sampler2DArray uOcean; // the FFT ocean (caustics; shaders/ocean.comp)
layout(set = 1, binding = 10) uniform sampler2D uAgWater; // the animated water texture (render/Water.hpp kAgWaterSize; VkRenderer recordAgWater)
#define OU_NDC_Y(uv) (1.0 - 2.0 * (uv).y)
#define OU_NDC_Z(d) (d)
#else
layout(binding = 6) uniform sampler2D uDepth;
layout(binding = 9) uniform sampler2D uRipples;
layout(binding = 12) uniform sampler2DArray uOcean;
layout(binding = 18) uniform sampler2D uAgWater; // the animated water texture (GlRenderer drawAgWater)
#define OU_NDC_Y(uv) (2.0 * (uv).y - 1.0)
#define OU_NDC_Z(d) (2.0 * (d) - 1.0)
#endif
vec3 ouWorld(vec2 uv, float depth, mat4 invViewProj) {
    vec4 clip = vec4(2.0 * uv.x - 1.0, OU_NDC_Y(uv), OU_NDC_Z(depth), 1.0);
    vec4 p = invViewProj * clip;
    return p.xyz / p.w;
}
// tru.exe's under-water pass (0x6D8C20 drawing the unit's water material, PS lib 410 slot 0; RENDERING.md "Under
// water"), both water qualities: the colour runs from the deep colour (looking down) to the surface colour (looking
// up), its alpha from the eye's value to 1 at the material's distance (view depth). The PS's noise texture wobble
// (+-0.05 of the gradient, 0.004 of the colour) and the scripted murk (c134.y) are left out.
// Enhanced water colour preset (w.optics[0].w, only set with Enhanced water): the colour takes the preset's in-scatter
// hue at the unit colour's luminance (as water.frag ouWaterLook), the alpha its per-channel extinction from the eye's
// value (red fogs first), so the view under water matches the surface seen from above.
vec3 ouPresetHue(vec3 gammaCol) { // gamma colour -> the preset hue at its luminance, gamma
    float lum = dot(pow(max(gammaCol, vec3(0.0)), vec3(2.2)), vec3(0.2126, 0.7152, 0.0722));
    return pow(max(w.optics[1].rgb * lum, vec3(0.0)), vec3(1.0 / 2.2));
}
// Enhanced under-water scene (ours, camera submerged: w.fogColor.w; RENDERING.md "Under water"): abyssal-garden's W_SOLID
// light and S_B fog (shaders/uw_medium.glsl, ag section) per pixel. An opaque point under its water surface is relit
// (agRelight: the depth falloff, the uAgWater caustics, ambient and side light; agCausLod with levels from the pixel
// footprint: p comes from the depth buffer), then fogged: agFog(c, p) = c e^(-sg d) ss(230, 185, d) + fogIn. Rays that
// leave through the surface (looking up from under it: water.frag's Snell's window, drawn over the sky too) fog up to it
// and keep that colour (W_SURF's fog(c, wp)); sky pixels are the demo's background (vsB / fsB): fog(0, eye + dir 240 m).
#include "shaders/waves.glsl" // kM
#include "shaders/uw_medium.glsl"
float ouSunShadowVisibility(vec3 p); // shaders/atmosphere.glsl (after render/sun.glsl in both renderers)
// raw: the scene texel itself (before FXAA / the screen filters): water.frag writes its from-below colour there encoded
// (agSurfEncode) and exactly 0 outside Snell's window.
vec3 ouUwFog(vec3 c, vec3 raw, vec2 uv) {
    float depth = texture(uDepth, uv).r;
    vec3 q = ouWorld(uv, depth, w.invViewProj);
    vec3 n = cross(dFdx(q), dFdy(q)); // before any branch
    bool sky = depth >= 0.999999;
    vec3 far = ouWorld(uv, 1.0, w.invViewProj);
    vec3 rd = normalize((sky ? far : q) - w.eye.xyz);
    float len = sky ? 1e30 : distance(q, w.eye.xyz), surf = ouUwSurfaceZ();
    bool through = rd.z > 1e-4 && w.eye.z < surf && (surf - w.eye.z) / rd.z < len;
    vec3 lin = pow(max(c, vec3(0.0)), vec3(2.2));
    vec3 cam = agEye(), dir = agDir(rd), lit, p;
    // Outside Snell's window the demo's surface pixels are 0 and unfogged (water.frag: its sky term is NaN there and fin()
    // clamps the fogged NaN to 0). Also where the opaque point lies just under the flat surface (within the waves'
    // reach, 3 m): the displaced water mesh can cover it while the flat-plane test above says it does not. The demo's
    // terrain hides its black surface at a distance; our open seas would show it meeting the 240 m background (fsB) at
    // a hard horizon. So the black fades into W_SURF's own fog of it, fog(0, wp) = fogIn, over the distances where its
    // slopes flatten (fl = ss(260, 60, d)): black as now within 60 m, the fogged surface past 260 m (= the background:
    // fogIn is saturated by then).
    if (rd.z > 1e-4 && w.eye.z < surf && all(equal(raw, vec3(0.0))) && (through || q.z > surf - 3.0 * kM)) {
        p = cam + dir * ((surf - w.eye.z) / rd.z / kM);
        vec3 fz = agFog(vec3(0.0), p, kAgZone) * (1.0 - agSs(260.0, 60.0, distance(p, cam)));
        return pow(max(mix(lin, fz, w.waterPass[2].x), vec3(0.0)), vec3(1.0 / 2.2));
    }
    if (through) { // the window's HDR colour comes encoded through the 8-bit scene target
        p = cam + dir * ((surf - w.eye.z) / rd.z / kM);
        lit = agSurfDecode(lin);
    } else if (sky) p = cam + dir * 240.0, lit = vec3(0.0);
    else {
        p = agPos(q);
        lit = agUnpost(lin); // TRU's display colour -> the demo's scene-referred HDR (uw_medium.glsl)
        float ps = -1e30; // the point's own water surface
        for (int i = 0; i < int(w.detail.w); ++i) {
            vec3 lo = w.volumes[2 * i].xyz, hi = w.volumes[2 * i + 1].xyz;
            if (all(greaterThanEqual(q, lo)) && all(lessThan(q, hi))) { ps = hi.z; break; }
        }
        if (ps > q.z) {
            n = dot(n, n) > 0.0 ? normalize(n) : vec3(0.0, 0.0, 1.0);
            if (dot(n, w.eye.xyz - q) < 0.0) n = -n;
            float d = distance(p, cam);
            // the pixel's footprint: distance x the angle between neighbouring pixels' rays
            vec3 f1 = ouWorld(uv + vec2(0.0, w.screen.w), 1.0, w.invViewProj);
            float fp = d * length(normalize(f1 - w.eye.xyz) - normalize(far - w.eye.xyz));
            vec3 ca = w.sunColor.w > 0.0 ? agCausLod(uAgWater, p, d, fp) : vec3(0.33); // no sun: no caustics
            lit = agRelight(lit, p, agDir(n), -dir, ca, ouSunShadowVisibility(q + n * (0.35 * kM)));
        }
    }
    vec3 fogged = agFog(lit, p, kAgZone);
    return pow(max(mix(lin, fogged, w.waterPass[2].x), vec3(0.0)), vec3(1.0 / 2.2));
}
vec3 ouWaterPass(vec3 c, vec3 raw, vec2 uv) {
    if (w.waterPass[2].x <= 0.0) return c;
    if (w.fogColor.w > 0.5) return ouUwFog(c, raw, uv); // Enhanced, submerged
    vec3 far = ouWorld(uv, 1.0, w.invViewProj);
    float up = clamp(0.5 + 0.5 * normalize(far - w.eye.xyz).z, 0.0, 1.0);
    vec3 col = mix(w.waterPass[1].rgb, w.waterPass[0].rgb, up);
    vec3 q = ouWorld(uv, texture(uDepth, uv).r, w.invViewProj);
    float viewZ = (w.viewProj * vec4(q, 1.0)).w;
    if (w.optics[0].w > 0.5) {
        vec3 a = 1.0 - (1.0 - w.waterPass[0].w) * exp(-w.optics[0].rgb * min(viewZ, 1e5));
        return mix(c, ouPresetHue(col), clamp(a * w.waterPass[2].x, 0.0, 1.0));
    }
    float a = mix(w.waterPass[0].w, 1.0, clamp(viewZ / w.waterPass[1].w, 0.0, 1.0)) * w.waterPass[2].x;
    return mix(c, col, clamp(a, 0.0, 1.0));
}
// tru.exe's under-water light rays (render::UnderwaterRays, both water qualities): the 50 x 50 additive image of the
// record's light columns, which the +0x6C material (PS lib 405) adds over the screen, bilinear, x the weight.
float ouRayTexel(ivec2 p) {
    p = clamp(p, ivec2(0), ivec2(49));
    uint i = uint(p.y * 50 + p.x);
    return float((w.rays[i >> 4][(i >> 2) & 3u] >> (8u * (i & 3u))) & 255u) / 255.0;
}
vec3 ouRays(vec3 c, vec2 uv) {
    if (w.waterPass[2].y <= 0.0) return c;
    vec2 t = vec2(uv.x, 0.5 - 0.5 * OU_NDC_Y(uv)) * 50.0 - 0.5; // texels, rows down
    ivec2 i = ivec2(floor(t));
    vec2 f = t - vec2(i);
    float r = mix(mix(ouRayTexel(i), ouRayTexel(i + ivec2(1, 0)), f.x), mix(ouRayTexel(i + ivec2(0, 1)), ouRayTexel(i + ivec2(1, 1)), f.x), f.y);
    return c + r * w.waterPass[2].y;
}
// Enhanced (RENDERING.md "Under water"): volumetric sun shafts, abyssal-garden's (Noniv, MIT; docs/RESEARCH.md 8.11)
// shaders.js W_RAYS + W_RBLUR literally, in ag space (shaders/uw_medium.glsl), two half-resolution passes before the post
// pass while ouShaftsOn(); uw_grade.frag adds them with W_POST's depth-aware 4-tap upsample after the bloom chain read the
// frame (main.js: scene -> rays -> blur -> bloom of the scene -> post). VK post set 1 bindings 6 (march output: the blur's
// input) and 7 (blurred: the grade's, through its own set 2); GL units 13 and 14.
#ifdef VULKAN
layout(set = 1, binding = 6) uniform sampler2D uShaftsRaw;
#else
layout(binding = 13) uniform sampler2D uShaftsRaw;
#endif
float ouSunTapVisibility(vec3 p); // shaders/atmosphere.glsl
const float kAgRayn = 34.0;  // quality.js RAYN (high)
const float kAgRays = 0.54;  // g.mis.y = main.js RAYS
const float kAgZFar = 6e4;   // view depth of sky pixels, metres: the demo's NEAR / 1e-6 = 1e5 clamped under half float's 65504
// Shafts this frame: Enhanced water (fogColor.w), the camera below the nearest surface, a sun above the horizon.
// Mirrors render::underwaterShaftsActive (Water.cpp), which records the passes.
bool ouShaftsOn() {
    return w.fogColor.w > 0.5 && w.sunColor.w > 0.0 && w.sunDir.z >= 0.05 && w.detail.w > 0.5 && w.eye.z < w.volumes[1].z;
}
// The demo's zl = g.fwd.w / depth: linear view depth (metres) of the opaque scene at full-resolution pixel `fp`, sky
// kAgZFar. The post pass stores it in its linear frame's alpha for uw_grade.frag's upsample.
float agViewDepth(ivec2 fp) {
    float d = texelFetch(uDepth, fp, 0).r;
    if (d >= 0.999999) return kAgZFar;
    vec2 uv = (vec2(fp) + 0.5) / vec2(textureSize(uDepth, 0));
    return min((w.viewProj * vec4(ouWorld(uv, d, w.invViewProj), 1.0)).w / kM, kAgZFar);
}
// W_RAYS fs at half-resolution pixel `hp` (fc.xy = hp + 0.5): rgb = in-scattered sun light, a = zl. The ray leaves the
// eye through uv = fc.xy * 2 / full size (our inverse view-projection instead of fwd/right/up x tan), rl = 1 / its
// forward component (viewProj's w row), tm = min(zl rl, 120 m), cut at the surface (y = 0) for rising rays, RAYN steps
// jittered by ign(fc.xy). Per step: the sun shadow map (one compare tap: ouSunTapVisibility at the world point, our own
// bias, for textureSampleCompareLevel(.., s.z - .0006)), the streak pattern ss(.3, .7, tW(q / 27) lod 2.5) x ss(.15, .6,
// tW(q.yx / 11 + .3) lod 2), q = p.xz - sun.xz p.y / sun.y, weight exp(-sg t + kd p.y / sun.y). The demo's mist term
// (1 + 1.2 mist^2, mist = sat(1 - (vol(p).r - .5) 3.4): vol().r = its openness, 1 in open water, so mist is 0 there and
// only rises next to its rock) has no volume here: mist = 0. Zone z = kAgZone (sun.w = 0): ph = .2 + 2 mu^3, wc = WCOL +
// (.04, .07, .05), (1 - .55 z) = 1.
vec4 agRays(ivec2 hp) {
    vec2 fc = vec2(hp) + 0.5;
    ivec2 fp = min(hp * 2, textureSize(uDepth, 0) - 1);
    vec2 uv = fc * 2.0 / vec2(textureSize(uDepth, 0));
    vec3 rw = normalize(ouWorld(uv, 1.0, w.invViewProj) - w.eye.xyz); // world, z up
    vec3 rd = agDir(rw);
    float rl = 1.0 / max(dot(vec3(w.viewProj[0][3], w.viewProj[1][3], w.viewProj[2][3]), rw), 1e-6);
    float zl = agViewDepth(fp);
    vec3 ro = agEye();
    float tm = min(zl * rl, 120.0);
    if (rd.y > 1e-4) tm = min(tm, -ro.y / rd.y);
    tm = max(tm, 0.0);
    vec3 sx = agSun(), k = agKd(kAgZone), sgz = agSg(kAgZone);
    float dt = tm / kAgRayn;
    float j = agIgn(fc);
    vec3 acc = vec3(0.0);
    for (int n = 0; n < int(kAgRayn); ++n) {
        float t = (float(n) + j) * dt;
        vec3 p = ro + rd * t;
        float vis = ouSunTapVisibility(agShadowPoint(w.eye.xyz + rw * (t * kM))); // along agSun (uw_medium.glsl)
        vec2 q = p.xz - sx.xz * (p.y / sx.y);
        float pat = agSs(0.3, 0.7, textureLod(uAgWater, q / 27.0, 2.5).r) *
                    agSs(0.15, 0.6, textureLod(uAgWater, q.yx / 11.0 + 0.3, 2.0).r);
        acc += exp(-sgz * t + k * (agLightY(p.y) / sx.y)) * (vis * (0.012 + pat * 6.0)); // agLightY [I]: uw_medium.glsl
    }
    float mu = max(dot(rd, sx), 0.0);
    float ph = mix(0.2 + 2.0 * mu * mu * mu, 0.8, kAgZone);
    vec3 wc = mix(kAgWcol + vec3(0.04, 0.07, 0.05), kAgPcol + vec3(0.28, 0.16, 0.12), kAgZone);
    return vec4(min(acc * dt * sgz * wc * kAgSunc * ph * kAgRays * (1.0 - 0.55 * kAgZone), vec3(20.0)), zl);
}
// W_RBLUR: depth-aware 3x3 blur of agRays at half resolution, weights 1 / (.03 + |r.a - c.a| / c.a).
vec4 agRaysBlur(ivec2 p) {
    ivec2 mx = textureSize(uShaftsRaw, 0) - 1;
    vec4 c = texelFetch(uShaftsRaw, p, 0);
    vec3 acc = vec3(0.0);
    float ws = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec4 r = texelFetch(uShaftsRaw, clamp(p + ivec2(x, y), ivec2(0), mx), 0);
            float wk = 1.0 / (0.03 + abs(r.a - c.a) / max(c.a, 1e-6));
            acc += r.rgb * wk, ws += wk;
        }
    return vec4(acc / ws, c.a);
}
// raw: the scene texel at uv before FXAA and the screen filters (ouUwFog reads water.frag's from-below encoding from it).
vec3 ouUnderwater(vec3 c, vec3 raw, vec2 uv) {
    // the water pass relights and fogs the scene (Enhanced: ouUwFog), then tru.exe's rays add light (0x6D8C20 order;
    // Original water only: ou-game leaves them out with Enhanced water); the Enhanced sun shafts are added by uw_grade.frag
    return ouRays(ouWaterPass(c, raw, uv), uv);
}
