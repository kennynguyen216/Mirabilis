#pragma once

#include "vk_engine.h"

#include <array>

#include <glm/geometric.hpp>

bool is_visible(const RenderObject& object, const glm::mat4& viewProjection);

inline constexpr std::array<const char*, 21> RenderDebugViewNames{
    "Final lighting",
    "Camera depth",
    "View normals",
    "View position",
    "World position",
    "Occlusion (raw)",
    "Occlusion (blurred once)",
    "Occlusion (final)",
    "Albedo (linear)",
    "Motion vectors",
    "Portal mask",
    "Direct lighting source",
    "SSGI (raw noisy indirect)",
    "SSGI hit/miss",
    "SSGI steps",
    "SSGI (temporal)",
    "SSGI history rejection",
    "SSGI reprojection",
    "SSGI (filtered)",
    "SSGI fallback only",
    "SSGI vs loaded reference (difference)"};

inline const char* render_debug_view_name(RenderDebugView view)
{
    const int index = static_cast<int>(view);
    if (index < 0 || index >= static_cast<int>(RenderDebugViewNames.size())) {
        return RenderDebugViewNames[0];
    }
    return RenderDebugViewNames[index];
}

inline bool is_ssgi_debug_view(RenderDebugView view)
{
    return view == RenderDebugView::SSGIRaw ||
        view == RenderDebugView::SSGIHitMiss ||
        view == RenderDebugView::SSGISteps ||
        view == RenderDebugView::SSGITemporal ||
        view == RenderDebugView::SSGIHistoryRejection ||
        view == RenderDebugView::SSGIReprojection ||
        view == RenderDebugView::SSGIFiltered ||
        view == RenderDebugView::SSGIFallback ||
        view == RenderDebugView::SSGIReferenceDifference;
}

inline bool is_ssgi_indirect_radiance_view(RenderDebugView view)
{
    return view == RenderDebugView::SSGIRaw ||
        view == RenderDebugView::SSGITemporal ||
        view == RenderDebugView::SSGIFiltered ||
        view == RenderDebugView::SSGIFallback;
}

inline glm::vec3 normalized_sun_direction(const glm::vec3& direction)
{
    if (glm::dot(direction, direction) < 0.000001f) {
        return glm::normalize(glm::vec3(0.0f, 1.0f, 0.5f));
    }
    return glm::normalize(direction);
}
