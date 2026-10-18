#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/FrameClock.hpp"
#include "render/Camera.hpp"
#include "scene/Placeholder.hpp"

using ou::engine::FrameClock;
using ou::render::Camera;

TEST(FrameClock, AccumulatesFixedSteps) {
    FrameClock c(1.0 / 60.0);
    EXPECT_EQ(c.advance(0.010), 0);
    EXPECT_NEAR(c.alpha(), 0.6, 1e-9);
    EXPECT_EQ(c.advance(0.010), 1); // 20 ms total -> one tick, 3.33 ms left
    EXPECT_NEAR(c.alpha(), (0.020 - 1.0 / 60.0) * 60.0, 1e-9);
    EXPECT_EQ(c.advance(0.050), 3);
    EXPECT_EQ(c.ticks(), 4u);
}

TEST(FrameClock, ExactlyOneSecondIsSixtyTicksAtAnyFrameRate) {
    for (int fps : {30, 60, 144, 240, 1000}) {
        FrameClock c;
        int ticks = 0;
        for (int f = 0; f < fps; ++f) ticks += c.advance(1.0 / fps);
        // floating-point accumulation may leave the 60th tick a hair short
        EXPECT_TRUE(ticks == 60 || (ticks == 59 && c.alpha() > 0.999)) << fps << " fps -> " << ticks;
        EXPECT_GE(c.alpha(), 0.0);
        EXPECT_LT(c.alpha(), 1.0);
    }
}

TEST(FrameClock, ClampsHitchesAndIgnoresNegativeTime) {
    FrameClock c(1.0 / 60.0, 8);
    EXPECT_EQ(c.advance(5.0), 8); // 300 due, clamped, excess dropped
    EXPECT_EQ(c.droppedFrames(), 1u);
    EXPECT_DOUBLE_EQ(c.alpha(), 0.0);
    EXPECT_EQ(c.advance(-1.0), 0);
    EXPECT_DOUBLE_EQ(c.alpha(), 0.0);
}

TEST(Camera, HorPlusKeepsVerticalFovAcrossAspects) {
    Camera cam;
    cam.verticalFovDeg = 60.0f;
    const float v = glm::radians(60.0f);
    const float f = 1.0f / std::tan(v / 2);
    for (float aspect : {4.0f / 3.0f, 16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f}) {
        glm::mat4 p = cam.projection(aspect);
        EXPECT_NEAR(p[1][1], f, 1e-5f);          // vertical extent fixed
        EXPECT_NEAR(p[0][0], f / aspect, 1e-5f); // horizontal widens
    }
    // 16:9 at 60 vFOV is the well-known ~91.49 deg hFOV; 21:9 and 32:9 widen further.
    EXPECT_NEAR(glm::degrees(ou::render::horizontalFov(v, 16.0f / 9.0f)), 91.4928f, 1e-3f);
    EXPECT_NEAR(glm::degrees(ou::render::horizontalFov(v, 21.0f / 9.0f)), 106.8264f, 1e-3f);
    EXPECT_NEAR(glm::degrees(ou::render::horizontalFov(v, 32.0f / 9.0f)), 128.0549f, 1e-3f);
}

TEST(Camera, ProjectsPointsAtFovEdgeToNdcEdge) {
    Camera cam;
    cam.position = {0, 0, 0};
    cam.yaw = 0; // look down +X, Z up
    const float aspect = 32.0f / 9.0f;
    const float h = ou::render::horizontalFov(glm::radians(cam.verticalFovDeg), aspect);
    // A point on the right edge of the horizontal FOV: +X forward, -Y is right.
    glm::vec4 p = cam.projection(aspect) * cam.view() * glm::vec4(100.0f, -100.0f * std::tan(h / 2), 0, 1);
    EXPECT_NEAR(p.x / p.w, 1.0f, 1e-4f);
    EXPECT_NEAR(p.y / p.w, 0.0f, 1e-4f);
    // Top edge of vertical FOV.
    p = cam.projection(aspect) * cam.view() *
        glm::vec4(100.0f, 0, 100.0f * std::tan(glm::radians(cam.verticalFovDeg) / 2), 1);
    EXPECT_NEAR(p.y / p.w, 1.0f, 1e-4f);
}

TEST(Camera, LookAtRoundTrips) {
    Camera cam;
    cam.position = {1, 2, 3};
    cam.lookAt({4, -2, 8});
    glm::vec3 d = glm::normalize(glm::vec3(3, -4, 5));
    EXPECT_NEAR(glm::length(cam.forward() - d), 0.0f, 1e-5f);
    EXPECT_NEAR(glm::dot(cam.right(), cam.forward()), 0.0f, 1e-5f);
}

TEST(Placeholder, SceneIsWellFormed) {
    auto s = ou::scene::makePlaceholderScene();
    EXPECT_TRUE(s.placeholder);
    ASSERT_EQ(s.textures.size(), 3u);
    for (const auto& t : s.textures) {
        EXPECT_EQ(t.origin.rfind("placeholder:", 0), 0u);
        ASSERT_FALSE(t.mips.empty());
        for (size_t i = 0; i < t.mips.size(); ++i) {
            const auto& m = t.mips[i];
            EXPECT_EQ(m.width, std::max(1u, t.mips[0].width >> i));
            size_t expect = t.format == ou::scene::PixelFormat::BC1 ? ((m.width + 3) / 4) * ((m.height + 3) / 4) * 8
                                                                    : size_t(m.width) * m.height * 4;
            EXPECT_EQ(m.data.size(), expect);
        }
        EXPECT_EQ(t.mips.back().width, 1u);
    }
    ASSERT_EQ(s.meshes.size(), 26u);
    for (const auto& m : s.meshes) {
        for (uint32_t i : m.indices) EXPECT_LT(i, m.vertices.size());
        for (const auto& sm : m.submeshes) EXPECT_LE(sm.firstIndex + sm.indexCount, m.indices.size());
    }
    // Every triangle is counter-clockwise seen from the side its normal points to.
    auto ccwOutward = [](const ou::scene::MeshData& m) {
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            const auto& a = m.vertices[m.indices[i]];
            const auto& b = m.vertices[m.indices[i + 1]];
            const auto& c = m.vertices[m.indices[i + 2]];
            glm::vec3 pa(a.position[0], a.position[1], a.position[2]);
            glm::vec3 pb(b.position[0], b.position[1], b.position[2]);
            glm::vec3 pc(c.position[0], c.position[1], c.position[2]);
            if (glm::dot(glm::cross(pb - pa, pc - pa), glm::vec3(a.normal[0], a.normal[1], a.normal[2])) <= 0)
                return false;
        }
        return true;
    };
    for (const auto& m : s.meshes) EXPECT_TRUE(ccwOutward(m)) << m.origin;
    auto gallery = ou::scene::makeTextureGallery(s.textures);
    for (const auto& m : gallery) EXPECT_TRUE(ccwOutward(m)) << m.origin;
    ASSERT_EQ(gallery.size(), 3u);
    EXPECT_EQ(gallery[2].submeshes[0].textureId, 3u);
}
