#include "physics/Physics.hpp"
#include "cdc/Cdc.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace ou; using physics::vec3;
// A run in a real unit: cv_real GAMEDIR unit x y z dirx diry ticks flat innerEdges stepForwardTest padding [trace]
int main(int argc, char** argv) {
    const cdc::BigfileSet bf(argv[1]);
    auto col = collision::loadLevelCollision(cdc::parseDrm(*bf.read(std::string("pc-w\\") + argv[2] + ".drm")));
    vec3 p(atof(argv[3]), atof(argv[4]), atof(argv[5]));
    const vec3 v = glm::normalize(vec3(atof(argv[6]), atof(argv[7]), 0)) * 1274.0f;
    const int ticks = atoi(argv[8]);
    physics::Scene::CharacterSpec spec{79.01f, 489.2f, 110, 110, 0.7f, 55};
    spec.flatBottom = atoi(argv[9]), spec.innerEdges = atoi(argv[10]), spec.stepForwardTest = atof(argv[11]), spec.padding = atof(argv[12]);
    physics::Scene s; s.setController(&spec);
    // OU_UNIT2="name x y z": a second resident unit at its stream offset (ou-game logs them)
    collision::CollisionMesh col2;
    std::vector<physics::Scene::Terrain> terrain{{argv[2], &col, vec3(0)}};
    if (const char* u = std::getenv("OU_UNIT2")) {
        char name[64]; float x, y, z;
        std::sscanf(u, "%63s %f %f %f", name, &x, &y, &z);
        col2 = collision::loadLevelCollision(cdc::parseDrm(*bf.read(std::string("pc-w\\") + name + ".drm")));
        terrain.push_back({name, &col2, vec3(x, y, z)});
    }
    s.setTerrain(terrain);
    const vec3 start = p;
    int off = 0, slow = 0;
    for (int i = 0; i < ticks; ++i) {
        const auto r = s.moveCharacter(p, v, 489.2f, 1 / 60.f, true);
        const float d = glm::length(glm::vec2(r.pos - p));
        if (argc > 13) std::printf("  %d %.1f %.1f %.2f moved %.1f g %d wall %d\n", i, r.pos.x, r.pos.y, r.pos.z, d, int(r.ground), r.hitWall);
        off += r.ground != physics::Scene::Ground::OnGround, slow += d < 15;
        p = r.pos;
    }
    std::printf("flat %d edges %d fwdtest %.0f pad %.1f: went %.0f of %.0f, z %.1f -> %.1f, off-ground ticks %d, slow ticks %d\n", spec.flatBottom, spec.innerEdges,
                spec.stepForwardTest, spec.padding, glm::length(glm::vec2(p - start)), 1274.0f / 60 * ticks, start.z, p.z, off, slow);
}
