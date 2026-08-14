#pragma once

#include <array>
#include <glm/vec3.hpp>

struct PlayerInput {
    bool forward{false};
    bool backward{false};
    bool left{false};
    bool right{false};
    bool jumpPressed{false};
    float yaw{0.0f};
};

// Axis-aligned collision box, defined by opposite world-space corners.
struct AABB {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

struct PlayerMovementSettings {
    float gravity{20.0f};
    float groundHeight{0.0f};

    // The player's collision body is centered on position.x/z, with position.y at its feet.
    float playerRadius{0.3f};
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

    // These match the four 2-unit-high visual boundary walls around the floor.
    std::array<AABB, 4> boundaryWalls{{
        {{-25.0f, 0.0f, -25.0f}, {25.0f, 2.0f, -24.5f}}, // north
        {{-25.0f, 0.0f,  24.5f}, {25.0f, 2.0f,  25.0f}}, // south
        {{-25.0f, 0.0f, -25.0f}, {-24.5f, 2.0f, 25.0f}}, // west
        {{ 24.5f, 0.0f, -25.0f}, { 25.0f, 2.0f, 25.0f}}, // east
    }};

    void simulate(const PlayerInput& input, float deltaTime);
};
