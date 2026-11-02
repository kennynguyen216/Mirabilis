#pragma once
#include "draw_instance.h"
#include <cstdint>

struct GPURayInstance {
    uint64_t vertices{}, indices{};
    glm::vec4 normalRows[3]{};
    glm::vec4 baseColor{}, emission{};
    glm::uvec4 geometry{}; // first index, index count, static-cache member, textured
};
static_assert(sizeof(GPURayInstance)==112 && offsetof(GPURayInstance,geometry)==96);
