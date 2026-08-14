#pragma once

#include <glm/vec3.hpp>
#include <span>

#include "world.h"

struct PlayerInput {
    bool forward{false};
    bool backward{false};
    bool left{false};
    bool right{false};
    bool jumpPressed{false};
    float yaw{0.0f};
};

struct PlayerMovementSettings {
    float gravity{20.0f};
    float groundHeight{0.0f};

    // The player's square collision body is centered on position.x/z, with position.y at its feet.
    float playerHalfWidth{0.3f};
    float playerHeight{1.8f};

    float floorHalfExtent{25.0f};
    float respawnHeight{-20.0f};
    glm::vec3 spawnPosition{0.0f};

    float maxGroundSpeed{8.0f};
    float groundAcceleration{50.0f};
    float groundFriction{8.0f};
    float stopSpeed{2.0f};
    float jumpSpeed{8.0f};
    float airAcceleration{16.0f};
    float airWishSpeedCap{4.0f};
    float jumpBufferSeconds{0.1f};
};

struct PlayerMovement {
    // World-space feet/collider position. The camera eye position comes later.
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    bool grounded{false};
    float jumpBufferRemaining{0.0f};
    PlayerMovementSettings settings{};

    void simulate(
        const PlayerInput& input,
        float deltaTime,
        std::span<const AABB> collisionWalls);
};
