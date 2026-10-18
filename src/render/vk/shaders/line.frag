#version 460
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 oColor;
void main() { oColor = vec4(vColor.rgb, 1.0); }
