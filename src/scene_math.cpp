#include "scene_types.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
glm::mat4 to_matrix(const Transform& transform)
{
    // Yaw, then pitch, then roll: the same order the camera uses.
    const glm::mat4 rotation =
        glm::rotate(glm::mat4(1.0f), transform.rotation.y, glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::rotate(glm::mat4(1.0f), transform.rotation.x, glm::vec3(1.0f, 0.0f, 0.0f)) *
        glm::rotate(glm::mat4(1.0f), transform.rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));

    return glm::translate(glm::mat4(1.0f), transform.position) * rotation *
        glm::scale(glm::mat4(1.0f), transform.scale);
}

// Inverse of scene.h's Y * X * Z Euler convention.  ImGuizmo gives us a
// modified matrix; scene objects store editable components, so recover those
// components after each drag instead of keeping a second matrix representation.
Transform transform_from_matrix(const glm::mat4& matrix)
{
    constexpr float Epsilon = 0.00001f;
    Transform transform{};
    transform.position = glm::vec3(matrix[3]);

    glm::mat3 rotation{matrix};
    transform.scale = glm::vec3(
        glm::length(rotation[0]),
        glm::length(rotation[1]),
        glm::length(rotation[2]));
    if (transform.scale.x < Epsilon || transform.scale.y < Epsilon ||
        transform.scale.z < Epsilon) {
        return transform;
    }
    rotation[0] /= transform.scale.x;
    rotation[1] /= transform.scale.y;
    rotation[2] /= transform.scale.z;

    // For R = Ry(yaw) * Rx(pitch) * Rz(roll):
    // pitch = asin(-R[1][2]), yaw = atan2(R[0][2], R[2][2]),
    // roll = atan2(R[1][0], R[1][1]). GLM uses column-major indexing.
    transform.rotation.x = std::asin(std::clamp(-rotation[2][1], -1.0f, 1.0f));
    const float cosPitch = std::cos(transform.rotation.x);
    if (std::abs(cosPitch) > Epsilon) {
        transform.rotation.y = std::atan2(rotation[2][0], rotation[2][2]);
        transform.rotation.z = std::atan2(rotation[0][1], rotation[1][1]);
    } else {
        // At the singularity yaw and roll describe the same degree of
        // freedom. Preserve a stable, editable representation.
        transform.rotation.y = std::atan2(-rotation[0][2], rotation[0][0]);
        transform.rotation.z = 0.0f;
    }
    return transform;
}
