#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <vector>
#include <cmath>
#include <limits>
#include <glm/mat4x4.hpp>
#include <glm/common.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/vec3.hpp>
#include <glm/vec2.hpp>

// Exact direction groups let a rejected facing test skip cards without losing coverage.
inline void append_surface_cache_candidates(std::span<const glm::vec3> directions,
    std::vector<uint32_t>& candidates, std::vector<glm::uvec2>& output, bool skipGroups)
{
    const auto key = [&](uint32_t card) {
        const glm::vec3 direction = directions[card];
        return std::array{std::bit_cast<uint32_t>(direction.x),
            std::bit_cast<uint32_t>(direction.y), std::bit_cast<uint32_t>(direction.z)};
    };
    std::sort(candidates.begin(), candidates.end(), [&](uint32_t a, uint32_t b) {
        return key(a) == key(b) ? a < b : key(a) < key(b);
    });
    const uint32_t base = static_cast<uint32_t>(output.size());
    for (size_t first = 0; first < candidates.size();) {
        size_t end = first + 1;
        while (end < candidates.size() && key(candidates[first]) == key(candidates[end])) ++end;
        for (size_t i = first; i < end; ++i)
            output.emplace_back(candidates[i], base + static_cast<uint32_t>(skipGroups ? end : i + 1));
        first = end;
    }
}

// Bound whole texel footprints, including every depth that can survive the blend cutoff.
template<class Emit>
bool surface_card_occupied_bounds(std::span<const uint16_t> depth,uint32_t atlasWidth,
    glm::uvec4 rect,const glm::mat4& cardToWorld,float worldDepth,
    float frontTolerance,float backTolerance,Emit emit)
{
    if (!atlasWidth || !rect.z || !rect.w || !(worldDepth>0) || !std::isfinite(worldDepth) ||
        uint64_t(rect.x)+rect.z>atlasWidth || uint64_t(rect.y)+rect.w>depth.size()/atlasWidth) return false;
    constexpr uint32_t Tile=8;
    for (uint32_t y=0;y<rect.w;y+=Tile) for (uint32_t x=0;x<rect.z;x+=Tile) {
        const uint32_t endX=std::min(x+Tile,rect.z),endY=std::min(y+Tile,rect.w);
        float minimum=1.f,maximum=-std::numeric_limits<float>::infinity();
        for (uint32_t row=y;row<endY;++row) for (uint32_t column=x;column<endX;++column) {
            const float value=glm::unpackHalf1x16(depth[size_t(rect.y+row)*atlasWidth+rect.x+column]);
            if (!std::isfinite(value)) return false;
            if (value<1.f) { minimum=std::min(minimum,value);maximum=std::max(maximum,value); }
        }
        if (maximum<minimum) continue;
        glm::vec3 lo(std::numeric_limits<float>::max()),hi(std::numeric_limits<float>::lowest());
        for (int corner=0;corner<8;++corner) {
            const glm::vec3 point=glm::vec3(cardToWorld*glm::vec4(
                float(corner&1?endX:x)/rect.z,float(corner&2?endY:y)/rect.w,
                corner&4?maximum+frontTolerance/worldDepth:minimum-backTolerance/worldDepth,1));
            lo=glm::min(lo,point);hi=glm::max(hi,point);
        }
        const glm::vec3 magnitude=glm::max(glm::abs(lo),glm::abs(hi));
        const float padding=std::max(1e-4f,32*std::numeric_limits<float>::epsilon()*
            std::max({magnitude.x,magnitude.y,magnitude.z}));
        emit(lo-glm::vec3(padding),hi+glm::vec3(padding));
    }
    return true;
}
