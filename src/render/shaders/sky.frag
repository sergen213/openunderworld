#version 460
// Time of day backdrop pass (render/TimeOfDay.hpp): mode 0 (tint.w) outputs the tint, which the blend multiplies into the
// exterior terrain and the empty sky (dst x src); mode 1 outputs the stars and the moon, added (src + dst). World z up.
#ifdef VULKAN
layout(push_constant) uniform SkyPush { vec4 right, up, forward, tint, stars, moon; } pc;
#else
layout(std140, binding = 5) uniform SkyPush { vec4 right, up, forward, tint, stars, moon; } pc;
#endif
layout(location = 0) in vec3 vRay;
layout(location = 0) out vec4 oColor;

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}
vec3 hash33(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

// Point stars on a 3D lattice around the view sphere: about one cell in 9 holds a star placed inside the cell's middle
// (so no star is cut by a cell border), sized to ~1.5 pixels of the current view whatever the resolution.
vec3 stars(vec3 d, float pixel) {
    const float kCells = 150.0;
    const vec3 p = d * kCells, cell = floor(p);
    const float h = hash13(cell);
    if (h > 0.11) return vec3(0.0);
    const vec3 star = cell + 0.25 + 0.5 * hash33(cell);
    const float dist = length(p - normalize(star) * kCells) / kCells; // radians
    const float size = max(1.5 * pixel, 0.0006);
    const float mag = pow(hash13(cell + 17.0), 3.0);                 // a few bright, many faint
    const float twinkle = 0.75 + 0.25 * sin(pc.stars.z * (1.5 + 3.0 * h) + 40.0 * h);
    const vec3 tint = mix(vec3(0.75, 0.82, 1.0), vec3(1.0, 0.9, 0.75), hash13(cell + 5.0));
    return tint * ((0.25 + 1.6 * mag) * twinkle * exp(-dist * dist / (size * size)));
}

void main() {
    if (pc.tint.w < 0.5) {
        oColor = vec4(pc.tint.rgb, 1.0);
        return;
    }
    const vec3 d = normalize(vRay);
    const float pixel = length(fwidth(vRay)) / length(vRay);
    vec3 c = vec3(0.0);
    const float horizon = smoothstep(-0.02, 0.18, d.z); // haze near the horizon hides the faint ones
    if (pc.stars.x > 0.0) c += stars(d, pixel) * (pc.stars.x * horizon);
    if (pc.stars.y > 0.0) {
        const float cosA = dot(d, normalize(pc.moon.xyz));
        const float a = acos(clamp(cosA, -1.0, 1.0));
        const float kRadius = 0.0105; // radians: twice the real moon, as games draw it
        const float disc = 1.0 - smoothstep(kRadius - max(pixel, 0.0004), kRadius, a);
        const float glow = exp(-a * 18.0) * 0.18 + exp(-a * 90.0) * 0.25;
        c += vec3(0.86, 0.9, 1.0) * ((disc * 0.95 + glow) * pc.stars.y * smoothstep(-0.03, 0.05, d.z));
    }
    oColor = vec4(c, 1.0);
}
