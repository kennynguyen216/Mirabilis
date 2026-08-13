#pragma once

#include <vk_types.h>
#include <SDL_events.h>

class Camera {
public:
    glm::vec3 velocity{0.0f};
    glm::vec3 position{0.0f};

    // Vertical and horizontal FPS-style rotation, in radians.
    float pitch{0.0f};
    float yaw{0.0f};
    float moveSpeed{5.0f};

    glm::mat4 getViewMatrix() const;
    glm::mat4 getRotationMatrix() const;

    void processSDLEvent(const SDL_Event& e);
    void update(float deltaTime);
};
