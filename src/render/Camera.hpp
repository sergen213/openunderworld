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
    // World-space plane (normal, d): points with dot(normal, p) + d < 0 are clipped by an oblique near plane (Lengyel);
    // zero = none. The planar reflection camera (original::reflection) keeps only what lies above the water.
    glm::vec4 clipPlane{0.0f};

    glm::vec3 forward() const {
        return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
    }
    glm::vec3 right() const { return {std::sin(yaw), -std::cos(yaw), 0.0f}; }
    // Rotation from the direction itself, then the translation: glm::lookAt(position, position + forward()) rounds the
    // look point to the world position's float grid (~0.004 at 40000 units), turning the view by up to ~0.2 deg, a pixel
    // or more that changed every frame of a moving camera (Lara, centred by the orbit camera, shook sideways).
    glm::mat4 view() const { return glm::translate(glm::lookAt(glm::vec3(0.0f), forward(), up), -position); }
    glm::mat4 projection(float aspect) const {
        return clipped(jittered(glm::perspective(glm::radians(verticalFovDeg), aspect, nearZ, farZ)), false);
    }
    // translate(jitter) * p for a perspective p (p[2][3] = -1, p[3][3] = 0): exact no-op at zero jitter.
    glm::mat4 jittered(glm::mat4 p) const {
        p[2][0] -= jitter.x;
        p[2][1] -= jitter.y;
        return p;
    }
    // p with its near plane replaced by clipPlane (clip z in [-w, w], or [0, w] when zeroToOne); x, y and w rows keep.
    glm::mat4 clipped(glm::mat4 p, bool zeroToOne) const {
        if (clipPlane == glm::vec4(0.0f)) return p;
        const glm::vec4 c = glm::transpose(glm::inverse(view())) * clipPlane; // view space
        const glm::vec4 q = glm::inverse(p) * glm::vec4(c.x < 0 ? -1.0f : 1.0f, c.y < 0 ? -1.0f : 1.0f, 1.0f, 1.0f);
        const glm::vec4 s = c * ((zeroToOne ? 1.0f : 2.0f) / glm::dot(c, q));
        for (int k = 0; k < 4; ++k) p[k][2] = zeroToOne ? s[k] : s[k] - p[k][3];
        return p;
    }
    void lookAt(const glm::vec3& target) {
        const glm::vec3 v = target - position;
        if (glm::dot(v, v) < 1e-6f) return; // camera on its target (orbit sweep hit at t=0): keep the last direction, not a NaN view
        glm::vec3 d = v / std::sqrt(glm::dot(v, v));
        yaw = std::atan2(d.y, d.x);
        pitch = std::asin(glm::clamp(d.z, -1.0f, 1.0f));
    }
};

// Horizontal FOV (radians) implied by a vertical FOV (radians) at `aspect` = width/height.
inline float horizontalFov(float verticalFovRad, float aspect) {
    return 2.0f * std::atan(std::tan(verticalFovRad * 0.5f) * aspect);
}

} // namespace ou::render
