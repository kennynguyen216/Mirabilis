#include "player_movement.h"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>

namespace {
constexpr float Gravity = 20.0f;
constexpr float GroundHeight = 0.0f;
constexpr float MaxGroundSpeed = 8.0f;
constexpr float GroundAcceleration = 50.0f;
constexpr float GroundFriction = 8.0f;
constexpr float StopSpeed = 2.0f;
constexpr float JumpSpeed = 8.0f;
constexpr float AirAcceleration = 16.0f;
constexpr float AirWishSpeedCap = 4.0f;
constexpr float JumpBufferSeconds = 0.1f;
}

void PlayerMovement::simulate(const PlayerInput& input, float deltaTime)
{
    if (input.jumpPressed) {
        jumpBufferRemaining = JumpBufferSeconds;
    } else {
        jumpBufferRemaining = std::max(jumpBufferRemaining - deltaTime, 0.0f);
    }

    if (grounded && jumpBufferRemaining > 0.0f) {
        velocity.y = JumpSpeed;
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
            const float control = std::max(speed, StopSpeed);
            const float drop = control * GroundFriction * deltaTime;
            horizontalVelocity *= std::max(speed - drop, 0.0f) / speed;
        }

        if (glm::length(wishDirection) > 0.0f) {
            const float currentSpeed = glm::dot(horizontalVelocity, wishDirection);
            const float addSpeed = MaxGroundSpeed - currentSpeed;
            if (addSpeed > 0.0f) {
                const float accelSpeed = GroundAcceleration * deltaTime * MaxGroundSpeed;
                horizontalVelocity += wishDirection * std::min(accelSpeed, addSpeed);
            }
        }
    } else if (glm::length(wishDirection) > 0.0f) {
        const float currentSpeed = glm::dot(horizontalVelocity, wishDirection);
        const float addSpeed = AirWishSpeedCap - currentSpeed;
        if (addSpeed > 0.0f) {
            const float accelSpeed = AirAcceleration * deltaTime * AirWishSpeedCap;
            horizontalVelocity += wishDirection * std::min(accelSpeed, addSpeed);
        }
    }

    velocity.x = horizontalVelocity.x;
    velocity.z = horizontalVelocity.z;
    velocity.y -= Gravity * deltaTime;
    position += velocity * deltaTime;

    grounded = false;
    if (position.y <= GroundHeight) {
        position.y = GroundHeight;
        if (velocity.y < 0.0f) {
            velocity.y = 0.0f;
        }
        grounded = true;
    }
}
