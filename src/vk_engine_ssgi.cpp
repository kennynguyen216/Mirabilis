#include "vk_engine.h"
#include "vk_engine_render_helpers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_pipelines.h>

namespace {
struct SSGIPreset {
    bool halfResolution;
    int raysPerPixel;
    int stepCount;
    float rayLength;
    float thickness;
    float startOffset;
    int filterRadius;
    float filterDepthFalloff;
    float filterNormalPower;
    float historyWeight;
};

// 0 full-resolution validation baseline, 1 high (half resolution, spending the
// saved pixels on longer rays), 2 balanced, 3 performance, 4 peak.
constexpr std::array<SSGIPreset, 5> SSGIPresets{{
    {false, 4, 32, 12.0f, 0.35f, 0.08f, 3, 800.0f, 32.0f, 0.92f},
    {true,  4, 48, 16.0f, 0.30f, 0.06f, 4, 700.0f, 28.0f, 0.94f},
    {true,  2, 32, 12.0f, 0.35f, 0.08f, 3, 800.0f, 32.0f, 0.92f},
    {true,  1, 16,  8.0f, 0.45f, 0.10f, 2, 900.0f, 36.0f, 0.90f},
    {false, 8, 96, 20.0f, 0.25f, 0.05f, 5, 800.0f, 32.0f, 0.96f}}};
}

void VulkanEngine::apply_ssgi_quality_preset(int preset)
{
    _ssgi.qualityPreset = std::clamp(preset, 0, int(SSGIPresets.size()) - 1);
    const SSGIPreset& chosen = SSGIPresets[_ssgi.qualityPreset];
    _ssgi.halfResolution = chosen.halfResolution;
    _ssgi.raysPerPixel = chosen.raysPerPixel;
    _ssgi.stepCount = chosen.stepCount;
    _ssgi.rayLength = chosen.rayLength;
    _ssgi.thickness = chosen.thickness;
    _ssgi.startOffset = chosen.startOffset;
    _ssgi.filterRadius = chosen.filterRadius;
    _ssgi.filterDepthFalloff = chosen.filterDepthFalloff;
    _ssgi.filterNormalPower = chosen.filterNormalPower;
    _ssgi.historyWeight = chosen.historyWeight;
    _ssgi.spatialFilterEnabled = true;
    _ssgi.historyValid = false;
}

void VulkanEngine::init_ssgi_descriptors()
{
    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        for (uint32_t binding = 2; binding <= 8; ++binding) {
            builder.add_binding(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        }
        builder.add_binding(9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        builder.add_binding(10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        builder.add_binding(11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        _ssgi.descriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_COMPUTE_BIT);
        for (VkDescriptorSet& set : _ssgi.descriptors) {
            set = globalDescriptorAllocator.allocate(_device, _ssgi.descriptorLayout);
        }

        builder.clear();
        for (uint32_t binding = 0; binding < 9; ++binding) {
            builder.add_binding(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        }
        _ssgi.debugDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_FRAGMENT_BIT);
        for (uint32_t historyIndex = 0;
             historyIndex < _ssgi.debugDescriptors.size(); ++historyIndex) {
            _ssgi.debugDescriptors[historyIndex] =
                globalDescriptorAllocator.allocate(
                    _device, _ssgi.debugDescriptorLayout);
            DescriptorWriter debugWriter;
            debugWriter.write_image(0, _sceneTargets.gbufferAlbedo.imageView,
                _prepass.sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(1, _sceneTargets.gbufferVelocity.imageView,
                _prepass.sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(2, _sceneTargets.portalMask.imageView,
                _prepass.sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(3, _sceneTargets.directLighting.imageView,
                _prepass.sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(4, _ssgi.rawImage.imageView,
                _prepass.sampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(5, _ssgi.debugImage.imageView,
                _prepass.sampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(6,
                _ssgi.temporalHistory[historyIndex].imageView,
                _prepass.sampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(7,
                _ssgi.temporalDiagnosticImage.imageView,
                _prepass.sampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(8, _ssgi.filteredImage.imageView,
                _prepass.sampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.update_set(
                _device, _ssgi.debugDescriptors[historyIndex]);
        }

        builder.clear();
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _ssgi.filterDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_COMPUTE_BIT);
        for (VkDescriptorSet& set : _ssgi.filterDescriptors) {
            set = globalDescriptorAllocator.allocate(
                _device, _ssgi.filterDescriptorLayout);
        }
        for (uint32_t historyIndex = 0; historyIndex < 2; ++historyIndex) {
            DescriptorWriter filterWriter;
            filterWriter.write_image(0,
                _ssgi.temporalHistory[historyIndex].imageView,
                _prepass.sampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            filterWriter.write_image(1, _ssgi.filterScratchImage.imageView,
                VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            filterWriter.write_image(2, _prepass.depthImage.imageView,
                _prepass.sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            filterWriter.write_image(3, _prepass.normalImage.imageView,
                _prepass.sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            filterWriter.update_set(
                _device, _ssgi.filterDescriptors[historyIndex]);
        }
        DescriptorWriter verticalWriter;
        verticalWriter.write_image(0, _ssgi.filterScratchImage.imageView,
            _prepass.sampler, VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        verticalWriter.write_image(1, _ssgi.filteredImage.imageView,
            VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        verticalWriter.write_image(2, _prepass.depthImage.imageView,
            _prepass.sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        verticalWriter.write_image(3, _prepass.normalImage.imageView,
            _prepass.sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        verticalWriter.update_set(_device, _ssgi.filterDescriptors[2]);

        builder.clear();
        // Five now, not four: the trace stores incident radiance, so the
        // composite is where the receiver's base colour is applied.
        for (uint32_t binding = 0; binding < 5; ++binding) {
            builder.add_binding(
                binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        }
        _ssgi.compositeDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_FRAGMENT_BIT);
        for (uint32_t historyIndex = 0; historyIndex < 2; ++historyIndex) {
            _ssgi.compositeDescriptors[historyIndex] =
                globalDescriptorAllocator.allocate(
                    _device, _ssgi.compositeDescriptorLayout);
            DescriptorWriter compositeWriter;
            compositeWriter.write_image(0, _ssgi.filteredImage.imageView,
                _prepass.sampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            compositeWriter.write_image(1,
                _ssgi.metadataHistory[historyIndex].imageView,
                _prepass.sampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            compositeWriter.write_image(2, _prepass.depthImage.imageView,
                _prepass.sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            compositeWriter.write_image(3, _prepass.normalImage.imageView,
                _prepass.sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            // draw_ssgi() leaves the forward pass's base-colour target in
            // SHADER_READ_ONLY_OPTIMAL, and it still holds this frame's
            // albedo when the composite runs immediately afterwards.
            compositeWriter.write_image(4, _sceneTargets.gbufferAlbedo.imageView,
                _prepass.sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            compositeWriter.update_set(
                _device, _ssgi.compositeDescriptors[historyIndex]);
        }
    }
}

void VulkanEngine::init_ssgi_resources()
{
    const auto requireFormat = [&](VkFormat format, VkFormatFeatureFlags required,
                                   const char* label) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(_chosenGPU, format, &properties);
        if ((properties.optimalTilingFeatures & required) != required) {
            fmt::print("Required SSGI {} format is unsupported\n", label);
            std::abort();
        }
    };
    requireFormat(
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        "albedo");
    requireFormat(
        VK_FORMAT_R8_UNORM,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        "portal mask");
    requireFormat(
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
        "HDR/velocity");

    const VkExtent3D extent = _drawImage.imageExtent;
    _sceneTargets.gbufferAlbedo = create_image(
        extent, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _sceneTargets.gbufferVelocity = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _sceneTargets.directLighting = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    _sceneTargets.portalMask = create_image(
        extent, VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _ssgi.rawImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _ssgi.debugImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    for (AllocatedImage& history : _ssgi.temporalHistory) {
        history = create_image(
            extent, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }
    for (AllocatedImage& history : _ssgi.metadataHistory) {
        history = create_image(
            extent, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }
    _ssgi.temporalDiagnosticImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _ssgi.filterScratchImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _ssgi.filteredImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    for (AllocatedImage& history : _sceneTargets.directLightingHistory) {
        history = create_image(
            extent, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }

    // Give persistent descriptors valid layouts before their first frame.
    // The history contents remain invalid until the first forward pass copies
    // into one side of the pair.
    immediate_submit([&](VkCommandBuffer cmd) {
        const std::array<AllocatedImage*, 6> sampledImages{
            &_sceneTargets.gbufferAlbedo,
            &_sceneTargets.gbufferVelocity,
            &_sceneTargets.directLighting,
            &_sceneTargets.portalMask,
            &_sceneTargets.directLightingHistory[0],
            &_sceneTargets.directLightingHistory[1]};
        for (const AllocatedImage* image : sampledImages) {
            vkutil::transition_image(
                cmd, image->image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        vkutil::transition_image(
            cmd, _ssgi.rawImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgi.debugImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
        for (AllocatedImage& history : _ssgi.temporalHistory) {
            vkutil::transition_image(
                cmd, history.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL);
        }
        for (AllocatedImage& history : _ssgi.metadataHistory) {
            vkutil::transition_image(
                cmd, history.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL);
        }
        vkutil::transition_image(
            cmd, _ssgi.temporalDiagnosticImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgi.filterScratchImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgi.filteredImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
    });

    _mainDeletionQueue.push_function([this]() {
        destroy_image(_sceneTargets.gbufferAlbedo);
        destroy_image(_sceneTargets.gbufferVelocity);
        destroy_image(_sceneTargets.directLighting);
        destroy_image(_sceneTargets.portalMask);
        destroy_image(_ssgi.rawImage);
        destroy_image(_ssgi.debugImage);
        for (const AllocatedImage& history : _ssgi.temporalHistory) {
            destroy_image(history);
        }
        for (const AllocatedImage& history : _ssgi.metadataHistory) {
            destroy_image(history);
        }
        destroy_image(_ssgi.temporalDiagnosticImage);
        destroy_image(_ssgi.filterScratchImage);
        destroy_image(_ssgi.filteredImage);
        for (const AllocatedImage& history : _sceneTargets.directLightingHistory) {
            destroy_image(history);
        }
    });
}

void VulkanEngine::init_ssgi_pipelines()
{
    ScopedShaderModule computeShader(_device);
    ScopedShaderModule temporalShader(_device);
    ScopedShaderModule filterShader(_device);
    ScopedShaderModule compositeVertexShader(_device);
    ScopedShaderModule compositeFragmentShader(_device);
    ScopedShaderModule meshVertexShader(_device);
    ScopedShaderModule maskFragmentShader(_device);
    if (!computeShader.load("../../shaders/ssgi.comp.spv") ||
        !temporalShader.load("../../shaders/ssgi_temporal.comp.spv") ||
        !filterShader.load("../../shaders/ssgi_bilateral.comp.spv") ||
        !compositeVertexShader.load("../../shaders/render_debug.vert.spv") ||
        !compositeFragmentShader.load(
            "../../shaders/ssgi_composite.frag.spv") ||
        !meshVertexShader.load("../../shaders/mesh.vert.spv") ||
        !maskFragmentShader.load(
            "../../shaders/portal_mask_output.frag.spv")) {
        fmt::print("Error loading SSGI shaders\n");
        return;
    }

    const std::array<VkDescriptorSetLayout, 2> computeLayouts{
        _gpuSceneDataDescriptorLayout, _ssgi.descriptorLayout};
    VkPushConstantRange computeRange{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(SSGIPushConstants)};
    VkPipelineLayoutCreateInfo computeLayoutInfo =
        vkinit::pipeline_layout_create_info();
    computeLayoutInfo.setLayoutCount =
        static_cast<uint32_t>(computeLayouts.size());
    computeLayoutInfo.pSetLayouts = computeLayouts.data();
    computeLayoutInfo.pushConstantRangeCount = 1;
    computeLayoutInfo.pPushConstantRanges = &computeRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &computeLayoutInfo, nullptr, &_ssgi.pipelineLayout));

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        for (uint32_t binding = 1; binding <= 5; ++binding) {
            builder.add_binding(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        }
        for (uint32_t binding = 6; binding <= 8; ++binding) {
            builder.add_binding(binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        }
        _ssgi.lumenLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
        const std::array<VkDescriptorSetLayout, 3> lumenLayouts{
            _gpuSceneDataDescriptorLayout, _ssgi.descriptorLayout, _ssgi.lumenLayout};
        VkPipelineLayoutCreateInfo lumenInfo = vkinit::pipeline_layout_create_info();
        lumenInfo.setLayoutCount = static_cast<uint32_t>(lumenLayouts.size());
        lumenInfo.pSetLayouts = lumenLayouts.data();
        lumenInfo.pushConstantRangeCount = 1;
        lumenInfo.pPushConstantRanges = &computeRange;
        VK_CHECK(vkCreatePipelineLayout(
            _device, &lumenInfo, nullptr, &_ssgi.lumenPipelineLayout));
        ScopedShaderModule lumenShader(_device);
        if (lumenShader.load("../../shaders/ssgi_lumen.comp.spv")) {
            VkComputePipelineCreateInfo lumenCreate{
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            lumenCreate.layout = _ssgi.lumenPipelineLayout;
            lumenCreate.stage = vkinit::pipeline_shader_stage_create_info(
                VK_SHADER_STAGE_COMPUTE_BIT, lumenShader.get());
            VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
                &lumenCreate, nullptr, &_ssgi.lumenPipeline));
        } else {
            fmt::print("Error loading ssgi_lumen.comp.spv; Lumen-lite fallback unavailable\n");
        }
        _mainDeletionQueue.push_function([this]() {
            vkDestroyPipeline(_device, _ssgi.lumenPipeline, nullptr);
            vkDestroyPipelineLayout(_device, _ssgi.lumenPipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(_device, _ssgi.lumenLayout, nullptr);
        });
    }

    VkComputePipelineCreateInfo computeInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    computeInfo.layout = _ssgi.pipelineLayout;
    computeInfo.stage = vkinit::pipeline_shader_stage_create_info(
        VK_SHADER_STAGE_COMPUTE_BIT, computeShader.get());
    VK_CHECK(vkCreateComputePipelines(
        _device, VK_NULL_HANDLE, 1, &computeInfo, nullptr, &_ssgi.pipeline));
    computeInfo.stage = vkinit::pipeline_shader_stage_create_info(
        VK_SHADER_STAGE_COMPUTE_BIT, temporalShader.get());
    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
        &computeInfo, nullptr, &_ssgi.temporalPipeline));

    VkPushConstantRange filterRange{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(SSGIFilterPushConstants)};
    VkPipelineLayoutCreateInfo filterLayoutInfo =
        vkinit::pipeline_layout_create_info();
    filterLayoutInfo.setLayoutCount = 1;
    filterLayoutInfo.pSetLayouts = &_ssgi.filterDescriptorLayout;
    filterLayoutInfo.pushConstantRangeCount = 1;
    filterLayoutInfo.pPushConstantRanges = &filterRange;
    VK_CHECK(vkCreatePipelineLayout(_device, &filterLayoutInfo, nullptr,
        &_ssgi.filterPipelineLayout));
    computeInfo.layout = _ssgi.filterPipelineLayout;
    computeInfo.stage = vkinit::pipeline_shader_stage_create_info(
        VK_SHADER_STAGE_COMPUTE_BIT, filterShader.get());
    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
        &computeInfo, nullptr, &_ssgi.filterPipeline));

    VkPushConstantRange compositeRange{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(SSGICompositePushConstants)};
    VkPipelineLayoutCreateInfo compositeLayoutInfo =
        vkinit::pipeline_layout_create_info();
    compositeLayoutInfo.setLayoutCount = 1;
    compositeLayoutInfo.pSetLayouts = &_ssgi.compositeDescriptorLayout;
    compositeLayoutInfo.pushConstantRangeCount = 1;
    compositeLayoutInfo.pPushConstantRanges = &compositeRange;
    VK_CHECK(vkCreatePipelineLayout(_device, &compositeLayoutInfo, nullptr,
        &_ssgi.compositePipeline.layout));
    PipelineBuilder compositeBuilder;
    compositeBuilder._pipelineLayout = _ssgi.compositePipeline.layout;
    compositeBuilder.set_shaders(
        compositeVertexShader.get(), compositeFragmentShader.get());
    compositeBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    compositeBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    compositeBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    compositeBuilder.set_multisampling_none();
    compositeBuilder.enable_blending_additive();
    compositeBuilder.disable_depthtest();
    compositeBuilder.set_color_attachment_format(_drawImage.imageFormat);
    compositeBuilder.set_depth_format(VK_FORMAT_UNDEFINED);
    _ssgi.compositePipeline.pipeline =
        compositeBuilder.build_pipeline(_device);

    const std::array<VkDescriptorSetLayout, 2> maskLayouts{
        _gpuSceneDataDescriptorLayout, metalRoughMaterial.materialLayout};
    VkPushConstantRange maskRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(GPUDrawPushConstants)};
    VkPipelineLayoutCreateInfo maskLayoutInfo =
        vkinit::pipeline_layout_create_info();
    maskLayoutInfo.setLayoutCount = static_cast<uint32_t>(maskLayouts.size());
    maskLayoutInfo.pSetLayouts = maskLayouts.data();
    maskLayoutInfo.pushConstantRangeCount = 1;
    maskLayoutInfo.pPushConstantRanges = &maskRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &maskLayoutInfo, nullptr, &_ssgi.portalMaskPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _ssgi.portalMaskPipeline.layout;
    builder.set_shaders(meshVertexShader.get(), maskFragmentShader.get());
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    builder.disable_blending();
    builder.enable_depthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);
    builder.set_color_attachment_format(_sceneTargets.portalMask.imageFormat);
    builder.set_depth_format(_depthImage.imageFormat);
    builder.set_stencil_format(_depthImage.imageFormat);
    _ssgi.portalMaskPipeline.pipeline = builder.build_pipeline(_device);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _ssgi.pipeline, nullptr);
        vkDestroyPipeline(_device, _ssgi.temporalPipeline, nullptr);
        vkDestroyPipelineLayout(_device, _ssgi.pipelineLayout, nullptr);
        vkDestroyPipeline(_device, _ssgi.filterPipeline, nullptr);
        vkDestroyPipelineLayout(_device, _ssgi.filterPipelineLayout, nullptr);
        vkDestroyPipeline(_device, _ssgi.compositePipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(
            _device, _ssgi.compositePipeline.layout, nullptr);
        vkDestroyPipeline(_device, _ssgi.portalMaskPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(
            _device, _ssgi.portalMaskPipeline.layout, nullptr);
    });
}

void VulkanEngine::draw_ssgi(VkCommandBuffer cmd)
{
    // The forward MRT outputs become read-only inputs for both SSGI and the
    // debug views. Direct lighting is copied into a ping-pong history so the
    // march never samples an image being rendered this frame.
    vkutil::transition_image(
        cmd, _sceneTargets.gbufferAlbedo.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkutil::transition_image(
        cmd, _sceneTargets.gbufferVelocity.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkutil::transition_image(
        cmd, _sceneTargets.directLighting.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    const VkExtent2D ssgiExtent = active_ssgi_extent();
    if (_ssgi.historyExtent.width != ssgiExtent.width ||
        _ssgi.historyExtent.height != ssgiExtent.height) {
        _ssgi.historyValid = false;
        _ssgi.historyExtent = ssgiExtent;
    }

    const uint32_t writeIndex = _ssgi.historyWriteIndex;
    const uint32_t readIndex = 1u - writeIndex;
    vkutil::transition_image(
        cmd, _sceneTargets.directLightingHistory[writeIndex].image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkutil::copy_image_to_image(
        cmd,
        _sceneTargets.directLighting.image,
        _sceneTargets.directLightingHistory[writeIndex].image,
        _drawExtent,
        _drawExtent);
    vkutil::transition_image(
        cmd, _sceneTargets.directLightingHistory[writeIndex].image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkutil::transition_image(
        cmd, _sceneTargets.directLighting.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    const bool debugRequestsSSGI = is_ssgi_debug_view(_debugViews.view);
    const bool runSSGI = (_ssgi.enabled || debugRequestsSSGI) &&
        _ssgi.pipeline != VK_NULL_HANDLE &&
        _ssgi.temporalPipeline != VK_NULL_HANDLE;
    if (runSSGI) {
        const uint32_t timingBase =
            (_frameNumber % FRAME_OVERLAP) * SSGITimestampsPerFrame;
        if (_gpuTiming.supported) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                _ssgi.timestampPool, timingBase + 0);
        }
        // Orders the preceding frame's fragment reads against this
        // dispatch's writes without changing descriptor state.
        vkutil::memory_barrier(cmd);

        const std::array<VkDescriptorSet, 2> sets{
            get_current_frame().sceneDescriptor,
            _ssgi.descriptors[writeIndex]};
        const bool lumen = _ssgi.lumenEnabled && lumen_lite_ready();
        VkPipelineLayout traceLayout = _ssgi.pipelineLayout;
        if (lumen) {
            VkDescriptorSet lumenSet = get_current_frame()._frameDescriptors.allocate(
                _device, _ssgi.lumenLayout);
            DescriptorWriter writer;
            writer.write_image(0, _sceneSdf.field.imageView, _sdf.sampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            const std::array<VkImageView, 5> pages{
                _surfaceCache.albedo.imageView, _surfaceCache.emissive.imageView,
                _surfaceCache.depth.imageView, _surfaceCache.direct.imageView,
                _surfaceCache.indirect.imageView};
            for (uint32_t i = 0; i < pages.size(); ++i) {
                writer.write_image(i + 1, pages[i], _prepass.sampler,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            }
            writer.write_buffer(6, _surfaceCache.cardBuffer.buffer, VK_WHOLE_SIZE, 0,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            writer.write_buffer(7, _surfaceCache.gridBuffer.buffer, VK_WHOLE_SIZE, 0,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            writer.write_buffer(8, _surfaceCache.indexBuffer.buffer, VK_WHOLE_SIZE, 0,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            writer.update_set(_device, lumenSet);
            traceLayout = _ssgi.lumenPipelineLayout;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgi.lumenPipeline);
            const std::array<VkDescriptorSet, 3> lumenSets{sets[0], sets[1], lumenSet};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, traceLayout,
                0, static_cast<uint32_t>(lumenSets.size()), lumenSets.data(), 0, nullptr);
        } else {
            vkCmdBindPipeline(
                cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgi.pipeline);
            vkCmdBindDescriptorSets(
                cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgi.pipelineLayout,
                0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        }
        SSGIPushConstants pushConstants{};
        pushConstants.control = glm::uvec4(
            ssgiExtent.width,
            ssgiExtent.height,
            _ssgi.historyValid ? 1u : 0u,
            static_cast<uint32_t>(_frameNumber));
        pushConstants.settings = glm::vec4(
            _ssgi.rayLength,
            _ssgi.thickness,
            _ssgi.startOffset,
            static_cast<float>(_ssgi.stepCount));
        pushConstants.temporal = glm::vec4(
            _ssgi.historyWeight,
            _ssgi.depthRejection,
            _ssgi.normalRejection,
            _ssgi.velocityRejection);
        pushConstants.quality = glm::uvec4(
            static_cast<uint32_t>(std::clamp(_ssgi.raysPerPixel, 1, 8)),
            _drawExtent.width, _drawExtent.height, 0u);
        vkCmdPushConstants(
            cmd, traceLayout, VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(pushConstants), &pushConstants);
        vkCmdDispatch(
            cmd,
            (ssgiExtent.width + 7u) / 8u,
            (ssgiExtent.height + 7u) / 8u,
            1);
        if (_gpuTiming.supported) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                _ssgi.timestampPool, timingBase + 1);
        }
        vkutil::memory_barrier(cmd);

        // Temporal accumulation is a separate dispatch so every invocation
        // sees the complete current-frame raw image. This makes the 3x3
        // neighborhood clamp deterministic across workgroups.
        vkCmdBindPipeline(
            cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgi.temporalPipeline);
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgi.pipelineLayout,
            0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        vkCmdPushConstants(
            cmd, _ssgi.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(pushConstants), &pushConstants);
        vkCmdDispatch(
            cmd,
            (ssgiExtent.width + 7u) / 8u,
            (ssgiExtent.height + 7u) / 8u,
            1);
        if (_gpuTiming.supported) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                _ssgi.timestampPool, timingBase + 2);
        }
        vkutil::memory_barrier(cmd);

        if (_ssgi.filterPipeline != VK_NULL_HANDLE) {
            vkCmdBindPipeline(
                cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgi.filterPipeline);
            SSGIFilterPushConstants filterPush{};
            filterPush.settings = glm::vec4(
                static_cast<float>(_ssgi.filterRadius),
                _ssgi.filterDepthFalloff,
                _ssgi.filterNormalPower,
                _ssgi.spatialFilterEnabled ? 1.0f : 0.0f);
            const uint32_t groupX = (ssgiExtent.width + 7u) / 8u;
            const uint32_t groupY = (ssgiExtent.height + 7u) / 8u;
            filterPush.control = glm::ivec4(
                static_cast<int>(ssgiExtent.width),
                static_cast<int>(ssgiExtent.height), 1, 0);
            filterPush.frame = glm::ivec4(
                static_cast<int>(_drawExtent.width),
                static_cast<int>(_drawExtent.height), 0, 0);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                _ssgi.filterPipelineLayout, 0, 1,
                &_ssgi.filterDescriptors[writeIndex], 0, nullptr);
            vkCmdPushConstants(cmd, _ssgi.filterPipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(filterPush), &filterPush);
            vkCmdDispatch(cmd, groupX, groupY, 1);
            vkutil::memory_barrier(cmd);

            filterPush.control.z = 0;
            filterPush.control.w = 1;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                _ssgi.filterPipelineLayout, 0, 1,
                &_ssgi.filterDescriptors[2], 0, nullptr);
            vkCmdPushConstants(cmd, _ssgi.filterPipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(filterPush), &filterPush);
            vkCmdDispatch(cmd, groupX, groupY, 1);
            vkutil::memory_barrier(cmd);
        }
        if (_gpuTiming.supported) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                _ssgi.timestampPool, timingBase + 3);
        }

        _ssgi.historyValid = true;
        _ssgi.historyWriteIndex = readIndex;
    } else {
        // Enabling the effect later must not blend history from before it was
        // disabled, when neither geometry nor camera continuity was tracked.
        _ssgi.historyValid = false;
    }
}

void VulkanEngine::draw_ssgi_composite(VkCommandBuffer cmd)
{
    // A forward-written material debug view is already the finished image;
    // adding indirect light to it would corrupt the value it shows.
    if (!_ssgi.enabled || _ssgi.compositePipeline.pipeline == VK_NULL_HANDLE ||
        is_forward_material_debug_view(_debugViews.view)) {
        return;
    }

    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _drawImage.imageView, nullptr,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, nullptr);
    vkCmdBeginRendering(cmd, &renderInfo);

    set_fullscreen_dynamic_state(cmd, _drawExtent);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        _ssgi.compositePipeline.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        _ssgi.compositePipeline.layout, 0, 1,
        &_ssgi.compositeDescriptors[1u - _ssgi.historyWriteIndex], 0, nullptr);
    SSGICompositePushConstants push{};
    const VkExtent2D ssgiExtent = active_ssgi_extent();
    push.extents = glm::vec4(
        static_cast<float>(_drawExtent.width),
        static_cast<float>(_drawExtent.height),
        static_cast<float>(ssgiExtent.width),
        static_cast<float>(ssgiExtent.height));
    push.settings = glm::vec4(
        _ssgi.intensity, _ssgi.halfResolution ? 1.0f : 0.0f,
        _ssgi.filterDepthFalloff, _ssgi.filterNormalPower);
    vkCmdPushConstants(cmd, _ssgi.compositePipeline.layout,
        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    if (_gpuTiming.supported) {
        const uint32_t timingBase =
            (_frameNumber % FRAME_OVERLAP) * SSGITimestampsPerFrame;
        vkCmdWriteTimestamp2(cmd,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            _ssgi.timestampPool, timingBase + 4);
        _ssgi.timingWritten[_frameNumber % FRAME_OVERLAP] = true;
    }
}

VkExtent2D VulkanEngine::active_ssgi_extent() const
{
    if (!_ssgi.halfResolution) {
        return _drawExtent;
    }
    return VkExtent2D{
        std::max(1u, (_drawExtent.width + 1u) / 2u),
        std::max(1u, (_drawExtent.height + 1u) / 2u)};
}

void VulkanEngine::write_ssgi_trace_descriptors()
{
    for (uint32_t writeIndex = 0; writeIndex < _ssgi.descriptors.size();
         ++writeIndex) {
        const uint32_t readIndex = 1u - writeIndex;
        DescriptorWriter ssgiWriter;
        ssgiWriter.write_image(0, _ssgi.rawImage.imageView, VK_NULL_HANDLE,
            VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        ssgiWriter.write_image(1, _ssgi.debugImage.imageView, VK_NULL_HANDLE,
            VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        ssgiWriter.write_image(2, _prepass.depthImage.imageView, _prepass.sampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(3, _prepass.normalImage.imageView, _prepass.sampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(4, _sceneTargets.gbufferAlbedo.imageView, _prepass.sampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(5, _sceneTargets.directLightingHistory[readIndex].imageView,
            _prepass.sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(6, _sceneTargets.portalMask.imageView, _prepass.sampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(7, _skyboxImage.imageView,
            _skyboxEnvironmentSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(8, _sceneTargets.gbufferVelocity.imageView,
            _prepass.sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(9, _ssgi.temporalHistory[writeIndex].imageView,
            VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        ssgiWriter.write_image(10, _ssgi.temporalDiagnosticImage.imageView,
            VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        ssgiWriter.write_image(11, _ssgi.temporalHistory[readIndex].imageView,
            _prepass.sampler, VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(12, _ssgi.metadataHistory[readIndex].imageView,
            _prepass.sampler, VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(13, _ssgi.metadataHistory[writeIndex].imageView,
            VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        ssgiWriter.update_set(_device, _ssgi.descriptors[writeIndex]);
    }
}
