#include "physics/Physics.hpp"
#include "cdc/Cdc.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <chrono>
#include <cstdio>
#include <set>
using namespace ou; using physics::vec3;
// Cost of the always-present terrain body per unit: Jolt mesh build (grouped for the controller / one mesh for loose
// bodies) against the BVH build the unit needs anyway.
int main(int argc, char** argv) {
    const cdc::BigfileSet bf(argv[1]);
    for (int i = 2; i < argc; ++i) {
        auto col = collision::loadLevelCollision(cdc::parseDrm(*bf.read(std::string("pc-w\\") + argv[i] + ".drm")));
        std::set<uint16_t> groups;
        for (const auto& f : col.faces) groups.insert(f.group);
        const auto ms = [](auto&& f) { const auto t = std::chrono::steady_clock::now(); f(); return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); };
        const std::vector<physics::Scene::Terrain> terrain{{argv[i], &col, vec3(0)}};
        const double bvh = ms([&] { col.build(); });
        physics::Scene::CharacterSpec spec{79.01f, 489.2f, 110, 110, 0.7f, 55};
        physics::Scene a;
        a.setController(&spec);
        const double grouped = ms([&] { a.setTerrain(terrain); });
        physics::ObjectPhysics o;
        o.segments = {glm::mat4(1)};
        physics::Prim p;
        p.kind = physics::PrimKind::Box, p.size = {50, 50, 50}, p.mass = 10;
        o.prims = {p};
        physics::Scene b;
        const std::vector<physics::Scene::Loose> loose{{1, &o, glm::mat4(1)}};
        b.setTerrain(terrain), b.setLoose(loose);
        const double plain = ms([&] { b.step(1 / 60.f); });
        std::printf("%s: %zu faces, %zu terrain groups; BVH build %.1f ms; Jolt terrain body: one mesh %.1f ms, per-group compound %.1f ms\n", argv[i], col.faces.size(),
                    groups.size(), bvh, plain, grouped);
    }
}
