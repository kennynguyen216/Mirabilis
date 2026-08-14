#pragma once

#include <glm/vec3.hpp>

struct PlayerInput {
    bool forward{false};
    bool backward{false};
    bool left{false};
    bool right{false};
    bool jumpPressed{false};
    float yaw{0.0f};
};

struct PlayerMovement {
    // World-space feet/collider position. The camera eye position comes later.
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    bool grounded{false};
    float jumpBufferRemaining{0.0f};

    void simulate(const PlayerInput& input, float deltaTime);
};
