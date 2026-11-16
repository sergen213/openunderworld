#version 460
// Enhanced water's floor caustics (ours, not abyssal-garden's; docs/RENDERING.md "Under water"): sunlight refracted
// through a wavy surface and forward-mapped onto a plane kFocus metres below it (Evan Wallace's caustic mesh). A grid
// over one caustic tile (kTile metres, uw_medium.glsl agCausLod's tiling; kCells^2 cells, two triangles each) is
// displaced by where each surface point's refracted vertical ray lands: x + kFocus (1 - 1/1.333) grad h (small slopes;
// agCuv projects the map along the refracted sun when sampling). Each triangle adds source area / landed area to the g
// channel of the animated water texture (uAgWater, additive, colour mask G; uw_caustics.frag), so the light is
// conserved: the tile's mean is 1, bright where triangles shrink (the filaments), dim where they spread (the cells).
// The surface is periodic on the tile, so a grid over [-m, 1 + m] of it (m = kMargin cells >= the largest landing
// offset), clipped to the viewport, delivers every contribution a texel receives, wrapped ones included.
// No vertex buffers: 6 vertices per cell from the vertex index (render/Water.hpp kAgCausVertices). Time = the demo's
// tm.x = agTime() (WaterBlock eye.w, seconds): VK push constant (shared with uw_water.frag) / GL uniform location 0.
#ifdef VULKAN
layout(push_constant) uniform AgWaterPush { float time; } pc;
#define OU_AG_TIME pc.time
#define OU_VERTEX_ID gl_VertexIndex
#else
layout(location = 0) uniform float uAgTime;
#define OU_AG_TIME uAgTime
#define OU_VERTEX_ID gl_VertexID
#endif
layout(location = 0) flat out float vGain;
const int kCells = 256, kMargin = 32, kRow = kCells + 2 * kMargin; // = render::kAgCausCells / kAgCausMargin
const float kTile = 6.6;                  // metres per tile (uw_medium.glsl kAgCausTile)
const float kFocus = 4.0;                 // metres from the surface to the plane the map is formed on
const float kRefr = 1.0 - 1.0 / 1.333;    // refracted ray tilt per unit slope (vertical sun, small slopes)
const float kPeak = 64.0;                 // gain cap where a triangle collapses (a fold): ~1 % of the light
// The surface: 32 travelling waves on the tile's integer wave vectors (xy: cycles per tile; drawn at random, density
// ~ 1/|n| per octave), z = slope amplitude (amplitude x wavenumber), w = phase. Wavelengths 0.26..1.09 m, amplitude
// ~ k^-2.6, rms slope 0.12 (a light chop), deep-water dispersion w = sqrt(g k). A tile this size (2x the demo's 3.3 m)
// with this many waves shows no lattice; its repeats sit 6.6 m apart. Largest slope 0.76: landing offsets <= kFocus
// kRefr 0.76 = 0.76 m = 29.5 cells < kMargin.
const vec4 kWave[32] = vec4[](
    vec4(-6.0, 1.0, 0.063196, 0.7992), vec4(6.0, 2.0, 0.059375, 4.3816), vec4(-4.0, 5.0, 0.058214, 1.9397),
    vec4(5.0, 4.0, 0.058214, 2.1724), vec4(6.0, 3.0, 0.054036, 6.2608), vec4(4.0, 6.0, 0.048134, 1.9941),
    vec4(1.0, 8.0, 0.040264, 5.6934), vec4(4.0, 8.0, 0.034102, 0.9280), vec4(0.0, 9.0, 0.033765, 3.6938),
    vec4(-4.0, 9.0, 0.029230, 2.9462), vec4(9.0, 4.0, 0.029230, 1.0042), vec4(10.0, 4.0, 0.025333, 2.8885),
    vec4(-11.0, 3.0, 0.023126, 0.1060), vec4(2.0, 12.0, 0.020847, 4.3419), vec4(12.0, 8.0, 0.015878, 0.1545),
    vec4(-11.0, 11.0, 0.014067, 1.2706), vec4(2.0, 16.0, 0.013282, 5.3149), vec4(16.0, 5.0, 0.012482, 5.6639),
    vec4(-12.0, 12.0, 0.012239, 5.6294), vec4(9.0, 15.0, 0.011659, 1.0849), vec4(-12.0, 13.0, 0.011450, 5.2724),
    vec4(18.0, 1.0, 0.011111, 1.2615), vec4(-12.0, 14.0, 0.010717, 0.2078), vec4(-15.0, 11.0, 0.010568, 4.6448),
    vec4(-8.0, 18.0, 0.009642, 0.5607), vec4(-1.0, 20.0, 0.009392, 2.9299), vec4(-9.0, 20.0, 0.008120, 0.2139),
    vec4(-20.0, 12.0, 0.007358, 4.2309), vec4(0.0, 24.0, 0.007029, 2.7014), vec4(-7.0, 24.0, 0.006585, 2.1320),
    vec4(-16.0, 20.0, 0.006335, 1.3644), vec4(21.0, 15.0, 0.006259, 0.3435));
// The surface slope (grad h) at tile point x (metres) and time t: the splat's only height source.
vec2 agCausSlope(vec2 x, float t) {
    vec2 s = vec2(0.0);
    for (int i = 0; i < 32; ++i) {
        vec2 k = kWave[i].xy * (6.2831853 / kTile);
        float kl = length(k);
        s -= kWave[i].z * sin(dot(k, x) - sqrt(9.81 * kl) * t + kWave[i].w) * (k / kl);
    }
    return s;
}
// Where grid vertex c's light lands, in tile uv (texture coordinates of uAgWater's mip 0).
vec2 agCausLand(ivec2 c, float t) {
    vec2 x = vec2(c - kMargin) / float(kCells);
    return x + (kFocus * kRefr / kTile) * agCausSlope(x * kTile, t);
}
void main() {
    int tri = OU_VERTEX_ID / 3, corner = OU_VERTEX_ID - 3 * tri;
    ivec2 c = ivec2((tri >> 1) % kRow, (tri >> 1) / kRow);
    ivec2 o0 = (tri & 1) == 0 ? ivec2(0, 0) : ivec2(1, 0), o1 = (tri & 1) == 0 ? ivec2(1, 0) : ivec2(1, 1);
    float t = OU_AG_TIME;
    vec2 a = agCausLand(c + o0, t), b = agCausLand(c + o1, t), d = agCausLand(c + ivec2(0, 1), t);
    vec2 e = b - a, f = d - a;
    float landed = 0.5 * abs(e.x * f.y - e.y * f.x), source = 0.5 / float(kCells * kCells);
    vGain = min(source / max(landed, 1e-12), kPeak);
    vec2 p = corner == 0 ? a : corner == 1 ? b : d;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
