#pragma once
#include <glm/glm.hpp>
#include <span>
#include <vector>
#include <algorithm>
#include <cstdint>

// Split a complete atlas sweep into bounded strips without starving later cards
// when a continuously edited light starts a newer revision.
struct SurfaceUpdateSchedule {
    struct Strip { uint32_t card, row, rows; };
    uint32_t card{}, row{};
    uint64_t remaining{};
    void restart(std::span<const glm::uvec4> rects, bool retainCursor) {
        if (!retainCursor || card >= rects.size()) card = row = 0;
        remaining = 0;
        for (auto rect : rects) remaining += uint64_t(rect.z) * rect.w;
    }
    std::vector<Strip> take(std::span<const glm::uvec4> rects, uint64_t budget) {
        std::vector<Strip> result;
        if (rects.empty()) { remaining = 0; return result; }
        while (remaining && budget) {
            const auto rect = rects[card];
            if (!rect.z || !rect.w) { remaining = 0; break; }
            uint32_t rows = uint32_t(std::min({uint64_t(rect.w - row), budget / rect.z, remaining / rect.z}));
            if (!rows) break;
            result.push_back({card, row, rows});
            uint64_t used = uint64_t(rows) * rect.z;
            budget -= used; remaining -= used; row += rows;
            if (row == rect.w) { row = 0; card = (card + 1) % uint32_t(rects.size()); }
        }
        return result;
    }
};
