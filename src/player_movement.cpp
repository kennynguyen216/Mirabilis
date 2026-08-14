#include "player_movement.h"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>

void PlayerMovement::simulate(const PlayerInput& input, float deltaTime)
{
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

    // Resolve the player against the four boundary walls. The player is an
    // upright box whose position is at its feet; only the shallowest horizontal
    // overlap is resolved so approaching a wall slides along it.
    for (const AABB& wall : boundaryWalls) {
        const bool overlapsVertically =
            position.y < wall.max.y &&
            position.y + settings.playerHeight > wall.min.y;
        if (!overlapsVertically) {
            continue;
        }

        const float playerMinX = position.x - settings.playerRadius;
        const float playerMaxX = position.x + settings.playerRadius;
        const float playerMinZ = position.z - settings.playerRadius;
        const float playerMaxZ = position.z + settings.playerRadius;

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
        velocity = glm::vec3(0.0f);
        grounded = false;
        jumpBufferRemaining = 0.0f;
        return;
    }

    const bool overFloor =
        std::abs(position.x) <= settings.floorHalfExtent &&
        std::abs(position.z) <= settings.floorHalfExtent;
    grounded = false;
    if (overFloor && position.y <= settings.groundHeight) {
        position.y = settings.groundHeight;
        if (velocity.y < 0.0f) {
            velocity.y = 0.0f;
        }
        grounded = true;
    }
}
