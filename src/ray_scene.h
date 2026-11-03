#pragma once
#include "draw_instance.h"
#include <cstdint>
#include <span>
#include <vector>
#include <algorithm>

struct RayGeometryDraw {
    uint64_t vertices{}, indices{};
    glm::mat4 transform{1};
    glm::uvec2 range{};
};
struct RayGeometryGroup {
    uint64_t vertices{}, indices{};
    glm::mat4 transform{1};
    std::vector<glm::uvec2> ranges;
    std::vector<size_t> draws;
};
inline std::vector<RayGeometryGroup> group_ray_geometry(std::span<const RayGeometryDraw> draws) {
    std::vector<RayGeometryGroup> groups;
    for (size_t i=0;i<draws.size();++i) {
        const auto& draw=draws[i];
        auto group=std::find_if(groups.begin(),groups.end(),[&](const auto& candidate) {
            return candidate.vertices==draw.vertices && candidate.indices==draw.indices && candidate.transform==draw.transform;
        });
        if (group==groups.end()) {
            groups.push_back({draw.vertices,draw.indices,draw.transform}); group=groups.end()-1;
        }
        group->ranges.push_back(draw.range); group->draws.push_back(i);
    }
    return groups;
}

struct GPURayInstance {
    uint64_t vertices{}, indices{};
    glm::vec4 normalRows[3]{};
    glm::vec4 baseColor{}, emission{};
    glm::uvec4 geometry{}; // first index, index count, static-cache member, textured
};
static_assert(sizeof(GPURayInstance)==112 && offsetof(GPURayInstance,geometry)==96);
