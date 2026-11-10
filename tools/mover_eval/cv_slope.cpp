#include "physics/Physics.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <cstdlib>
#include <cmath>
using namespace ou; using physics::vec3;
// A ramp of normal z `nz` rising toward +x from x = 500: horizontal speed kept running up it at 1274 units/s, and
// how far the feet (root) stand above the ramp surface under them.
int main(int argc, char** argv) {
    for (int flat = 0; flat < 2; ++flat)
        for (float nz : {0.95f, 0.9f, 0.8f, 0.72f}) {
            physics::Scene::CharacterSpec spec{79.01f, 489.2f, 110, 110, 0.7f, 55};
            spec.flatBottom = flat;
            const float th = std::acos(nz);
            const glm::mat4 r = glm::rotate(glm::mat4(1), -th, {0, 1, 0});
            const vec3 centre = vec3(500, 0, 0) - vec3(r * glm::vec4(-3000, 0, 50, 0));
            collision::CollisionMesh m;
            collision::addBox(m, glm::translate(glm::mat4(1), {0, 0, -50}), {12000, 12000, 100});
            collision::addBox(m, glm::translate(glm::mat4(1), centre) * r, {6000, 2000, 100});
            m.build();
            physics::Scene s; s.setController(&spec);
            s.setTerrain(std::vector<physics::Scene::Terrain>{{"t", &m, vec3(0)}});
            vec3 p(0, 0, 0);
            float x0 = 0; int n = 0, off = 0; float hover = 0;
            for (int i = 0; i < 150; ++i) {
                auto q = s.moveCharacter(p, {1274, 0, 0}, 489.2f, 1 / 60.f, true);
                p = q.pos;
                off += q.ground != physics::Scene::Ground::OnGround;
                if (p.x > 900 && !n) x0 = p.x;
                if (p.x > 900) ++n, hover = p.z - (p.x - 500) * std::tan(th);
            }
            std::printf("%s nz %.2f (%.1f deg): horizontal speed %.0f of 1274, root %.1f above the ramp, off-ground ticks %d\n", flat ? "cylinder" : "capsule ", nz,
                        th * 57.29578f, n > 1 ? (p.x - x0) / (n - 1) * 60 : 0.f, hover, off);
        }
}
