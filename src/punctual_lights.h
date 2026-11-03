#pragma once
#include <glm/glm.hpp>
#include <array>
#include <cstdint>

// Color stores incident irradiance / pi, matching the raster direct-light convention.
struct PunctualLight {
    glm::vec4 position{}, color{}, direction{}, cone{};
};
struct PunctualLights {
    glm::uvec4 count{};
    std::array<PunctualLight, 16> lights{};
};
static_assert(sizeof(PunctualLight) == 64 && sizeof(PunctualLights) == 1040);
