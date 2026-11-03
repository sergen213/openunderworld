#version 460
// Full-screen triangle (3 vertices, no buffers) for the splash blur and composite passes.
#ifdef VULKAN
#define OU_VERTEX_ID gl_VertexIndex
#else
#define OU_VERTEX_ID gl_VertexID
#endif
void main() {
    vec2 p = vec2((OU_VERTEX_ID << 1) & 2, OU_VERTEX_ID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
