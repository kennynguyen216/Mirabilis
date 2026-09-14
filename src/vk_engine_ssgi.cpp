#include "vk_engine.h"
#include "vk_engine_render_helpers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>

#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_pipelines.h>

void VulkanEngine::apply_ssgi_quality_preset(int preset)
{
    _ssgiQualityPreset = std::clamp(preset, 0, 4);
    switch (_ssgiQualityPreset) {
    case 0: // Full-resolution validation baseline.
        _ssgiHalfResolution = false;
        _ssgiRaysPerPixel = 4;
        _ssgiStepCount = 32;
        _ssgiRayLength = 12.0f;
        _ssgiThickness = 0.35f;
        _ssgiStartOffset = 0.08f;
        _ssgiFilterRadius = 3;
        _ssgiFilterDepthFalloff = 800.0f;
        _ssgiFilterNormalPower = 32.0f;
        _ssgiHistoryWeight = 0.92f;
        break;
    case 1: // High: spend saved pixels on longer rays.
        _ssgiHalfResolution = true;
        _ssgiRaysPerPixel = 4;
        _ssgiStepCount = 48;
        _ssgiRayLength = 16.0f;
        _ssgiThickness = 0.30f;
        _ssgiStartOffset = 0.06f;
        _ssgiFilterRadius = 4;
        _ssgiFilterDepthFalloff = 700.0f;
        _ssgiFilterNormalPower = 28.0f;
        _ssgiHistoryWeight = 0.94f;
        break;
    case 2: // Balanced.
        _ssgiHalfResolution = true;
        _ssgiRaysPerPixel = 2;
        _ssgiStepCount = 32;
        _ssgiRayLength = 12.0f;
        _ssgiThickness = 0.35f;
        _ssgiStartOffset = 0.08f;
        _ssgiFilterRadius = 3;
        _ssgiFilterDepthFalloff = 800.0f;
        _ssgiFilterNormalPower = 32.0f;
        _ssgiHistoryWeight = 0.92f;
        break;
    case 3: // Performance.
        _ssgiHalfResolution = true;
        _ssgiRaysPerPixel = 1;
        _ssgiStepCount = 16;
        _ssgiRayLength = 8.0f;
        _ssgiThickness = 0.45f;
        _ssgiStartOffset = 0.10f;
        _ssgiFilterRadius = 2;
        _ssgiFilterDepthFalloff = 900.0f;
        _ssgiFilterNormalPower = 36.0f;
        _ssgiHistoryWeight = 0.90f;
        break;
    default: // Peak: maximum samples and march precision at full resolution.
        _ssgiHalfResolution = false;
        _ssgiRaysPerPixel = 8;
        _ssgiStepCount = 96;
        _ssgiRayLength = 20.0f;
        _ssgiThickness = 0.25f;
        _ssgiStartOffset = 0.05f;
        _ssgiFilterRadius = 5;
        _ssgiFilterDepthFalloff = 800.0f;
        _ssgiFilterNormalPower = 32.0f;
        _ssgiHistoryWeight = 0.96f;
        break;
    }
    _ssgiSpatialFilterEnabled = true;
    _ssgiHistoryValid = false;
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
        builder.add_binding(14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        _ssgiDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_COMPUTE_BIT);
        for (VkDescriptorSet& set : _ssgiDescriptors) {
            set = globalDescriptorAllocator.allocate(_device, _ssgiDescriptorLayout);
        }

        builder.clear();
        for (uint32_t binding = 0; binding < 11; ++binding) {
            builder.add_binding(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        }
        _ssgiDebugDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_FRAGMENT_BIT);
        for (uint32_t historyIndex = 0;
             historyIndex < _ssgiDebugDescriptors.size(); ++historyIndex) {
            _ssgiDebugDescriptors[historyIndex] =
                globalDescriptorAllocator.allocate(
                    _device, _ssgiDebugDescriptorLayout);
            DescriptorWriter debugWriter;
            debugWriter.write_image(0, _gbufferAlbedoImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(1, _gbufferVelocityImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(2, _portalMaskImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(3, _directLightingImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(4, _ssgiRawImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(5, _ssgiDebugImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(6,
                _ssgiTemporalHistory[historyIndex].imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(7,
                _ssgiTemporalDiagnosticImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(8, _ssgiFilteredImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(9, _ssgiReferenceImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(10, _ssgiFallbackImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.update_set(
                _device, _ssgiDebugDescriptors[historyIndex]);
        }

        builder.clear();
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _ssgiFilterDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_COMPUTE_BIT);
        for (VkDescriptorSet& set : _ssgiFilterDescriptors) {
            set = globalDescriptorAllocator.allocate(
                _device, _ssgiFilterDescriptorLayout);
        }
        for (uint32_t historyIndex = 0; historyIndex < 2; ++historyIndex) {
            DescriptorWriter filterWriter;
            filterWriter.write_image(0,
                _ssgiTemporalHistory[historyIndex].imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            filterWriter.write_image(1, _ssgiFilterScratchImage.imageView,
                VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            filterWriter.write_image(2, _prepassDepthImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            filterWriter.write_image(3, _prepassNormalImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            filterWriter.update_set(
                _device, _ssgiFilterDescriptors[historyIndex]);
        }
        DescriptorWriter verticalWriter;
        verticalWriter.write_image(0, _ssgiFilterScratchImage.imageView,
            _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        verticalWriter.write_image(1, _ssgiFilteredImage.imageView,
            VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        verticalWriter.write_image(2, _prepassDepthImage.imageView,
            _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        verticalWriter.write_image(3, _prepassNormalImage.imageView,
            _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        verticalWriter.update_set(_device, _ssgiFilterDescriptors[2]);

        builder.clear();
        // Five now, not four: the trace stores incident radiance, so the
        // composite is where the receiver's base colour is applied.
        for (uint32_t binding = 0; binding < 5; ++binding) {
            builder.add_binding(
                binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        }
        _ssgiCompositeDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_FRAGMENT_BIT);
        for (uint32_t historyIndex = 0; historyIndex < 2; ++historyIndex) {
            _ssgiCompositeDescriptors[historyIndex] =
                globalDescriptorAllocator.allocate(
                    _device, _ssgiCompositeDescriptorLayout);
            DescriptorWriter compositeWriter;
            compositeWriter.write_image(0, _ssgiFilteredImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            compositeWriter.write_image(1,
                _ssgiMetadataHistory[historyIndex].imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            compositeWriter.write_image(2, _prepassDepthImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            compositeWriter.write_image(3, _prepassNormalImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            // draw_ssgi() leaves the forward pass's base-colour target in
            // SHADER_READ_ONLY_OPTIMAL, and it still holds this frame's
            // albedo when the composite runs immediately afterwards.
            compositeWriter.write_image(4, _gbufferAlbedoImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            compositeWriter.update_set(
                _device, _ssgiCompositeDescriptors[historyIndex]);
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
    _gbufferAlbedoImage = create_image(
        extent, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _gbufferVelocityImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _directLightingImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    _portalMaskImage = create_image(
        extent, VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _ssgiRawImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _ssgiDebugImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _ssgiFallbackImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    for (AllocatedImage& history : _ssgiTemporalHistory) {
        history = create_image(
            extent, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }
    for (AllocatedImage& history : _ssgiMetadataHistory) {
        history = create_image(
            extent, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }
    _ssgiTemporalDiagnosticImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _ssgiFilterScratchImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _ssgiFilteredImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    _ssgiReferenceImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

    AllocatedBuffer referenceUpload{};
    if (const char* referencePath = std::getenv("MIRABILIS_SSGI_REFERENCE")) {
        std::ifstream file(referencePath, std::ios::binary);
        std::string magic;
        uint32_t width = 0, height = 0;
        float scale = 0.0f;
        file >> magic >> width >> height >> scale;
        file.get();
        if (!file || magic != "PF" || width != extent.width ||
            height != extent.height || scale >= 0.0f) {
            fmt::print("SSGI reference must be a matching little-endian RGB PFM: {}\n",
                referencePath);
            std::abort();
        }
        std::vector<uint64_t> packed(size_t(width) * height);
        for (int y = int(height) - 1; y >= 0; --y) {
            for (uint32_t x = 0; x < width; ++x) {
                glm::vec3 rgb{};
                file.read(reinterpret_cast<char*>(&rgb), 3 * sizeof(float));
                const uint64_t rg = glm::packHalf2x16(glm::vec2(rgb));
                const uint64_t ba = glm::packHalf2x16(glm::vec2(rgb.z, 1.0f));
                packed[size_t(y) * width + x] = rg | (ba << 32u);
            }
        }
        if (!file) std::abort();
        referenceUpload = create_buffer(
            packed.size() * sizeof(uint64_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU);
        std::memcpy(referenceUpload.info.pMappedData, packed.data(),
            packed.size() * sizeof(uint64_t));
        _ssgiReferenceLoaded = true;
        fmt::print("Loaded SSGI reference: {} ({}x{})\n",
            referencePath, width, height);
    }
    for (AllocatedImage& history : _directLightingHistory) {
        history = create_image(
            extent, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }

    // Give persistent descriptors valid layouts before their first frame.
    // The history contents remain invalid until the first forward pass copies
    // into one side of the pair.
    immediate_submit([&](VkCommandBuffer cmd) {
        const std::array<AllocatedImage*, 6> sampledImages{
            &_gbufferAlbedoImage,
            &_gbufferVelocityImage,
            &_directLightingImage,
            &_portalMaskImage,
            &_directLightingHistory[0],
            &_directLightingHistory[1]};
        for (const AllocatedImage* image : sampledImages) {
            vkutil::transition_image(
                cmd, image->image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        vkutil::transition_image(
            cmd, _ssgiRawImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiDebugImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiFallbackImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
        for (AllocatedImage& history : _ssgiTemporalHistory) {
            vkutil::transition_image(
                cmd, history.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL);
        }
        for (AllocatedImage& history : _ssgiMetadataHistory) {
            vkutil::transition_image(
                cmd, history.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL);
        }
        vkutil::transition_image(
            cmd, _ssgiTemporalDiagnosticImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiFilterScratchImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiFilteredImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(cmd, _ssgiReferenceImage.image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange referenceRange{
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (_ssgiReferenceLoaded) {
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = extent;
            vkCmdCopyBufferToImage(cmd, referenceUpload.buffer,
                _ssgiReferenceImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &copy);
        } else {
            VkClearColorValue clear{};
            vkCmdClearColorImage(cmd, _ssgiReferenceImage.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                &referenceRange);
        }
        vkutil::transition_image(cmd, _ssgiReferenceImage.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    if (_ssgiReferenceLoaded) {
        destroy_buffer(referenceUpload);
    }

    _mainDeletionQueue.push_function([this]() {
        destroy_image(_gbufferAlbedoImage);
        destroy_image(_gbufferVelocityImage);
        destroy_image(_directLightingImage);
        destroy_image(_portalMaskImage);
        destroy_image(_ssgiRawImage);
        destroy_image(_ssgiDebugImage);
        destroy_image(_ssgiFallbackImage);
        for (const AllocatedImage& history : _ssgiTemporalHistory) {
            destroy_image(history);
        }
        for (const AllocatedImage& history : _ssgiMetadataHistory) {
            destroy_image(history);
        }
        destroy_image(_ssgiTemporalDiagnosticImage);
        destroy_image(_ssgiFilterScratchImage);
        destroy_image(_ssgiFilteredImage);
        destroy_image(_ssgiReferenceImage);
        for (const AllocatedImage& history : _directLightingHistory) {
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
        _gpuSceneDataDescriptorLayout, _ssgiDescriptorLayout};
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
        _device, &computeLayoutInfo, nullptr, &_ssgiPipelineLayout));

    VkComputePipelineCreateInfo computeInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    computeInfo.layout = _ssgiPipelineLayout;
    computeInfo.stage = vkinit::pipeline_shader_stage_create_info(
        VK_SHADER_STAGE_COMPUTE_BIT, computeShader.get());
    VK_CHECK(vkCreateComputePipelines(
        _device, VK_NULL_HANDLE, 1, &computeInfo, nullptr, &_ssgiPipeline));
    computeInfo.stage = vkinit::pipeline_shader_stage_create_info(
        VK_SHADER_STAGE_COMPUTE_BIT, temporalShader.get());
    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
        &computeInfo, nullptr, &_ssgiTemporalPipeline));

    VkPushConstantRange filterRange{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(SSGIFilterPushConstants)};
    VkPipelineLayoutCreateInfo filterLayoutInfo =
        vkinit::pipeline_layout_create_info();
    filterLayoutInfo.setLayoutCount = 1;
    filterLayoutInfo.pSetLayouts = &_ssgiFilterDescriptorLayout;
    filterLayoutInfo.pushConstantRangeCount = 1;
    filterLayoutInfo.pPushConstantRanges = &filterRange;
    VK_CHECK(vkCreatePipelineLayout(_device, &filterLayoutInfo, nullptr,
        &_ssgiFilterPipelineLayout));
    computeInfo.layout = _ssgiFilterPipelineLayout;
    computeInfo.stage = vkinit::pipeline_shader_stage_create_info(
        VK_SHADER_STAGE_COMPUTE_BIT, filterShader.get());
    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
        &computeInfo, nullptr, &_ssgiFilterPipeline));

    VkPushConstantRange compositeRange{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(SSGICompositePushConstants)};
    VkPipelineLayoutCreateInfo compositeLayoutInfo =
        vkinit::pipeline_layout_create_info();
    compositeLayoutInfo.setLayoutCount = 1;
    compositeLayoutInfo.pSetLayouts = &_ssgiCompositeDescriptorLayout;
    compositeLayoutInfo.pushConstantRangeCount = 1;
    compositeLayoutInfo.pPushConstantRanges = &compositeRange;
    VK_CHECK(vkCreatePipelineLayout(_device, &compositeLayoutInfo, nullptr,
        &_ssgiCompositePipeline.layout));
    PipelineBuilder compositeBuilder;
    compositeBuilder._pipelineLayout = _ssgiCompositePipeline.layout;
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
    _ssgiCompositePipeline.pipeline =
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
        _device, &maskLayoutInfo, nullptr, &_ssgiPortalMaskPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _ssgiPortalMaskPipeline.layout;
    builder.set_shaders(meshVertexShader.get(), maskFragmentShader.get());
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    builder.disable_blending();
    builder.enable_depthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);
    builder.set_color_attachment_format(_portalMaskImage.imageFormat);
    builder.set_depth_format(_depthImage.imageFormat);
    builder.set_stencil_format(_depthImage.imageFormat);
    _ssgiPortalMaskPipeline.pipeline = builder.build_pipeline(_device);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _ssgiPipeline, nullptr);
        vkDestroyPipeline(_device, _ssgiTemporalPipeline, nullptr);
        vkDestroyPipelineLayout(_device, _ssgiPipelineLayout, nullptr);
        vkDestroyPipeline(_device, _ssgiFilterPipeline, nullptr);
        vkDestroyPipelineLayout(_device, _ssgiFilterPipelineLayout, nullptr);
        vkDestroyPipeline(_device, _ssgiCompositePipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(
            _device, _ssgiCompositePipeline.layout, nullptr);
        vkDestroyPipeline(_device, _ssgiPortalMaskPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(
            _device, _ssgiPortalMaskPipeline.layout, nullptr);
    });
}

void VulkanEngine::draw_ssgi(VkCommandBuffer cmd)
{
    // The forward MRT outputs become read-only inputs for both SSGI and the
    // debug views. Direct lighting is copied into a ping-pong history so the
    // march never samples an image being rendered this frame.
    vkutil::transition_image(
        cmd, _gbufferAlbedoImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkutil::transition_image(
        cmd, _gbufferVelocityImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkutil::transition_image(
        cmd, _directLightingImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    const VkExtent2D ssgiExtent = active_ssgi_extent();
    if (_ssgiHistoryExtent.width != ssgiExtent.width ||
        _ssgiHistoryExtent.height != ssgiExtent.height) {
        _ssgiHistoryValid = false;
        _ssgiHistoryExtent = ssgiExtent;
    }

    const uint32_t writeIndex = _ssgiHistoryWriteIndex;
    const uint32_t readIndex = 1u - writeIndex;
    vkutil::transition_image(
        cmd, _directLightingHistory[writeIndex].image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkutil::copy_image_to_image(
        cmd,
        _directLightingImage.image,
        _directLightingHistory[writeIndex].image,
        _drawExtent,
        _drawExtent);
    vkutil::transition_image(
        cmd, _directLightingHistory[writeIndex].image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkutil::transition_image(
        cmd, _directLightingImage.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    const bool debugRequestsSSGI = is_ssgi_debug_view(_renderDebugView);
    const bool runSSGI = (_ssgiEnabled || debugRequestsSSGI) &&
        _ssgiPipeline != VK_NULL_HANDLE &&
        _ssgiTemporalPipeline != VK_NULL_HANDLE;
    if (runSSGI) {
        const uint32_t timingBase =
            (_frameNumber % FRAME_OVERLAP) * SSGITimestampsPerFrame;
        if (_gpuTimingSupported) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                _ssgiTimestampPool, timingBase + 0);
        }
        // Same-layout barriers make the preceding frame's fragment reads and
        // this dispatch's writes explicit without changing descriptor state.
        vkutil::transition_image(
            cmd, _ssgiRawImage.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiDebugImage.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiFallbackImage.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL);

        vkutil::transition_image(
            cmd, _ssgiTemporalHistory[writeIndex].image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiMetadataHistory[writeIndex].image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiTemporalDiagnosticImage.image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);

        vkCmdBindPipeline(
            cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgiPipeline);
        const std::array<VkDescriptorSet, 2> sets{
            get_current_frame().sceneDescriptor,
            _ssgiDescriptors[writeIndex]};
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgiPipelineLayout,
            0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        SSGIPushConstants pushConstants{};
        pushConstants.control = glm::uvec4(
            ssgiExtent.width,
            ssgiExtent.height,
            _ssgiHistoryValid ? 1u : 0u,
            static_cast<uint32_t>(_frameNumber));
        pushConstants.settings = glm::vec4(
            _ssgiRayLength,
            _ssgiThickness,
            _ssgiStartOffset,
            static_cast<float>(_ssgiStepCount));
        pushConstants.temporal = glm::vec4(
            _ssgiHistoryWeight,
            _ssgiDepthRejection,
            _ssgiNormalRejection,
            _ssgiVelocityRejection);
        pushConstants.quality = glm::uvec4(
            static_cast<uint32_t>(std::clamp(_ssgiRaysPerPixel, 1, 8)),
            0u, 0u, 0u);
        vkCmdPushConstants(
            cmd, _ssgiPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(pushConstants), &pushConstants);
        vkCmdDispatch(
            cmd,
            (ssgiExtent.width + 7u) / 8u,
            (ssgiExtent.height + 7u) / 8u,
            1);
        if (_gpuTimingSupported) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                _ssgiTimestampPool, timingBase + 1);
        }
        vkutil::transition_image(
            cmd, _ssgiRawImage.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiDebugImage.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiFallbackImage.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL);

        // Temporal accumulation is a separate dispatch so every invocation
        // sees the complete current-frame raw image. This makes the 3x3
        // neighborhood clamp deterministic across workgroups.
        vkCmdBindPipeline(
            cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgiTemporalPipeline);
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgiPipelineLayout,
            0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        vkCmdPushConstants(
            cmd, _ssgiPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(pushConstants), &pushConstants);
        vkCmdDispatch(
            cmd,
            (ssgiExtent.width + 7u) / 8u,
            (ssgiExtent.height + 7u) / 8u,
            1);
        if (_gpuTimingSupported) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                _ssgiTimestampPool, timingBase + 2);
        }
        vkutil::transition_image(
            cmd, _ssgiTemporalHistory[writeIndex].image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiMetadataHistory[writeIndex].image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiTemporalDiagnosticImage.image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);

        if (_ssgiFilterPipeline != VK_NULL_HANDLE) {
            vkCmdBindPipeline(
                cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgiFilterPipeline);
            SSGIFilterPushConstants filterPush{};
            filterPush.settings = glm::vec4(
                static_cast<float>(_ssgiFilterRadius),
                _ssgiFilterDepthFalloff,
                _ssgiFilterNormalPower,
                _ssgiSpatialFilterEnabled ? 1.0f : 0.0f);
            const uint32_t groupX = (ssgiExtent.width + 7u) / 8u;
            const uint32_t groupY = (ssgiExtent.height + 7u) / 8u;
            filterPush.control = glm::ivec4(
                static_cast<int>(ssgiExtent.width),
                static_cast<int>(ssgiExtent.height), 1, 0);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                _ssgiFilterPipelineLayout, 0, 1,
                &_ssgiFilterDescriptors[writeIndex], 0, nullptr);
            vkCmdPushConstants(cmd, _ssgiFilterPipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(filterPush), &filterPush);
            vkCmdDispatch(cmd, groupX, groupY, 1);
            vkutil::transition_image(cmd, _ssgiFilterScratchImage.image,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);

            filterPush.control.z = 0;
            filterPush.control.w = 1;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                _ssgiFilterPipelineLayout, 0, 1,
                &_ssgiFilterDescriptors[2], 0, nullptr);
            vkCmdPushConstants(cmd, _ssgiFilterPipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(filterPush), &filterPush);
            vkCmdDispatch(cmd, groupX, groupY, 1);
            vkutil::transition_image(cmd, _ssgiFilteredImage.image,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        }
        if (_gpuTimingSupported) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                _ssgiTimestampPool, timingBase + 3);
        }

        _ssgiHistoryValid = true;
        _ssgiHistoryWriteIndex = readIndex;
    } else {
        // Enabling the effect later must not blend history from before it was
        // disabled, when neither geometry nor camera continuity was tracked.
        _ssgiHistoryValid = false;
    }
}

void VulkanEngine::draw_ssgi_composite(VkCommandBuffer cmd)
{
    if (!_ssgiEnabled || _ssgiCompositePipeline.pipeline == VK_NULL_HANDLE) {
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
        _ssgiCompositePipeline.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        _ssgiCompositePipeline.layout, 0, 1,
        &_ssgiCompositeDescriptors[1u - _ssgiHistoryWriteIndex], 0, nullptr);
    SSGICompositePushConstants push{};
    const VkExtent2D ssgiExtent = active_ssgi_extent();
    push.extents = glm::vec4(
        static_cast<float>(_drawExtent.width),
        static_cast<float>(_drawExtent.height),
        static_cast<float>(ssgiExtent.width),
        static_cast<float>(ssgiExtent.height));
    push.settings = glm::vec4(
        _ssgiIntensity, _ssgiHalfResolution ? 1.0f : 0.0f, 0.0f, 0.0f);
    vkCmdPushConstants(cmd, _ssgiCompositePipeline.layout,
        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    if (_gpuTimingSupported) {
        const uint32_t timingBase =
            (_frameNumber % FRAME_OVERLAP) * SSGITimestampsPerFrame;
        vkCmdWriteTimestamp2(cmd,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            _ssgiTimestampPool, timingBase + 4);
        _ssgiTimingWritten[_frameNumber % FRAME_OVERLAP] = true;
    }
}

VkExtent2D VulkanEngine::active_ssgi_extent() const
{
    if (!_ssgiHalfResolution) {
        return _drawExtent;
    }
    return VkExtent2D{
        std::max(1u, (_drawExtent.width + 1u) / 2u),
        std::max(1u, (_drawExtent.height + 1u) / 2u)};
}

void VulkanEngine::write_ssgi_trace_descriptors()
{
    for (uint32_t writeIndex = 0; writeIndex < _ssgiDescriptors.size();
         ++writeIndex) {
        const uint32_t readIndex = 1u - writeIndex;
        DescriptorWriter ssgiWriter;
        ssgiWriter.write_image(0, _ssgiRawImage.imageView, VK_NULL_HANDLE,
            VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        ssgiWriter.write_image(1, _ssgiDebugImage.imageView, VK_NULL_HANDLE,
            VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        ssgiWriter.write_image(2, _prepassDepthImage.imageView, _prepassSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(3, _prepassNormalImage.imageView, _prepassSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(4, _gbufferAlbedoImage.imageView, _prepassSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(5, _directLightingHistory[readIndex].imageView,
            _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(6, _portalMaskImage.imageView, _prepassSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(7, _skyboxImage.imageView,
            _skyboxEnvironmentSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(8, _gbufferVelocityImage.imageView,
            _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(9, _ssgiTemporalHistory[writeIndex].imageView,
            VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        ssgiWriter.write_image(10, _ssgiTemporalDiagnosticImage.imageView,
            VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        ssgiWriter.write_image(11, _ssgiTemporalHistory[readIndex].imageView,
            _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(12, _ssgiMetadataHistory[readIndex].imageView,
            _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssgiWriter.write_image(13, _ssgiMetadataHistory[writeIndex].imageView,
            VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        ssgiWriter.write_image(14, _ssgiFallbackImage.imageView,
            VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        ssgiWriter.update_set(_device, _ssgiDescriptors[writeIndex]);
    }
}

