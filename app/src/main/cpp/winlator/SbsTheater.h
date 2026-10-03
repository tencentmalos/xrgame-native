#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Fixed, parallel eyes viewing the existing flat desktop as a world-space quad.
// No guest stereo rendering, readback, extra image, or cross-queue synchronization.
struct SbsTheater {
    bool enabled = false;
    float widthMeters = 1.6f;
    float distanceMeters = 2.0f;

    void set(bool on, float width, float distance) {
        enabled = on;
        widthMeters = std::isfinite(width) ? std::clamp(width, 0.8f, 4.8f) : 1.6f;
        distanceMeters = std::isfinite(distance) ? std::clamp(distance, 1.0f, 5.0f) : 2.0f;
    }

    struct Eye {
        uint32_t x, width;
        float scaleX, scaleY, offsetX;
        float projectX(float ndc) const { return ndc * scaleX + offsetX; }
        float projectY(float ndc) const { return ndc * scaleY; }
    };

    Eye eye(uint32_t fullWidth, int index) const {
        if (!enabled || fullWidth < 2) return {0, fullWidth, 1, 1, 0};
        const uint32_t leftWidth = fullWidth / 2;
        const uint32_t eyeWidth = index == 0 ? leftWidth : fullWidth - leftWidth;
        // 60-degree horizontal FOV per eye, 64 mm IPD. Preserve square pixels and
        // the aspect of the composed desktop, including its existing letterbox.
        constexpr float focal = 1.7320508075688772f; // cot(30 degrees)
        const float scaleX = widthMeters * 0.5f * focal / distanceMeters;
        const float scaleY = scaleX * static_cast<float>(eyeWidth) / std::max(1u, fullWidth);
        const float offset = 0.032f * focal / distanceMeters;
        return {index == 0 ? 0u : leftWidth, eyeWidth, scaleX, scaleY, index == 0 ? offset : -offset};
    }
};
