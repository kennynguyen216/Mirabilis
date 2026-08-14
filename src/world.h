#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

// Axis-aligned collision box, defined by opposite world-space corners.
struct AABB {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

struct RaycastHit {
    float distance{0.0f};
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
};

// Tests a normalized ray against an axis-aligned box with the slab method.
// The returned normal points outward from the face the ray entered through.
inline std::optional<RaycastHit> raycast_aabb(
    const glm::vec3& origin,
    const glm::vec3& direction,
    const AABB& box)
{
    constexpr float DirectionEpsilon = 0.000001f;
    if (glm::dot(direction, direction) < DirectionEpsilon * DirectionEpsilon) {
        return std::nullopt;
    }

    float nearDistance = -std::numeric_limits<float>::infinity();
    float farDistance = std::numeric_limits<float>::infinity();
    glm::vec3 nearNormal{0.0f};
    glm::vec3 farNormal{0.0f};

    for (int axis = 0; axis < 3; axis++) {
        const float originAxis = origin[axis];
        const float directionAxis = direction[axis];
        if (std::abs(directionAxis) < DirectionEpsilon) {
            if (originAxis < box.min[axis] || originAxis > box.max[axis]) {
                return std::nullopt;
            }
            continue;
        }

        float firstDistance = (box.min[axis] - originAxis) / directionAxis;
        float secondDistance = (box.max[axis] - originAxis) / directionAxis;

        glm::vec3 firstNormal{0.0f};
        glm::vec3 secondNormal{0.0f};
        firstNormal[axis] = -1.0f;
        secondNormal[axis] = 1.0f;
        if (firstDistance > secondDistance) {
            std::swap(firstDistance, secondDistance);
            std::swap(firstNormal, secondNormal);
        }

        if (firstDistance > nearDistance) {
            nearDistance = firstDistance;
            nearNormal = firstNormal;
        }
        if (secondDistance < farDistance) {
            farDistance = secondDistance;
            farNormal = secondNormal;
        }
        if (nearDistance > farDistance) {
            return std::nullopt;
        }
    }

    const bool startsInsideBox = nearDistance < 0.0f;
    const float hitDistance = startsInsideBox ? farDistance : nearDistance;
    if (hitDistance < 0.0f || !std::isfinite(hitDistance)) {
        return std::nullopt;
    }

    return RaycastHit{
        .distance = hitDistance,
        .position = origin + direction * hitDistance,
        .normal = startsInsideBox ? farNormal : nearNormal,
    };
}

// Static level object. Rendering and physics both derive their data from this.
struct Wall {
    glm::vec3 position{0.0f};
    glm::vec3 halfExtents{0.0f};
};

inline AABB get_aabb(const Wall& wall)
{
    return {
        .min = wall.position - wall.halfExtents,
        .max = wall.position + wall.halfExtents,
    };
}
