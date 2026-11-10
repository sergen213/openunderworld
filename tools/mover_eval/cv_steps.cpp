#include "physics/Physics.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <cstdlib>
#include <cmath>
using namespace ou; using physics::vec3;
// floor; box1 x 500..1500 top h1; box2 x 1500..2500 top h1+h2. Walk +x at speed: report where she ends.
int main(int argc, char** argv) {
    float h1 = atof(argv[1]), h2 = atof(argv[2]), speed = atof(argv[3]);
    physics::Scene::CharacterSpec spec{79.01f, 489.2f, 110, 110, 0.7f, 55};
    spec.innerEdges = atoi(argv[4]);
    if (argc > 6) spec.padding = atof(argv[6]);
    if (argc > 7) spec.flatBottom = atoi(argv[7]);
    bool trace = argc > 5 && atoi(argv[5]);
    collision::CollisionMesh m;
    collision::addBox(m, glm::translate(glm::mat4(1), {0, 0, -50}), {12000, 12000, 100});
    if (h1 > 0) collision::addBox(m, glm::translate(glm::mat4(1), {1000, 0, h1 / 2}), {1000, 4000, h1});
    collision::addBox(m, glm::translate(glm::mat4(1), {2000, 0, (h1 + h2) / 2}), {1000, 4000, h1 + h2});
    m.build();
    physics::Scene s; s.setController(&spec);
    s.setTerrain(std::vector<physics::Scene::Terrain>{{"t", &m, vec3(0)}});
    const bool back = argc > 8 && atoi(argv[8]);
    vec3 p(back ? 1900 : 300, 0, back ? h1 + h2 : 0);
    if (back) speed = -speed;
    int air = 0;
    for (int i = 0; i < int(1700 / std::abs(speed) * 60); ++i) {
        auto r = s.moveCharacter(p, {speed, 0, 0}, 489.2f, 1 / 60.f, true);
        if (trace && (std::abs(r.pos.z - p.z) > 0.01f || int(r.ground))) printf("  %d x %.2f -> %.2f z %.2f -> %.2f g %d wall %d\n", i, p.x, r.pos.x, p.z, r.pos.z, int(r.ground), r.hitWall);
        p = r.pos;
        air += r.ground != physics::Scene::Ground::OnGround;
    }
    printf("h1 %.0f h2 %.0f speed %.0f edges %d: x %.2f z %.2f offGroundTicks %d\n", h1, h2, speed, spec.innerEdges, p.x, p.z, air);
}
