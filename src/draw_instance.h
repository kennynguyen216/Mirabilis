#pragma once
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

// std430 layout mirrored by draw_instance.glsl. Kept out of push constants:
// transforms, motion and normal matrices must fit the portable 128-byte limit.
struct GPUDrawInstance {
    glm::mat4 world{1};
    glm::vec4 previousRows[3]{};
    glm::vec4 normalRows[3]{};
    glm::vec4 flags{}; // x double-sided, y determinant sign
};
static_assert(sizeof(GPUDrawInstance) == 176);

inline GPUDrawInstance make_draw_instance(const glm::mat4 &world, const glm::mat4 &previous,
                                          bool doubleSided) {
    GPUDrawInstance instance;
    instance.world = world;
    const glm::mat3 linear(world);
    const float determinant = glm::determinant(linear);
    // Zero scale is legal for hidden/editor-authored geometry. Avoid NaNs in the vertex shader.
    const glm::mat3 normal = std::isfinite(determinant) && std::abs(determinant) > 1e-12f
                                 ? glm::transpose(glm::inverse(linear))
                                 : glm::mat3(1);
    for (int row = 0; row < 3; ++row) {
        instance.previousRows[row] =
            glm::vec4(previous[0][row], previous[1][row], previous[2][row], previous[3][row]);
        instance.normalRows[row] = glm::vec4(normal[0][row], normal[1][row], normal[2][row], 0);
    }
    instance.flags = glm::vec4(doubleSided ? 1.f : 0.f, determinant < 0 ? -1.f : 1.f, 0, 0);
    return instance;
}
