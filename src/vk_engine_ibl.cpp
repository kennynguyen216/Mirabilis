// Image-based lighting built from the selected skybox
// (docs/image_based_lighting_design.md): nine spherical-harmonic irradiance
// coefficients, a GGX-prefiltered equirectangular mip chain, and a split-sum
// BRDF lookup table.

#include "vk_engine.h"
#include "vk_engine_render_helpers.h"

#include <chrono>
#include <cmath>
#include <vector>

#include <glm/gtc/packing.hpp>

#include <vk_initializers.h>
#include <vk_pipelines.h>

namespace {

constexpr float Pi = 3.14159265359f;
constexpr uint32_t PrefilterWidth = 512;
constexpr uint32_t PrefilterHeight = 256;

// The inverse of equirectangular_uv in shaders/environment.glsl.
glm::vec3 direction_from_uv(float u, float v)
{
    const float longitude = (u - 0.5f) * 2.0f * Pi;
    const float latitude = v * Pi;
    const float ringRadius = std::sin(latitude);
    return {
        ringRadius * std::cos(longitude),
        std::cos(latitude),
        ringRadius * std::sin(longitude)};
}

// The real spherical-harmonic basis through band 2.  shaders/material_brdf.glsl
// evaluates the same nine functions in the same order.
std::array<float, 9> sh_basis(const glm::vec3& d)
{
    return {
        0.282095f,
        0.488603f * d.y,
        0.488603f * d.z,
        0.488603f * d.x,
        1.092548f * d.x * d.y,
        1.092548f * d.y * d.z,
        0.315392f * (3.0f * d.z * d.z - 1.0f),
        1.092548f * d.x * d.z,
        0.546274f * (d.x * d.x - d.y * d.y)};
}

float radical_inverse(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

// The path tracer's separable Smith term (path_trace_material.glsl).
float smith_g1(float noX, float alpha)
{
    return 2.0f * noX /
        (noX + std::sqrt(alpha * alpha + (1.0f - alpha * alpha) * noX * noX));
}

// Every level at once: the shared transition helper covers a single level.
void transition_levels(
    VkCommandBuffer cmd,
    VkImage image,
    uint32_t levels,
    VkImageLayout from,
    VkImageLayout to)
{
    VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

} // namespace

void VulkanEngine::init_image_based_lighting()
{
    // The split-sum scale and bias against (N.V, roughness), integrated with
    // the path tracer's GGX, separable Smith and Schlick Fresnel so the
    // ambient specular term agrees with the reference BRDF.
    constexpr uint32_t LutSize = 64;
    constexpr uint32_t LutSamples = 256;
    std::vector<uint16_t> lut(LutSize * LutSize * 4);
    for (uint32_t y = 0; y < LutSize; ++y) {
        const float roughness = (static_cast<float>(y) + 0.5f) / LutSize;
        const float alpha = std::max(roughness * roughness, 0.002025f);
        for (uint32_t x = 0; x < LutSize; ++x) {
            const float noV = (static_cast<float>(x) + 0.5f) / LutSize;
            const glm::vec3 view(std::sqrt(1.0f - noV * noV), 0.0f, noV);
            float scale = 0.0f;
            float bias = 0.0f;
            for (uint32_t i = 0; i < LutSamples; ++i) {
                const float phi = 2.0f * Pi * static_cast<float>(i) / LutSamples;
                const float xi = radical_inverse(i);
                const float cosTheta = std::sqrt(
                    (1.0f - xi) / (1.0f + (alpha * alpha - 1.0f) * xi));
                const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
                const glm::vec3 h(
                    sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);
                const glm::vec3 l = 2.0f * glm::dot(view, h) * h - view;
                const float noL = l.z;
                if (noL <= 0.0f) {
                    continue;
                }
                const float noH = std::max(h.z, 1e-6f);
                const float voH = std::max(glm::dot(view, h), 0.0f);
                // f cos / pdf with pdf = D noH / (4 voH): the D and the 4 noL
                // cancel, leaving G voH / (noH noV).
                const float visibility = smith_g1(noV, alpha) * smith_g1(noL, alpha) *
                    voH / (noH * noV);
                const float fresnel = std::pow(1.0f - voH, 5.0f);
                scale += (1.0f - fresnel) * visibility;
                bias += fresnel * visibility;
            }
            const size_t texel = (static_cast<size_t>(y) * LutSize + x) * 4;
            lut[texel] = glm::packHalf1x16(scale / LutSamples);
            lut[texel + 1] = glm::packHalf1x16(bias / LutSamples);
            lut[texel + 2] = glm::packHalf1x16(0.0f);
            lut[texel + 3] = glm::packHalf1x16(1.0f);
        }
    }
    _ibl.brdfLut = create_image(
        lut.data(),
        lut.size() * sizeof(uint16_t),
        {LutSize, LutSize, 1},
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_SAMPLED_BIT);
    VkSamplerCreateInfo lutSamplerInfo = sampler_info(
        VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    VK_CHECK(vkCreateSampler(_device, &lutSamplerInfo, nullptr, &_ibl.brdfLutSampler));

    // The prefiltered chain.  Created by hand because it needs exactly
    // IblPrefilterLevels levels, one storage view per level, and a clear so
    // its descriptors are valid before the first skybox loads.
    _ibl.prefiltered.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    _ibl.prefiltered.imageExtent = {PrefilterWidth, PrefilterHeight, 1};
    VkImageCreateInfo imageInfo = vkinit::image_create_info(
        _ibl.prefiltered.imageFormat,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        _ibl.prefiltered.imageExtent);
    imageInfo.mipLevels = IblPrefilterLevels;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VK_CHECK(vmaCreateImage(
        _allocator, &imageInfo, &allocationInfo,
        &_ibl.prefiltered.image, &_ibl.prefiltered.allocation, nullptr));
    VkImageViewCreateInfo viewInfo = vkinit::imageview_create_info(
        _ibl.prefiltered.imageFormat, _ibl.prefiltered.image, VK_IMAGE_ASPECT_COLOR_BIT);
    viewInfo.subresourceRange.levelCount = IblPrefilterLevels;
    VK_CHECK(vkCreateImageView(_device, &viewInfo, nullptr, &_ibl.prefiltered.imageView));
    for (uint32_t level = 0; level < IblPrefilterLevels; ++level) {
        viewInfo.subresourceRange.baseMipLevel = level;
        viewInfo.subresourceRange.levelCount = 1;
        VK_CHECK(vkCreateImageView(_device, &viewInfo, nullptr, &_ibl.levelViews[level]));
    }
    immediate_submit([&](VkCommandBuffer cmd) {
        transition_levels(cmd, _ibl.prefiltered.image, IblPrefilterLevels,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{
            VK_IMAGE_ASPECT_COLOR_BIT, 0, IblPrefilterLevels, 0, 1};
        vkCmdClearColorImage(cmd, _ibl.prefiltered.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        transition_levels(cmd, _ibl.prefiltered.image, IblPrefilterLevels,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    // U wraps across the panorama's seam; V clamps at the poles.
    VkSamplerCreateInfo prefilteredSamplerInfo = sampler_info(
        VK_FILTER_LINEAR,
        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_MIPMAP_MODE_LINEAR,
        VK_LOD_CLAMP_NONE);
    prefilteredSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(
        _device, &prefilteredSamplerInfo, nullptr, &_ibl.prefilteredSampler));

    ScopedShaderModule shader(_device);
    if (!shader.load("../../shaders/environment_prefilter.comp.spv")) {
        fmt::print("Error loading environment prefilter shader\n");
    } else {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        _ibl.prefilterSetLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
        for (VkDescriptorSet& set : _ibl.prefilterSets) {
            set = globalDescriptorAllocator.allocate(_device, _ibl.prefilterSetLayout);
        }
        VkPushConstantRange range{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(glm::vec4)};
        VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &_ibl.prefilterSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &range;
        VK_CHECK(vkCreatePipelineLayout(_device, &layoutInfo, nullptr, &_ibl.prefilterLayout));
        VkComputePipelineCreateInfo pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.layout = _ibl.prefilterLayout;
        pipelineInfo.stage = vkinit::pipeline_shader_stage_create_info(
            VK_SHADER_STAGE_COMPUTE_BIT, shader.get());
        VK_CHECK(vkCreateComputePipelines(
            _device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &_ibl.prefilterPipeline));
    }

    _mainDeletionQueue.push_function([this]() {
        if (_ibl.prefilterPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(_device, _ibl.prefilterPipeline, nullptr);
            vkDestroyPipelineLayout(_device, _ibl.prefilterLayout, nullptr);
            vkDestroyDescriptorSetLayout(_device, _ibl.prefilterSetLayout, nullptr);
        }
        vkDestroySampler(_device, _ibl.prefilteredSampler, nullptr);
        vkDestroySampler(_device, _ibl.brdfLutSampler, nullptr);
        for (VkImageView view : _ibl.levelViews) {
            vkDestroyImageView(_device, view, nullptr);
        }
        destroy_image(_ibl.prefiltered);
        destroy_image(_ibl.brdfLut);
    });
}

void VulkanEngine::project_environment_sh(
    int width,
    int height,
    const std::function<glm::vec3(int, int)>& radiance)
{
    // A stride keeps a 4K panorama to roughly a quarter of a million samples;
    // band-2 harmonics cannot resolve anything finer.
    const int stride = std::max(1, width / 512);
    std::array<glm::vec3, 9> coefficients{};
    for (int y = stride / 2; y < height; y += stride) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
        const float solidAngle = (2.0f * Pi / static_cast<float>(width)) *
            (Pi / static_cast<float>(height)) * std::sin(v * Pi) *
            static_cast<float>(stride * stride);
        for (int x = stride / 2; x < width; x += stride) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
            const std::array<float, 9> basis = sh_basis(direction_from_uv(u, v));
            const glm::vec3 sample = radiance(x, y) * solidAngle;
            for (size_t i = 0; i < basis.size(); ++i) {
                coefficients[i] += sample * basis[i];
            }
        }
    }
    // Convolution with the clamped cosine turns radiance into irradiance.
    constexpr std::array<float, 9> band{
        Pi,
        2.0f * Pi / 3.0f, 2.0f * Pi / 3.0f, 2.0f * Pi / 3.0f,
        Pi / 4.0f, Pi / 4.0f, Pi / 4.0f, Pi / 4.0f, Pi / 4.0f};
    for (size_t i = 0; i < coefficients.size(); ++i) {
        _ibl.environmentSH[i] = glm::vec4(coefficients[i] * band[i], 0.0f);
    }
}

void VulkanEngine::prefilter_environment()
{
    if (_ibl.prefilterPipeline == VK_NULL_HANDLE ||
        _skyboxImage.imageView == VK_NULL_HANDLE) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    const uint32_t sourceWidth = std::max(1u, _skyboxImage.imageExtent.width);
    const uint32_t sourceHeight = std::max(1u, _skyboxImage.imageExtent.height);
    const float sourceMaxLod =
        std::floor(std::log2(static_cast<float>(std::max(sourceWidth, sourceHeight))));

    // Every level's set is written before recording: a set may not change
    // while a command buffer that uses it is pending.
    DescriptorWriter writer;
    for (uint32_t level = 0; level < IblPrefilterLevels; ++level) {
        writer.clear();
        writer.write_image(
            0,
            _skyboxImage.imageView,
            _skyboxEnvironmentSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writer.write_image(
            1,
            _ibl.levelViews[level],
            VK_NULL_HANDLE,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        writer.update_set(_device, _ibl.prefilterSets[level]);
    }

    immediate_submit([&](VkCommandBuffer cmd) {
        transition_levels(cmd, _ibl.prefiltered.image, IblPrefilterLevels,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ibl.prefilterPipeline);
        for (uint32_t level = 0; level < IblPrefilterLevels; ++level) {
            const uint32_t levelWidth = std::max(1u, PrefilterWidth >> level);
            const uint32_t levelHeight = std::max(1u, PrefilterHeight >> level);
            vkCmdBindDescriptorSets(
                cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ibl.prefilterLayout,
                0, 1, &_ibl.prefilterSets[level], 0, nullptr);
            const glm::vec4 settings(
                static_cast<float>(level) / static_cast<float>(IblPrefilterLevels - 1),
                static_cast<float>(sourceWidth),
                _skyboxIndirectClamp,
                sourceMaxLod);
            vkCmdPushConstants(
                cmd, _ibl.prefilterLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                0, sizeof(settings), &settings);
            vkCmdDispatch(cmd, (levelWidth + 7) / 8, (levelHeight + 7) / 8, 1);
        }
        transition_levels(cmd, _ibl.prefiltered.image, IblPrefilterLevels,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    _ibl.prefilterMilliseconds = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    fmt::print(
        "Environment prefiltered: {} levels at {}x{} in {:.1f} ms; irradiance SH L0 = {:.3f} {:.3f} {:.3f}\n",
        IblPrefilterLevels, PrefilterWidth, PrefilterHeight, _ibl.prefilterMilliseconds,
        _ibl.environmentSH[0].x, _ibl.environmentSH[0].y, _ibl.environmentSH[0].z);
}
