#version 460
// Enhanced water's floor caustics (uw_caustics.vert): each landed triangle's gain, added into uAgWater's g channel
// (the pass writes only G, blend ONE + ONE, after uw_water.frag cleared g to 0).
layout(location = 0) flat in float vGain;
layout(location = 0) out vec4 oWater;
void main() { oWater = vec4(0.0, vGain, 0.0, 0.0); }
