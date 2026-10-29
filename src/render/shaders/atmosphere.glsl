// Enhanced atmosphere + weather, post pass (after tonemap / brightness, before the underwater effect).
// Written for OpenUnderworld; technique list and credits in docs/RENDERING.md "Atmosphere" / "Weather".
// Included after underwater.glsl and render/sun.glsl: uses the block `w`, uDepth, ouWorld and the sun shadow map. Returns `c` unchanged when both the
// atmosphere and the weather are off, or the camera is submerged (the underwater effect owns that view).
float ouHashCell(vec2 cell, float salt) { // integer hash of a cell (+ salt) -> [0, 1)
    uvec2 q = uvec2(ivec2(floor(cell)) + ivec2(int(salt) * 7919, int(salt) * 104729));
    uint n = (q.x * 1597334677u) ^ (q.y * 3812015801u);
    n = (n ^ (n >> 15)) * 2246822519u;
    return float(n >> 8) * (1.0 / 16777216.0);
}
// Nearest hit of the ray (o + t dir, |dir| = 1) on a water volume's top face, seen from above; 1e30 = none.
float ouWaterSurfaceT(vec3 o, vec3 dir) {
    float t = 1e30;
    for (int i = 0; i < int(w.detail.w); ++i) {
        vec3 lo = w.volumes[2 * i].xyz, hi = w.volumes[2 * i + 1].xyz;
        if (o.z <= hi.z || dir.z > -1e-5) continue;
        float s = (hi.z - o.z) / dir.z;
        vec2 p = o.xy + dir.xy * s;
        if (all(greaterThanEqual(p, lo.xy)) && all(lessThanEqual(p, hi.xy))) t = min(t, s);
    }
    return t;
}
// Optical depth of a height layer (x density, y falloff, z base height) along `len` units of a ray from height z0 to
// z1: density * exp(-falloff * (z - base)) above the base, constant density below it (pits and caves stay finite).
float ouLayerDepth(vec4 m, float z0, float z1, float len) {
    float lo = min(z0, z1), hi = max(z0, z1), dz = hi - lo;
    if (dz < 1e-2) return m.x * len * exp(-max(z0 - m.z, 0.0) * m.y);
    float below = clamp((m.z - lo) / dz, 0.0, 1.0);
    float a = max(lo, m.z);
    return m.x * len * (below + (exp(-(a - m.z) * m.y) - exp(-(hi - m.z) * m.y)) / (m.y * dz));
}
float ouPhase(float cosT, float g) { // Henyey-Greenstein, normalised over the sphere
    float g2 = g * g;
    return (1.0 - g2) / (12.566371 * pow(max(1.0 + g2 - 2.0 * g * cosT, 1e-4), 1.5));
}
// Sun shadow map visibility at p, one compare tap in the finest cascade holding it (1 outside all cascades).
float ouSunTap(vec3 p) {
    for (int c = 0; c < 3; ++c) {
        vec3 q = (ouShadow.cascade[c] * vec4(p, 1.0)).xyz;
        if (all(greaterThan(q, vec3(0.002, 0.002, 0.0))) && all(lessThan(q, vec3(0.998, 0.998, 1.0))))
            return texture(ouShadowMap, vec3((float(c) + q.x) / 3.0, q.y, q.z));
    }
    return 1.0;
}
// One precipitation layer on a camera-centred cylinder of radius `radius`; azimuth and world height anchor it, so
// turning the camera does not drag it along. Returns (rgb premultiplied, alpha).
vec4 ouPrecipLayer(vec3 dir, float sceneT, float radius, float layer, bool snow, float amount) {
    float horiz = max(length(dir.xy), 1e-3);
    float t = radius / horiz;
    float z = w.eye.z + dir.z * t;
    float az = atan(dir.y, dir.x);
    float cellX = snow ? 26.0 : 7.0, cellZ = snow ? 26.0 : 150.0;
    float speed = snow ? 85.0 : 1100.0;
    float around = floor(6.2831853 * radius / cellX);
    float fall = z + w.eye.w * speed + layer * 977.0;
    vec2 tangent = vec2(-sin(az), cos(az));
    float slant = dot(w.weather.zw, tangent) * (snow ? 0.6 : 1.0);
    vec2 g = vec2(az / 6.2831853 * around + slant * fall / cellX, fall / cellZ);
    float aa = max(fwidth(g.x), 1e-4); // before any per-pixel return: derivatives need uniform control flow
    if (t >= sceneT || t > 8.0 * radius) return vec4(0); // behind the scene (roofs, walls) or too steep
    vec2 cell = floor(g), f = fract(g);
    cell.x = mod(cell.x, around);
    float rnd = ouHashCell(cell, layer);
    if (rnd > amount) return vec4(0);
    float x0 = 0.2 + 0.6 * ouHashCell(cell, layer + 31.0);
    float a;
    vec3 col;
    if (snow) {
        x0 += 0.18 * sin(w.eye.w * (0.9 + rnd) + rnd * 40.0);
        float y0 = 0.25 + 0.5 * ouHashCell(cell, layer + 57.0);
        float r = 0.05 + 0.05 * rnd, d = length(vec2(f.x - x0, (f.y - y0) * cellZ / cellX));
        a = (1.0 - smoothstep(r * 0.4, r + aa, d)) * 0.85;
        col = vec3(0.92, 0.95, 1.0);
    } else {
        float width = max(0.07, aa);
        float len = 0.35 + 0.3 * rnd;
        a = (1.0 - smoothstep(0.0, width, abs(f.x - x0))) * (0.07 / width) *
            smoothstep(0.0, 0.08, f.y) * (1.0 - smoothstep(len - 0.2, len, f.y)) * 0.32;
        col = vec3(0.70, 0.75, 0.80);
    }
    a *= exp(-t / (snow ? 1800.0 : 2600.0));
    return vec4(col * a, a);
}
// Precipitation layers, near to far (depth-tested against the scene): premultiplied colour (lit) + coverage. Also the
// FSR reactivity source (vk/shaders/fsr_weather.frag).
vec4 ouPrecipitation(vec3 dir, float sceneT, bool snow, float amount) {
    const float radii[4] = float[4](110.0, 240.0, 500.0, 1000.0);
    float density = snow ? 0.12 + 0.4 * amount : 0.25 + 0.55 * amount;
    float light = 0.55 + 0.45 * clamp(dot(w.fogColor.rgb, vec3(0.3, 0.59, 0.11)) * 2.0, 0.0, 1.0);
    vec4 acc = vec4(0);
    for (int i = 0; i < 4; ++i) {
        vec4 l = ouPrecipLayer(dir, sceneT, radii[i], float(i), snow, density);
        acc += l * (1.0 - acc.a);
    }
    return vec4(acc.rgb * light, acc.a);
}
vec3 ouAtmosphere(vec3 c, vec2 uv) {
    if ((w.haze.w < 0.5 && w.weather.x < 0.5) || w.fogColor.w > 0.5) return c;
    float d = texture(uDepth, uv).r;
    bool sky = d >= 0.999999;
    vec3 p = ouWorld(uv, d, w.invViewProj);
    vec3 dpx = dFdx(p), dpy = dFdy(p); // surface normal for rain splashes (uniform control flow here)
    vec3 ray = ouWorld(uv, 1.0, w.invViewProj) - w.eye.xyz;
    vec3 dir = normalize(sky ? ray : p - w.eye.xyz);
    float sceneT = sky ? 1e30 : length(p - w.eye.xyz);
    float waterT = ouWaterSurfaceT(w.eye.xyz, dir);
    float airT = min(sceneT, waterT); // the medium ends at the scene or at the water surface
    const float far = 60000.0; // medium depth [I]; TRU fog ranges reach 1e6 units and would fog everything
    float amount = w.weather.y;
    bool rain = w.weather.x > 0.5 && w.weather.x < 1.5, snow = w.weather.x > 1.5;

    // Rain: splash rings and darkening on upward-facing surfaces (water surfaces count as upward).
    if (rain && airT < 2500.0) {
        vec3 sp = waterT < sceneT ? w.eye.xyz + dir * waterT : p;
        vec3 n = waterT < sceneT ? vec3(0, 0, 1) : normalize(cross(dpx, dpy));
        if (n.z < 0.0) n = -n;
        float up = smoothstep(0.7, 0.9, n.z) * (1.0 - airT / 2500.0);
        if (up > 0.0) {
            const float cell = 34.0;
            vec2 id = floor(sp.xy / cell);
            float r0 = ouHashCell(id, 3.0);
            float phase = fract(w.eye.w * 1.7 + r0);
            vec2 centre = (id + 0.25 + 0.5 * vec2(r0, ouHashCell(id, 5.0))) * cell;
            float ring = 1.0 - smoothstep(0.0, 1.6, abs(length(sp.xy - centre) - phase * cell * 0.4));
            c *= 1.0 - 0.14 * amount * up;
            c += vec3(0.55, 0.6, 0.65) * (ring * (1.0 - phase) * 0.45 * amount * up * step(r0, 0.25 + 0.6 * amount));
        }
    }

    // Height fog: haze + ground mist, in-scattering the unit's fog colour plus forward-scattered sunlight.
    float cosSun = dot(dir, w.sunDir.xyz);
    if (w.haze.w > 0.5) {
        float len = min(airT, far);
        float z1 = w.eye.z + dir.z * len;
        float od = ouLayerDepth(w.haze, w.eye.z, z1, len) + ouLayerDepth(w.mist, w.eye.z, z1, len);
        float trans = exp(-min(od, 12.0));
        vec3 scatter = w.fogColor.rgb;
        if (w.mist.w > 0.5) scatter += w.sunColor.rgb * (ouPhase(cosSun, 0.6) * 1.6);
        c = c * trans + scatter * (1.0 - trans);

        // Sun shafts: march the view ray through the sun shadow map (render/sun.glsl; one compare tap per step)
        // over its first 9000 units; lit air in-scatters the sun (HG g 0.75), shadowed air does not. Off-screen
        // occluders count too. Needs the shadow map (ouShadow.sun.w); 16 steps, 32 at High, jittered per pixel.
        if (w.mist.w > 1.5 && ouShadow.sun.w != 0.0) {
            int steps = w.haze.w > 2.5 ? 32 : 16;
            float march = min(airT, 9000.0), dt = march / float(steps);
            float t = dt * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
            float lit = 0.0;
            for (int i = 0; i < steps; ++i, t += dt) lit += ouSunTap(w.eye.xyz + dir * t);
            float air = 1.0 - exp(-march / 7000.0);
            c += w.sunColor.rgb * (lit / float(steps) * air * ouPhase(cosSun, 0.7) * 0.3);
        }
    }

    // Precipitation layers, near to far (depth-tested against the scene).
    if (rain || snow) {
        vec4 pr = ouPrecipitation(dir, sceneT, snow, amount);
        c = c * (1.0 - pr.a) + pr.rgb;
    }
    return c;
}
