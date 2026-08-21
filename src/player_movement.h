#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>
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

// A horizontal walkable surface. The current movement sandbox intentionally
// keeps these axis-aligned; sloped surfaces need plane/triangle collision.
struct GroundPlane {
    glm::vec2 center{0.0f};
    glm::vec2 halfExtents{0.5f};
    float height{0.0f};
};

// The ramp mesh rises from local y = 0 at z = -0.5 to local y = 1 at z = 0.5.
// Keeping its world transform lets the physics derive the same sloped plane
// the renderer draws, including scale and rotation.
struct SurfRamp {
    glm::mat4 worldToLocal{1.0f};
    glm::vec3 lowLeft{0.0f};
    glm::vec3 lowRight{0.0f};
    glm::vec3 highLeft{0.0f};
};

struct PlayerMovementSettings {
    float gravity{20.0f};
    float groundHeight{0.0f};

    // The player's square collision body is centered on position.x/z, with position.y at its feet.
    float playerHalfWidth{0.3f};
    float playerHeight{1.8f};

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
    glm::vec3 previousPosition{0.0f};
    glm::vec3 velocity{0.0f};
    bool grounded{false};
    float jumpBufferRemaining{0.0f};
    PlayerMovementSettings settings{};

    // Advances player-controlled movement, but deliberately does not resolve
    // world geometry. Portal traversal will run after this step.
    void integrate(const PlayerInput& input, float deltaTime);
    void resolve_world_collision(
        std::span<const AABB> collisionWalls,
        std::span<const GroundPlane> groundPlanes,
        std::span<const SurfRamp> surfRamps);
};
