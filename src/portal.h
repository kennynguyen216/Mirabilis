#pragma once

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "scene.h"
#include "world.h"

// Gameplay/rendering data for one placed portal. Linking and traversal come later.
struct Portal {
    bool placed{false};
    // Scene object this portal was placed on. It lets physics replace the
    // matching solid wall collider with a frame around this opening.
    SceneObjectID hostWallObject{InvalidSceneObject};
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    float halfWidth{0.8f};
    float halfHeight{1.2f};
};

// Projects a reference up vector onto the portal plane, guaranteeing that
// normal/up/right form an orthonormal portal frame.
inline void orient_portal(Portal& portal, const glm::vec3& normal)
{
    portal.normal = glm::normalize(normal);

    glm::vec3 referenceUp{0.0f, 1.0f, 0.0f};
    if (std::abs(glm::dot(referenceUp, portal.normal)) > 0.999f) {
        referenceUp = glm::vec3(0.0f, 0.0f, 1.0f);
    }

    portal.up = glm::normalize(referenceUp -
        portal.normal * glm::dot(referenceUp, portal.normal));
}

inline glm::mat4 get_portal_frame(const Portal& portal)
{
    const glm::vec3 right = glm::normalize(glm::cross(portal.up, portal.normal));
    glm::mat4 frame{1.0f};
    frame[0] = glm::vec4(right, 0.0f);
    frame[1] = glm::vec4(portal.up, 0.0f);
    frame[2] = glm::vec4(portal.normal, 0.0f);
    frame[3] = glm::vec4(portal.position, 1.0f);
    return frame;
}

// Maps a point or direction entering source to the matching frame at
// destination. The local 180-degree Y rotation makes the result exit from
// the front of the destination portal.
inline glm::mat4 get_portal_transfer_transform(
    const Portal& source,
    const Portal& destination)
{
    glm::mat4 portalFlip{1.0f};
    portalFlip[0][0] = -1.0f;
    portalFlip[2][2] = -1.0f;

    return get_portal_frame(destination) * portalFlip *
        glm::inverse(get_portal_frame(source));
}

inline glm::vec3 transform_position_through_portal(
    const Portal& source,
    const Portal& destination,
    const glm::vec3& position)
{
    const glm::vec4 transformed = get_portal_transfer_transform(source, destination) *
        glm::vec4(position, 1.0f);
    return glm::vec3(transformed);
}

inline glm::vec3 transform_direction_through_portal(
    const Portal& source,
    const Portal& destination,
    const glm::vec3& direction)
{
    const glm::vec4 transformed = get_portal_transfer_transform(source, destination) *
        glm::vec4(direction, 0.0f);
    return glm::vec3(transformed);
}

inline float portal_signed_distance(const Portal& portal, const glm::vec3& point)
{
    return glm::dot(point - portal.position, portal.normal);
}

// A view rendered through `source` is located on the back side of its linked
// destination portal.  Never let that *render-only* camera land exactly on
// the destination plane: the clip plane and the wall depth can otherwise
// disagree for a frame while the player crosses the portal.
inline glm::vec3 stabilize_portal_view_camera(
    const Portal& destination,
    const glm::vec3& virtualPosition)
{
    constexpr float MinimumDistanceBehindPlane = 0.05f;
    const float signedDistance = portal_signed_distance(destination, virtualPosition);
    if (signedDistance > -MinimumDistanceBehindPlane) {
        return virtualPosition - destination.normal *
            (signedDistance + MinimumDistanceBehindPlane);
    }
    return virtualPosition;
}

// Tests whether the player's collision body overlaps the portal opening when
// its leading face reaches the portal plane. Overlap (rather than a center or
// full-fit test) lets a player grazing the opening enter at shallow angles.
inline bool portal_overlaps_upright_player(
    const Portal& portal,
    const glm::vec3& playerFeetAtPlane,
    float playerHalfWidth,
    float playerHeight)
{
    constexpr float FitEpsilon = 0.05f;
    const glm::vec3 localFeet = glm::vec3(
        glm::inverse(get_portal_frame(portal)) *
        glm::vec4(playerFeetAtPlane, 1.0f));
    const float playerMinX = localFeet.x - playerHalfWidth;
    const float playerMaxX = localFeet.x + playerHalfWidth;
    const float playerMinY = localFeet.y;
    const float playerMaxY = localFeet.y + playerHeight;

    const bool overlapsHorizontally =
        playerMaxX >= -portal.halfWidth - FitEpsilon &&
        playerMinX <= portal.halfWidth + FitEpsilon;
    const bool overlapsVertically =
        playerMaxY >= -portal.halfHeight - FitEpsilon &&
        playerMinY <= portal.halfHeight + FitEpsilon;
    return overlapsHorizontally && overlapsVertically;
}

// Snaps a portal to the closest legal center on one vertical, axis-aligned
// wall. Placement fails only when the wall is physically too small.
inline bool snap_portal_to_wall(Portal& portal, const AABB& wall)
{
    // The level walls are still solid boxes rather than meshes with holes
    // cut for portals.  Keep an extra gap from a wall end so a portal's
    // virtual camera cannot begin inside the perpendicular wall at a corner.
    constexpr float CornerClearance = 1.0f;
    const float minCenterY = wall.min.y + portal.halfHeight;
    const float maxCenterY = wall.max.y - portal.halfHeight;
    if (minCenterY > maxCenterY) {
        return false;
    }

    // The bhop sandbox has a single ground plane at every wall's bottom edge.
    // Floor-aligning portals means the upright player can always fit through
    // them; their horizontal coordinate still comes from the placement ray.
    portal.position.y = minCenterY;

    if (std::abs(portal.normal.x) > 0.5f) {
        const float minCenterZ = wall.min.z + portal.halfWidth + CornerClearance;
        const float maxCenterZ = wall.max.z - portal.halfWidth - CornerClearance;
        if (minCenterZ > maxCenterZ) {
            return false;
        }
        portal.position.z = std::clamp(portal.position.z, minCenterZ, maxCenterZ);
        return true;
    }
    if (std::abs(portal.normal.z) > 0.5f) {
        const float minCenterX = wall.min.x + portal.halfWidth + CornerClearance;
        const float maxCenterX = wall.max.x - portal.halfWidth - CornerClearance;
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
