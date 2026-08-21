#include "player_movement.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <glm/geometric.hpp>
#include <glm/vec4.hpp>

void PlayerMovement::integrate(const PlayerInput& input, float deltaTime)
{
    previousPosition = position;
    if (input.jumpPressed) {
        jumpBufferRemaining = settings.jumpBufferSeconds;
    } else {
        jumpBufferRemaining = std::max(jumpBufferRemaining - deltaTime, 0.0f);
    }

    if (grounded && jumpBufferRemaining > 0.0f) {
        velocity.y = settings.jumpSpeed;
        grounded = false;
        jumpBufferRemaining = 0.0f;
    }

    glm::vec3 horizontalVelocity{velocity.x, 0.0f, velocity.z};
    const float forwardMove = (input.forward ? 1.0f : 0.0f) -
        (input.backward ? 1.0f : 0.0f);
    const float rightMove = (input.right ? 1.0f : 0.0f) -
        (input.left ? 1.0f : 0.0f);
    const glm::vec3 forward{std::sin(input.yaw), 0.0f, -std::cos(input.yaw)};
    const glm::vec3 right{std::cos(input.yaw), 0.0f, std::sin(input.yaw)};
    glm::vec3 wishDirection = forward * forwardMove + right * rightMove;
    if (glm::length(wishDirection) > 0.0f) {
        wishDirection = glm::normalize(wishDirection);
    }

    if (grounded) {
        const float speed = glm::length(horizontalVelocity);
        if (speed > 0.0f) {
            const float control = std::max(speed, settings.stopSpeed);
            const float drop = control * settings.groundFriction * deltaTime;
            horizontalVelocity *= std::max(speed - drop, 0.0f) / speed;
        }

        if (glm::length(wishDirection) > 0.0f) {
            const float currentSpeed = glm::dot(horizontalVelocity, wishDirection);
            const float addSpeed = settings.maxGroundSpeed - currentSpeed;
            if (addSpeed > 0.0f) {
                const float accelSpeed = settings.groundAcceleration * deltaTime *
                    settings.maxGroundSpeed;
                horizontalVelocity += wishDirection * std::min(accelSpeed, addSpeed);
            }
        }
    } else if (glm::length(wishDirection) > 0.0f) {
        const float currentSpeed = glm::dot(horizontalVelocity, wishDirection);
        const float addSpeed = settings.airWishSpeedCap - currentSpeed;
        if (addSpeed > 0.0f) {
            const float accelSpeed = settings.airAcceleration * deltaTime *
                settings.airWishSpeedCap;
            horizontalVelocity += wishDirection * std::min(accelSpeed, addSpeed);
        }
    }

    velocity.x = horizontalVelocity.x;
    velocity.z = horizontalVelocity.z;
    velocity.y -= settings.gravity * deltaTime;
    position += velocity * deltaTime;
}

void PlayerMovement::resolve_world_collision(
    std::span<const AABB> collisionWalls,
    std::span<const GroundPlane> groundPlanes,
    std::span<const SurfRamp> surfRamps)
{
    // Resolve the player against the four boundary walls. The player is an
    // upright box whose position is at its feet; only the shallowest horizontal
    // overlap is resolved so approaching a wall slides along it.
    for (const AABB& wall : collisionWalls) {
        const bool overlapsVertically =
            position.y < wall.max.y &&
            position.y + settings.playerHeight > wall.min.y;
        if (!overlapsVertically) {
            continue;
        }

        const float playerMinX = position.x - settings.playerHalfWidth;
        const float playerMaxX = position.x + settings.playerHalfWidth;
        const float playerMinZ = position.z - settings.playerHalfWidth;
        const float playerMaxZ = position.z + settings.playerHalfWidth;

        const float overlapX = std::min(playerMaxX, wall.max.x) -
            std::max(playerMinX, wall.min.x);
        const float overlapZ = std::min(playerMaxZ, wall.max.z) -
            std::max(playerMinZ, wall.min.z);
        if (overlapX <= 0.0f || overlapZ <= 0.0f) {
            continue;
        }

        const glm::vec3 wallCenter = (wall.min + wall.max) * 0.5f;
        if (overlapX < overlapZ) {
            if (position.x < wallCenter.x) {
                position.x -= overlapX;
                velocity.x = std::min(velocity.x, 0.0f);
            } else {
                position.x += overlapX;
                velocity.x = std::max(velocity.x, 0.0f);
            }
        } else {
            if (position.z < wallCenter.z) {
                position.z -= overlapZ;
                velocity.z = std::min(velocity.z, 0.0f);
            } else {
                position.z += overlapZ;
                velocity.z = std::max(velocity.z, 0.0f);
            }
        }
    }

    if (position.y < settings.respawnHeight) {
        position = settings.spawnPosition;
        previousPosition = position;
        velocity = glm::vec3(0.0f);
        grounded = false;
        jumpBufferRemaining = 0.0f;
        return;
    }

    grounded = false;
    float landingHeight = -std::numeric_limits<float>::infinity();
    glm::vec3 landingNormal{0.0f, 1.0f, 0.0f};
    for (const GroundPlane& ground : groundPlanes) {
        const bool overlapsGround =
            std::abs(position.x - ground.center.x) <=
                ground.halfExtents.x + settings.playerHalfWidth &&
            std::abs(position.z - ground.center.y) <=
                ground.halfExtents.y + settings.playerHalfWidth;
        const bool crossedFromAbove =
            previousPosition.y >= ground.height && position.y <= ground.height;
        if (overlapsGround && crossedFromAbove && ground.height > landingHeight) {
            landingHeight = ground.height;
            landingNormal = glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }

    for (const SurfRamp& ramp : surfRamps) {
        glm::vec3 normal = glm::cross(
            ramp.lowRight - ramp.lowLeft, ramp.highLeft - ramp.lowLeft);
        if (glm::length(normal) < 0.0001f || std::abs(normal.y) < 0.0001f) {
            continue;
        }
        normal = glm::normalize(normal);
        if (normal.y < 0.0f) {
            normal = -normal;
        }

        const float planeD = -glm::dot(normal, ramp.lowLeft);
        const auto height_on_ramp = [&](const glm::vec3& point) {
            return (-planeD - normal.x * point.x - normal.z * point.z) / normal.y;
        };
        const float currentHeight = height_on_ramp(position);
        const float previousHeight = height_on_ramp(previousPosition);
        const glm::vec3 surfacePoint{position.x, currentHeight, position.z};
        const glm::vec3 localSurfacePoint = glm::vec3(
            ramp.worldToLocal * glm::vec4(surfacePoint, 1.0f));
        const bool overRamp =
            std::abs(localSurfacePoint.x) <= 0.5f + settings.playerHalfWidth &&
            localSurfacePoint.z >= -0.5f - settings.playerHalfWidth &&
            localSurfacePoint.z <= 0.5f + settings.playerHalfWidth;
        const bool crossedFromAbove =
            previousPosition.y >= previousHeight && position.y <= currentHeight;
        if (overRamp && crossedFromAbove && currentHeight > landingHeight) {
            landingHeight = currentHeight;
            landingNormal = normal;
        }
    }

    if (std::isfinite(landingHeight)) {
        position.y = landingHeight;
        const float velocityIntoSurface = glm::dot(velocity, landingNormal);
        if (velocityIntoSurface < 0.0f) {
            velocity -= landingNormal * velocityIntoSurface;
        }
        // Steep ramps are intentionally not treated as walkable ground: the
        // player keeps air control and avoids ground friction while gravity's
        // tangent component carries them down the slope, like a surf ramp.
        // A 45-degree ramp has a vertical normal component of about 0.707.
        // Keep the cutoff above that so the default 45-degree Surf Ramp is
        // handled as a surf surface rather than sticky walkable ground.
        constexpr float WalkableNormalY = 0.75f;
        grounded = landingNormal.y >= WalkableNormalY;
    }
}
