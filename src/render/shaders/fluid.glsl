// Splash fluid particles (render/Fluid.hpp, docs/RENDERING.md "Splash fluid"): the pool fluid.comp simulates and
// splash.vert draws. Shared by both; spliced in for GL (render::fluid::computeSource, render/Water.cpp expandIncludes).
#ifndef OU_FLUID_GLSL
#define OU_FLUID_GLSL
struct OuFluidParticle {
    vec4 x; // xyz position at the step's start (units), w life left (s; <= 0: dead)
    vec4 v; // xyz velocity (units/s), w the rest surface z of the water it falls back into
    vec4 p; // xyz predicted position (the solver's), w lambda
    vec4 d; // xyz the solver's other position / the step's final position, w density / rest density (spray < ~0.35)
};
const float kOuFluidSpacing = 7.0;                  // render::fluid::kSpacing
const float kOuFluidRadius = 0.8 * kOuFluidSpacing; // drawn sphere radius (the blur bridges the gaps)
// Spray: a particle with few neighbours (density ratio under ~0.35) reads as aerated white water, not clear liquid.
float ouFluidFoam(float density) { return 1.0 - smoothstep(0.15, 0.45, density); }
#endif
