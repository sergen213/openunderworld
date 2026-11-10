// Lara's Jolt mover (Tuning::mover; docs/formats/PHYSICS.md "Character controller"): ground, air and slope-slide moves
// through physics::Scene's character controller (a Jolt CharacterVirtual) in place of the capsule sweep on the BVH.
// The state chart, the clips' root motion, the air integrator (tru.exe 0x5DB370) and every markup query stay Player's.
#include "game/Game.hpp"

#include <glm/glm.hpp>

namespace ou::game {
namespace {
vec2 xy(vec3 v) { return {v.x, v.y}; }
} // namespace

// The scene she moves in, with her controller: World's (attach), or her own over col_ (a Player without a world),
// which follows col_'s dynamics once per tick.
physics::Scene& Player::moverScene(float dt) {
    if (!scene_) ownScene_ = std::make_unique<physics::Scene>(), scene_ = ownScene_.get();
    physics::Scene::CharacterSpec spec;
    spec.radius = t_.radius, spec.top = t_.capsuleTop;
    spec.stepUp = t_.stepUp, spec.stepDown = t_.stepDown;
    spec.maxSlopeCos = t_.minFloorNormalZ;
    spec.mass = 55.0f; // her ragdoll record's mass, as her proxy's (World::stepPhysics)
    spec.flatBottom = t_.mover == Mover::Jolt;
    scene_->setController(&spec);
    if (ownScene_ && !moverSynced_) {
        const physics::Scene::Terrain terrain{"", &col_, vec3(0)};
        ownScene_->setTerrain({&terrain, 1});
        ownScene_->setColliders(col_.dynamics);
        ownScene_->step(dt); // the kinematic colliders reach their transforms
        moverSynced_ = true;
    }
    return *scene_;
}

// Root motion as a velocity (delta / dt, no smoothing) into Jolt's extended update: slide along walls, climb risers up
// to stepUp, follow the floor down to stepDown. Off the ground she slides (a steep floor) or falls.
void Player::groundMoveJolt(vec3 delta, float dt) {
    const auto r = moverScene(dt).moveCharacter(s_.pos, delta / dt, capHeight(), dt, true);
    s_.pos = r.pos;
    if (r.ground == physics::Scene::Ground::OnGround) return;
    const vec3 vel(xy(delta) / dt, 0);
    if (r.ground == physics::Scene::Ground::Steep && r.groundNormal.z >= t_.slideMinNormalZ) startSlide(vel);
    else startFall(vel);
}

// The air step: our integration, Jolt's contacts (they clip the velocity: ceilings stop the rise, walls the run-up).
// Ground under her while not rising lands her; a steep floor starts the slide.
bool Player::airMoveJolt(float dt, vec2 move) {
    const vec3 d = s_.vel * dt + vec3(0, 0, -0.5f * t_.gravity * dt * dt);
    s_.vel.z -= t_.gravity * dt;
    const bool falling = s_.vel.z <= 0;
    const auto r = moverScene(dt).moveCharacter(s_.pos, d / dt, capHeight(), dt, false);
    s_.pos = r.pos;
    for (const vec3& n : r.contacts) s_.vel -= n * std::min(0.0f, glm::dot(s_.vel, n));
    if (!falling) return false;
    if (r.ground == physics::Scene::Ground::OnGround) { land(move); return true; }
    if (r.ground == physics::Scene::Ground::Steep && r.groundNormal.z >= t_.slideMinNormalZ) { startSlide(s_.vel); return true; }
    return false;
}

// State::Slide: down the fall line of the ground Jolt reports under her, accelerated as the sweep mover's slide
// (g sin(theta), no friction, no cap); walkable ground ends it, none falls. The move runs at the velocity the last
// tick's ground gave; this tick's ground gives the next.
void Player::slideJolt(vec2 move, float dt) {
    const auto r = moverScene(dt).moveCharacter(s_.pos, s_.vel, capHeight(), dt, true);
    s_.pos = r.pos;
    if (r.ground == physics::Scene::Ground::Air || r.ground == physics::Scene::Ground::Unsupported) { startFall(s_.vel); return; }
    const float dl = glm::length(xy(r.groundNormal));
    if (r.ground == physics::Scene::Ground::OnGround || dl < 1e-4f) { land(move); return; }
    const vec2 dir = xy(r.groundNormal) / dl; // the normal leans downhill
    s_.yaw = std::atan2(dir.y, dir.x);
    const float nz = std::max(r.groundNormal.z, 1e-3f), along = std::max(0.0f, glm::dot(xy(s_.vel), dir)) / nz;
    const float speed = (along + t_.gravity * std::sqrt(1 - nz * nz) * dt) * nz; // horizontal part of the along-slope speed
    s_.vel = vec3(dir * speed, -speed * std::sqrt(1 - nz * nz) / nz);          // z: the descent, carried into a fall
}

} // namespace ou::game
