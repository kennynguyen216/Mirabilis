#pragma once

#include "vk_engine.h"

#include <glm/geometric.hpp>

bool is_visible(const RenderObject& object, const glm::mat4& viewProjection);

inline glm::vec3 normalized_sun_direction(const glm::vec3& direction)
{
    if (glm::dot(direction, direction) < 0.000001f) {
        return glm::normalize(glm::vec3(0.0f, 1.0f, 0.5f));
    }
    return glm::normalize(direction);
}
