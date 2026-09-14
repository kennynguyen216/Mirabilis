#pragma once

#include "vk_engine.h"

#include <vk_pipelines.h>

#include <array>
#include <initializer_list>

#include <glm/geometric.hpp>

bool is_visible(const RenderObject& object, const glm::mat4& viewProjection);

class ScopedShaderModule {
public:
    explicit ScopedShaderModule(VkDevice device)
        : _device(device)
    {
    }

    ScopedShaderModule(const ScopedShaderModule&) = delete;
    ScopedShaderModule& operator=(const ScopedShaderModule&) = delete;

    ScopedShaderModule(ScopedShaderModule&& other) noexcept
        : _device(other._device)
        , _module(other._module)
    {
        other._module = VK_NULL_HANDLE;
    }

    ScopedShaderModule& operator=(ScopedShaderModule&& other) noexcept
    {
        if (this != &other) {
            reset();
            _device = other._device;
            _module = other._module;
            other._module = VK_NULL_HANDLE;
        }
        return *this;
    }

    ~ScopedShaderModule() { reset(); }

    bool load(const char* path)
    {
        reset();
        return vkutil::load_shader_module(path, _device, &_module);
    }

    VkShaderModule get() const { return _module; }
    operator VkShaderModule() const { return _module; }

private:
    void reset()
    {
        if (_module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, _module, nullptr);
            _module = VK_NULL_HANDLE;
        }
    }

    VkDevice _device{VK_NULL_HANDLE};
    VkShaderModule _module{VK_NULL_HANDLE};
};

inline void set_fullscreen_dynamic_state(VkCommandBuffer cmd, VkExtent2D extent)
{
    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Several fullscreen passes have no stencil attachment, but the shared
    // PipelineBuilder makes stencil state dynamic on every pipeline.
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0x00);
}

inline VkSamplerCreateInfo sampler_info(
    VkFilter filter,
    VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT,
    VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    float maxLod = 0.0f)
{
    VkSamplerCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = filter;
    info.minFilter = filter;
    info.mipmapMode = mipmapMode;
    info.addressModeU = addressMode;
    info.addressModeV = addressMode;
    info.addressModeW = addressMode;
    info.maxLod = maxLod;
    return info;
}

struct PickedFormat {
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkFormatFeatureFlags2 features{0};
};

inline PickedFormat pick_format(
    VkPhysicalDevice gpu,
    std::initializer_list<VkFormat> candidates,
    VkFormatFeatureFlags2 requiredFeatures,
    VkFormatFeatureFlags2 preferredFeatures = 0)
{
    PickedFormat fallback{};
    for (VkFormat candidate : candidates) {
        VkFormatProperties3 properties3{
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 properties2{
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
            .pNext = &properties3};
        vkGetPhysicalDeviceFormatProperties2(gpu, candidate, &properties2);
        const VkFormatFeatureFlags2 features =
            properties3.optimalTilingFeatures;
        if ((features & requiredFeatures) != requiredFeatures) {
            continue;
        }
        if (fallback.format == VK_FORMAT_UNDEFINED) {
            fallback = PickedFormat{candidate, features};
        }
        if (preferredFeatures == 0 ||
            (features & preferredFeatures) == preferredFeatures) {
            return PickedFormat{candidate, features};
        }
    }
    return fallback;
}

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
