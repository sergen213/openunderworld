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
const float kOuFluidRadius = 1.3 * kOuFluidSpacing; // drawn sphere radius: neighbours overlap into one sheet
// How white a particle reads, from its density ratio (rho / rho0; a lone particle's own weight is 0.19 of rho0, render::
// fluid::restDensity): spray (a lone drop or a pair, under ~0.3) is aerated white water; a thin sheet (a crown's two
// layers, ~0.4-0.7) carries a little air and catches light; the bulk is clear.
float ouFluidSpray(float density) { return 1.0 - smoothstep(0.21, 0.32, density); }
float ouFluidFoam(float density) { return max(ouFluidSpray(density), 0.25 * (1.0 - smoothstep(0.45, 0.85, density))); }
#endif
