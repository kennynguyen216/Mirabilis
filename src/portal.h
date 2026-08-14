#pragma once

#include <algorithm>
#include <cmath>

#include <glm/vec3.hpp>

#include "world.h"

// Gameplay/rendering data for one placed portal. Linking and traversal come later.
struct Portal {
    bool placed{false};
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    float halfWidth{0.8f};
    float halfHeight{1.2f};
};

// Snaps a portal to the closest legal center on one vertical, axis-aligned
// wall. Placement fails only when the wall is physically too small.
inline bool snap_portal_to_wall(Portal& portal, const AABB& wall)
{
    constexpr float EdgeMargin = 0.001f;
    const float minCenterY = wall.min.y + portal.halfHeight + EdgeMargin;
    const float maxCenterY = wall.max.y - portal.halfHeight - EdgeMargin;
    if (minCenterY > maxCenterY) {
        return false;
    }
    portal.position.y = std::clamp(portal.position.y, minCenterY, maxCenterY);

    if (std::abs(portal.normal.x) > 0.5f) {
        const float minCenterZ = wall.min.z + portal.halfWidth + EdgeMargin;
        const float maxCenterZ = wall.max.z - portal.halfWidth - EdgeMargin;
        if (minCenterZ > maxCenterZ) {
            return false;
        }
        portal.position.z = std::clamp(portal.position.z, minCenterZ, maxCenterZ);
        return true;
    }
    if (std::abs(portal.normal.z) > 0.5f) {
        const float minCenterX = wall.min.x + portal.halfWidth + EdgeMargin;
        const float maxCenterX = wall.max.x - portal.halfWidth - EdgeMargin;
        if (minCenterX > maxCenterX) {
            return false;
        }
        portal.position.x = std::clamp(portal.position.x, minCenterX, maxCenterX);
        return true;
    }

    return false;
}

// The placement prototype only permits one portal rectangle per area of a
// wall face. Perpendicular walls may still each contain a portal.
inline bool portals_overlap(const Portal& first, const Portal& second)
{
    if (!first.placed || !second.placed) {
        return false;
    }

    const float normalAlignment = std::abs(glm::dot(first.normal, second.normal));
    if (normalAlignment < 0.999f) {
        return false;
    }

    const glm::vec3 betweenCenters = first.position - second.position;
    if (std::abs(glm::dot(betweenCenters, first.normal)) > 0.05f) {
        return false;
    }

    const bool overlapsVertically =
        std::abs(first.position.y - second.position.y) <
        first.halfHeight + second.halfHeight;
    if (!overlapsVertically) {
        return false;
    }

    if (std::abs(first.normal.x) > 0.5f) {
        return std::abs(first.position.z - second.position.z) <
            first.halfWidth + second.halfWidth;
    }
    if (std::abs(first.normal.z) > 0.5f) {
        return std::abs(first.position.x - second.position.x) <
            first.halfWidth + second.halfWidth;
    }

    return false;
}
