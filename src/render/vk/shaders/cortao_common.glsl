// Shared by the CORTAO compute passes (cortao_*.comp; VkRenderer recordCortao, docs/RENDERING.md "Ambient occlusion").
// One descriptor set for all passes; each pass declares the bindings it uses.
#extension GL_EXT_control_flow_attributes : enable
#extension GL_EXT_samplerless_texture_functions : require
#define CORTAO_GLSL 1
#define CORTAO_PERSPECTIVE_PROJECTION 1
#define CORTAO_PACKED_NORMAL_BITS 12 // render::cortao::kPackedNormalBits
#define CORTAO_16_BIT 0

// Pre-pass targets (MSAA: sample 0, the sample the depth resolve keeps): depth, octahedral world normal (sun.glsl
// ouOctEncode, written by the depth pre-pass).
#ifdef OU_MS
#define OU_TEX2D texture2DMS
#else
#define OU_TEX2D texture2D
#endif

vec3 ouOctDecode(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

// World normal -> view space (rows of the view rotation), turned to face the viewer along `ray` (view space, toward the
// pixel): interpolated normals at silhouettes may point slightly away; CORTAO's spheremap needs z > -1.
vec3 ouViewNormal(vec2 e, vec4 rows[3], vec3 ray) {
    vec3 w = ouOctDecode(e);
    vec3 n = vec3(dot(rows[0].xyz, w), dot(rows[1].xyz, w), dot(rows[2].xyz, w));
    vec3 v = normalize(ray);
    float away = dot(n, v);
    if (away > -0.02) n = normalize(n - v * (away + 0.02));
    return n;
}
