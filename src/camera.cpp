#include "camera.h"

#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/transform.hpp>

void Camera::update(float deltaTime)
{
    // Velocity is expressed in camera-local space, so rotate it before
    // applying it to the world-space position.
    const glm::mat4 cameraRotation = getRotationMatrix();
    position += glm::vec3(
        cameraRotation * glm::vec4(velocity * moveSpeed * deltaTime, 0.0f));
}

void Camera::processSDLEvent(const SDL_Event& e)
{
    if (e.type == SDL_KEYDOWN && e.key.repeat == 0) {
        if (e.key.keysym.sym == SDLK_w) velocity.z = -1.0f;
        if (e.key.keysym.sym == SDLK_s) velocity.z = 1.0f;
        if (e.key.keysym.sym == SDLK_a) velocity.x = -1.0f;
        if (e.key.keysym.sym == SDLK_d) velocity.x = 1.0f;
    }

    if (e.type == SDL_KEYUP) {
        if (e.key.keysym.sym == SDLK_w || e.key.keysym.sym == SDLK_s) {
            velocity.z = 0.0f;
        }
        if (e.key.keysym.sym == SDLK_a || e.key.keysym.sym == SDLK_d) {
            velocity.x = 0.0f;
        }
    }

    if (e.type == SDL_MOUSEMOTION) {
        yaw += static_cast<float>(e.motion.xrel) / 200.0f;
        pitch -= static_cast<float>(e.motion.yrel) / 200.0f;
    }
}

glm::mat4 Camera::getViewMatrix() const
{
    // Rendering moves the world opposite to the camera, so invert the
    // camera's world transform to obtain the view matrix.
    const glm::mat4 cameraTranslation = glm::translate(glm::mat4(1.0f), position);
    return glm::inverse(cameraTranslation * getRotationMatrix());
}

glm::mat4 Camera::getRotationMatrix() const
{
    const glm::quat pitchRotation =
        glm::angleAxis(pitch, glm::vec3{1.0f, 0.0f, 0.0f});
    const glm::quat yawRotation =
        glm::angleAxis(yaw, glm::vec3{0.0f, -1.0f, 0.0f});

    return glm::toMat4(yawRotation) * glm::toMat4(pitchRotation);
}
