// Backend-neutral camera. World is Z-up (cdc engine convention [INFERENCE], see docs).
// Projection is Hor+: the vertical FOV is fixed, horizontal FOV widens with aspect ratio,
// so 21:9 / 32:9 screens see more to the sides instead of cropping top/bottom.
#pragma once

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace ou::render {

struct Camera {
    glm::vec3 position{0.0f};
    float yaw = 0.0f;    // radians, around +Z; 0 looks down +X
    float pitch = 0.0f;  // radians, clamped to +-89 deg by users
    float verticalFovDeg = 60.0f;
    float nearZ = 1.0f, farZ = 100000.0f;
    glm::vec3 up{0.0f, 0.0f, 1.0f}; // view up; cinematic cameras roll
    // Clip-space sub-pixel offset of the projection (FSR jitter: 2 * pixels / size, y up); 0 = none.
    glm::vec2 jitter{0.0f};

    glm::vec3 forward() const {
        return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
    }
    glm::vec3 right() const { return {std::sin(yaw), -std::cos(yaw), 0.0f}; }
    glm::mat4 view() const { return glm::lookAt(position, position + forward(), up); }
    glm::mat4 projection(float aspect) const { return jittered(glm::perspective(glm::radians(verticalFovDeg), aspect, nearZ, farZ)); }
    // translate(jitter) * p for a perspective p (p[2][3] = -1, p[3][3] = 0): exact no-op at zero jitter.
    glm::mat4 jittered(glm::mat4 p) const {
        p[2][0] -= jitter.x;
        p[2][1] -= jitter.y;
        return p;
    }
    void lookAt(const glm::vec3& target) {
        glm::vec3 d = glm::normalize(target - position);
        yaw = std::atan2(d.y, d.x);
        pitch = std::asin(glm::clamp(d.z, -1.0f, 1.0f));
    }
};

// Horizontal FOV (radians) implied by a vertical FOV (radians) at `aspect` = width/height.
inline float horizontalFov(float verticalFovRad, float aspect) {
    return 2.0f * std::atan(std::tan(verticalFovRad * 0.5f) * aspect);
}

} // namespace ou::render
