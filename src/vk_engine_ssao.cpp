#include "vk_engine.h"
#include "vk_engine_render_helpers.h"

#include <array>
#include <cmath>
#include <random>

#include <glm/packing.hpp>

#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_pipelines.h>

void VulkanEngine::init_ssao_descriptors()
{
    if (_ssao.format != VK_FORMAT_UNDEFINED) {
        // The sampling pass: the two prepass buffers it reads, the rotation
        // noise, the image it writes, and the kernel it walks.
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        builder.add_binding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        _ssao.descriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_COMPUTE_BIT);

        // The blur: occlusion in, the two buffers that tell it which
        // neighbours belong to the same surface, and occlusion out.
        builder.clear();
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        _ssao.blurDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_COMPUTE_BIT);

        // None of these images is recreated at runtime, so every set below is
        // written once here and never revisited.
        _ssao.descriptor = globalDescriptorAllocator.allocate(
            _device, _ssao.descriptorLayout);
        DescriptorWriter ssaoWriter;
        ssaoWriter.write_image(
            0,
            _prepassDepthImage.imageView,
            _prepassSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssaoWriter.write_image(
            1,
            _prepassNormalImage.imageView,
            _prepassSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssaoWriter.write_image(
            2,
            _ssao.noiseImage.imageView,
            _ssao.noiseSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssaoWriter.write_image(
            3,
            _ssao.rawImage.imageView,
            VK_NULL_HANDLE,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        ssaoWriter.write_buffer(
            4,
            _ssao.kernelBuffer.buffer,
            sizeof(SSAOKernelBlock),
            0,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        ssaoWriter.update_set(_device, _ssao.descriptor);

        // The two blur directions share a layout and a pipeline; only which
        // image they read and which they write differs.
        const auto writeBlurSet = [&](VkDescriptorSet set,
                                      const AllocatedImage& source,
                                      const AllocatedImage& target) {
            DescriptorWriter blurWriter;
            blurWriter.write_image(
                0,
                source.imageView,
                _prepassSampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            blurWriter.write_image(
                1,
                _prepassDepthImage.imageView,
                _prepassSampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            blurWriter.write_image(
                2,
                _prepassNormalImage.imageView,
                _prepassSampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            blurWriter.write_image(
                3,
                target.imageView,
                VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            blurWriter.update_set(_device, set);
        };
        _ssao.blurHorizontalDescriptor = globalDescriptorAllocator.allocate(
            _device, _ssao.blurDescriptorLayout);
        writeBlurSet(
            _ssao.blurHorizontalDescriptor, _ssao.rawImage, _ssao.blurImage);
        _ssao.blurVerticalDescriptor = globalDescriptorAllocator.allocate(
            _device, _ssao.blurDescriptorLayout);
        writeBlurSet(
            _ssao.blurVerticalDescriptor, _ssao.blurImage, _ssao.finalImage);

    }

    const VkImageView occlusionView = ssao_occlusion_view();

    {
        // All three occlusion stages at once, for the debug views.  The layout
        // exists whether or not occlusion does, because the render-debug
        // pipeline is built against it either way.
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _ssao.debugDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_FRAGMENT_BIT);
        _ssao.debugDescriptor = globalDescriptorAllocator.allocate(
            _device, _ssao.debugDescriptorLayout);

        // Nearest, because a debug view should show the stored texel rather
        // than a filtered version of it.
        const std::array<VkImageView, 3> stages{
            _ssao.rawImage.imageView != VK_NULL_HANDLE
                ? _ssao.rawImage.imageView : occlusionView,
            _ssao.blurImage.imageView != VK_NULL_HANDLE
                ? _ssao.blurImage.imageView : occlusionView,
            _ssao.finalImage.imageView != VK_NULL_HANDLE
                ? _ssao.finalImage.imageView : occlusionView};
        DescriptorWriter debugWriter;
        for (int binding = 0; binding < 3; ++binding) {
            debugWriter.write_image(
                binding,
                stages[binding],
                _prepassSampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        }
        debugWriter.update_set(_device, _ssao.debugDescriptor);
    }
}

VkExtent2D VulkanEngine::active_ssao_extent() const
{
    // Rounded up, so an odd rendered width still has a column of occlusion
    // pixels covering its last column of geometry.
    return VkExtent2D{
        std::max(1u, (_drawExtent.width + 1) / 2),
        std::max(1u, (_drawExtent.height + 1) / 2)};
}

bool VulkanEngine::ssao_active() const
{
    // Every pipeline has to have been built, and the whole effect reads the
    // prepass, so a device that failed to produce either leaves it off rather
    // than shading against an image nothing wrote.
    return _ssao.globalEnabled && _ssao.settings.enabled &&
        _ssao.blurPipeline != VK_NULL_HANDLE &&
        _ssao.pipelines[_ssao.quality] != VK_NULL_HANDLE &&
        _depthNormalPipeline.pipeline != VK_NULL_HANDLE;
}

void VulkanEngine::init_ssao_resources()
{
    // The occlusion images are both written by a compute shader and read by
    // one, so storage and sampled use are both required and neither is
    // guaranteed for a given format.  R8 is the cheapest that can hold a
    // visibility fraction; the wider candidates exist for devices that do not
    // advertise storage support for it.
    constexpr VkFormatFeatureFlags2 requiredFeatures =
        VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    _ssao.format = pick_format(
        _chosenGPU,
        {VK_FORMAT_R8_UNORM, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT},
        requiredFeatures).format;
    if (_ssao.format == VK_FORMAT_UNDEFINED) {
        // Reported rather than fatal: the rest of the renderer works without
        // ambient occlusion, and the flag in the scene data already makes
        // every material shade as if nothing were occluding it.
        fmt::print("No format supports a storage-plus-sampled occlusion image; "
                   "ambient occlusion disabled\n");
        _ssao.settings.enabled = false;
        return;
    }
    _ssao.formatName = string_VkFormat(_ssao.format);

    // Half of the allocation rather than of the current render scale, so
    // moving the resolution slider never reallocates any of this.
    _ssao.extent = VkExtent2D{
        std::max(1u, (_drawImage.imageExtent.width + 1) / 2),
        std::max(1u, (_drawImage.imageExtent.height + 1) / 2)};
    const VkExtent3D ssaoExtent3D{_ssao.extent.width, _ssao.extent.height, 1};
    constexpr VkImageUsageFlags ssaoUsage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    _ssao.rawImage = create_image(ssaoExtent3D, _ssao.format, ssaoUsage);
    _ssao.blurImage = create_image(ssaoExtent3D, _ssao.format, ssaoUsage);
    _ssao.finalImage = create_image(ssaoExtent3D, _ssao.format, ssaoUsage);

    // Fully visible is the neutral value, so a frame that never ran the
    // dispatches - the first one, or any frame with the effect off - shades
    // exactly as it would with no ambient occlusion at all.
    immediate_submit([this](VkCommandBuffer cmd) {
        VkClearColorValue white{};
        white.float32[0] = 1.0f;
        white.float32[1] = 1.0f;
        white.float32[2] = 1.0f;
        white.float32[3] = 1.0f;
        VkImageSubresourceRange range =
            vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
        for (AllocatedImage* image :
             {&_ssao.rawImage, &_ssao.blurImage, &_ssao.finalImage}) {
            vkutil::transition_image(
                cmd,
                image->image,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(
                cmd,
                image->image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                &white,
                1,
                &range);
            vkutil::transition_image(
                cmd,
                image->image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });

    // A fixed seed, so the pattern is identical from run to run.  Captures
    // stay comparable and a change in the image is a change in the code.
    std::mt19937 generator(0x5EED5A0Bu);
    std::uniform_real_distribution<float> zeroToOne(0.0f, 1.0f);
    const auto signedRandom = [&]() { return zeroToOne(generator) * 2.0f - 1.0f; };

    // Every quality level reads a prefix of this one kernel, so the position
    // of an entry within it decides which qualities ever see that entry.
    // Taking the distance from the index straight through would give the
    // first 32 entries only the nearest third of the range, and Medium would
    // sample a radius a third of the one it was asked for.  Reversing the
    // bits spreads any power-of-two prefix across the whole range instead.
    const auto reversedFraction = [](uint32_t index) {
        uint32_t reversed = 0;
        for (int bit = 0; bit < 6; ++bit) {
            reversed = (reversed << 1) | ((index >> bit) & 1u);
        }
        return static_cast<float>(reversed) /
            static_cast<float>(MaxSSAOKernelSize);
    };

    SSAOKernelBlock kernel{};
    for (uint32_t i = 0; i < MaxSSAOKernelSize; ++i) {
        glm::vec3 point(signedRandom(), signedRandom(), zeroToOne(generator));
        // A point on the unit hemisphere, then pulled inside it.
        point = glm::normalize(point) * zeroToOne(generator);
        // Squared interpolation still crowds the kernel towards the surface.
        // Contact occlusion is a close-range effect, so most of the samples
        // are worth spending near it - just not all of them.
        const float t = reversedFraction(i);
        point *= 0.1f + 0.9f * t * t;
        kernel.samples[i] = glm::vec4(point, 0.0f);
    }
    // Raising the sample count therefore refines the estimate rather than
    // replacing it, and every quality covers the same radius.
    _ssao.kernelBuffer = create_buffer(
        sizeof(SSAOKernelBlock),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    std::memcpy(
        _ssao.kernelBuffer.info.pMappedData, &kernel, sizeof(SSAOKernelBlock));

    // Rotation vectors in the tangent plane, stored unsigned and decoded in
    // the shader because the upload helper works in four bytes per pixel.  A
    // larger tile keeps its repetition from surviving the bilateral blur as
    // regularly spaced bands on broad, flat surfaces.
    constexpr uint32_t NoiseSize = 16;
    constexpr float TwoPi = 6.28318530718f;
    std::uniform_real_distribution<float> randomAngle(0.0f, TwoPi);
    std::array<uint32_t, NoiseSize * NoiseSize> noisePixels{};
    for (uint32_t& pixel : noisePixels) {
        // Sampling an angle gives every rotation equal probability.  Picking
        // x and y independently from a square would favour its diagonals.
        const float angle = randomAngle(generator);
        const glm::vec2 direction(std::cos(angle), std::sin(angle));
        const glm::vec2 encoded = direction * 0.5f + 0.5f;
        pixel = glm::packUnorm4x8(glm::vec4(encoded.x, encoded.y, 0.5f, 1.0f));
    }
    _ssao.noiseImage = create_image(
        noisePixels.data(),
        VkExtent3D{NoiseSize, NoiseSize, 1},
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT);

    // Nearest and repeating: the point is a hard 16x16 tile of distinct
    // rotations, and filtering between them would average the noise away.
    VkSamplerCreateInfo noiseSamplerInfo = sampler_info(VK_FILTER_NEAREST);
    VK_CHECK(vkCreateSampler(
        _device, &noiseSamplerInfo, nullptr, &_ssao.noiseSampler));

    // Linear and clamped, for the half-to-full resolution step in material
    // shading.  The bilateral blur has already preserved the edges, so a plain
    // bilinear lift is enough to start with.
    VkSamplerCreateInfo ssaoSamplerInfo = sampler_info(
        VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    VK_CHECK(vkCreateSampler(_device, &ssaoSamplerInfo, nullptr, &_ssao.sampler));

    _mainDeletionQueue.push_function([this]() {
        vkDestroySampler(_device, _ssao.sampler, nullptr);
        vkDestroySampler(_device, _ssao.noiseSampler, nullptr);
        destroy_image(_ssao.noiseImage);
        destroy_buffer(_ssao.kernelBuffer);
        destroy_image(_ssao.finalImage);
        destroy_image(_ssao.blurImage);
        destroy_image(_ssao.rawImage);
    });
}

void VulkanEngine::init_ssao_pipelines()
{
    if (_ssao.format == VK_FORMAT_UNDEFINED) {
        return;
    }

    ScopedShaderModule ssaoShader(_device);
    ScopedShaderModule blurShader(_device);
    if (!ssaoShader.load("../../shaders/ssao.comp.spv") ||
        !blurShader.load("../../shaders/ssao_blur.comp.spv")) {
        fmt::print("Error loading ambient occlusion shaders\n");
        return;
    }

    // Set 0 is the camera block, which both passes need for the projection
    // and its inverse.  Set 1 holds the images belonging to the pass.
    VkPushConstantRange pushRange{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(SSAOPushConstants)};

    const std::array<VkDescriptorSetLayout, 2> ssaoLayouts{
        _gpuSceneDataDescriptorLayout, _ssao.descriptorLayout};
    VkPipelineLayoutCreateInfo ssaoLayoutInfo =
        vkinit::pipeline_layout_create_info();
    ssaoLayoutInfo.setLayoutCount = static_cast<uint32_t>(ssaoLayouts.size());
    ssaoLayoutInfo.pSetLayouts = ssaoLayouts.data();
    ssaoLayoutInfo.pushConstantRangeCount = 1;
    ssaoLayoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &ssaoLayoutInfo, nullptr, &_ssao.pipelineLayout));

    const std::array<VkDescriptorSetLayout, 2> blurLayouts{
        _gpuSceneDataDescriptorLayout, _ssao.blurDescriptorLayout};
    VkPipelineLayoutCreateInfo blurLayoutInfo =
        vkinit::pipeline_layout_create_info();
    blurLayoutInfo.setLayoutCount = static_cast<uint32_t>(blurLayouts.size());
    blurLayoutInfo.pSetLayouts = blurLayouts.data();
    blurLayoutInfo.pushConstantRangeCount = 1;
    blurLayoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &blurLayoutInfo, nullptr, &_ssao.blurPipelineLayout));

    // One pipeline per sample count.  The count is a specialization constant,
    // so the compiler sees a fixed loop bound and a lower quality setting
    // genuinely issues fewer texture fetches rather than skipping iterations.
    VkSpecializationMapEntry kernelEntry{
        .constantID = 0, .offset = 0, .size = sizeof(int32_t)};
    for (size_t quality = 0; quality < SSAOKernelSizes.size(); ++quality) {
        const int32_t kernelSize = SSAOKernelSizes[quality];
        VkSpecializationInfo specialization{
            .mapEntryCount = 1,
            .pMapEntries = &kernelEntry,
            .dataSize = sizeof(kernelSize),
            .pData = &kernelSize};

        VkPipelineShaderStageCreateInfo stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = ssaoShader.get();
        stage.pName = "main";
        stage.pSpecializationInfo = &specialization;

        VkComputePipelineCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        createInfo.layout = _ssao.pipelineLayout;
        createInfo.stage = stage;
        VK_CHECK(vkCreateComputePipelines(
            _device,
            VK_NULL_HANDLE,
            1,
            &createInfo,
            nullptr,
            &_ssao.pipelines[quality]));
    }

    // Both blur directions are the same pipeline; only the tap direction in
    // the push constants and the bound images differ.
    VkPipelineShaderStageCreateInfo blurStage{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    blurStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    blurStage.module = blurShader.get();
    blurStage.pName = "main";
    VkComputePipelineCreateInfo blurCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    blurCreateInfo.layout = _ssao.blurPipelineLayout;
    blurCreateInfo.stage = blurStage;
    VK_CHECK(vkCreateComputePipelines(
        _device,
        VK_NULL_HANDLE,
        1,
        &blurCreateInfo,
        nullptr,
        &_ssao.blurPipeline));

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _ssao.blurPipeline, nullptr);
        for (VkPipeline pipeline : _ssao.pipelines) {
            vkDestroyPipeline(_device, pipeline, nullptr);
        }
        vkDestroyPipelineLayout(_device, _ssao.blurPipelineLayout, nullptr);
        vkDestroyPipelineLayout(_device, _ssao.pipelineLayout, nullptr);
    });
}

SSAOPushConstants VulkanEngine::build_ssao_push_constants() const
{
    const VkExtent2D activeExtent = active_ssao_extent();
    const glm::vec2 ssaoAllocation(
        static_cast<float>(_ssao.extent.width),
        static_cast<float>(_ssao.extent.height));
    const glm::vec2 prepassAllocation(
        static_cast<float>(_drawImage.imageExtent.width),
        static_cast<float>(_drawImage.imageExtent.height));

    SSAOPushConstants push{};
    push.extents = glm::ivec4(
        static_cast<int>(activeExtent.width),
        static_cast<int>(activeExtent.height),
        0,
        0);
    push.texelSize = glm::vec4(
        1.0f / ssaoAllocation.x,
        1.0f / ssaoAllocation.y,
        1.0f / prepassAllocation.x,
        1.0f / prepassAllocation.y);
    // What fraction of each allocation this frame's render scale filled.  A
    // screen-space coordinate is scaled by these to reach a live texel.
    push.activeFraction = glm::vec4(
        static_cast<float>(activeExtent.width) / ssaoAllocation.x,
        static_cast<float>(activeExtent.height) / ssaoAllocation.y,
        static_cast<float>(_drawExtent.width) / prepassAllocation.x,
        static_cast<float>(_drawExtent.height) / prepassAllocation.y);
    push.screenTexel = glm::vec4(
        1.0f / static_cast<float>(_drawExtent.width),
        1.0f / static_cast<float>(_drawExtent.height),
        0.0f,
        0.0f);
    return push;
}

void VulkanEngine::draw_ssao(VkCommandBuffer cmd)
{
    const VkExtent2D activeExtent = active_ssao_extent();
    const uint32_t groupsX = (activeExtent.width + 7) / 8;
    const uint32_t groupsY = (activeExtent.height + 7) / 8;
    const uint32_t timestampBase =
        (_frameNumber % FRAME_OVERLAP) * TimestampsPerFrame;

    stats.ssao_width = static_cast<int>(activeExtent.width);
    stats.ssao_height = static_cast<int>(activeExtent.height);
    stats.ssao_kernel_samples = SSAOKernelSizes[_ssao.quality];

    // Each image moves through the same two steps: written by one dispatch,
    // then read by the next.  The barrier between them is what makes the
    // second dispatch see the first one.
    const auto toStorageWrite = [&](const AllocatedImage& image) {
        vkutil::transition_image(
            cmd,
            image.image,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
    };
    const auto toSampledRead = [&](const AllocatedImage& image) {
        vkutil::transition_image(
            cmd,
            image.image,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    };

    SSAOPushConstants push = build_ssao_push_constants();

    if (_gpuTimingSupported) {
        vkCmdWriteTimestamp2(
            cmd,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            _timestampPool,
            timestampBase + 0);
    }

    // Sampling pass: raw, noisy occlusion at half resolution.
    toStorageWrite(_ssao.rawImage);
    vkCmdBindPipeline(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssao.pipelines[_ssao.quality]);
    const std::array<VkDescriptorSet, 2> ssaoSets{
        get_current_frame().sceneDescriptor, _ssao.descriptor};
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        _ssao.pipelineLayout,
        0,
        static_cast<uint32_t>(ssaoSets.size()),
        ssaoSets.data(),
        0,
        nullptr);
    push.settings = glm::vec4(
        _ssao.settings.radius,
        _ssao.settings.bias,
        _ssao.settings.power,
        _ssao.settings.intensity);
    vkCmdPushConstants(
        cmd,
        _ssao.pipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(SSAOPushConstants),
        &push);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);
    toSampledRead(_ssao.rawImage);

    if (_gpuTimingSupported) {
        vkCmdWriteTimestamp2(
            cmd,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            _timestampPool,
            timestampBase + 1);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssao.blurPipeline);
    push.settings =
        glm::vec4(_ssao.depthFalloff, _ssao.normalFalloff, 0.0f, 0.0f);

    const auto runBlur = [&](VkDescriptorSet set,
                             const AllocatedImage& target,
                             int directionX,
                             int directionY) {
        toStorageWrite(target);
        const std::array<VkDescriptorSet, 2> blurSets{
            get_current_frame().sceneDescriptor, set};
        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            _ssao.blurPipelineLayout,
            0,
            static_cast<uint32_t>(blurSets.size()),
            blurSets.data(),
            0,
            nullptr);
        push.extents.z = directionX;
        push.extents.w = directionY;
        vkCmdPushConstants(
            cmd,
            _ssao.blurPipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            sizeof(SSAOPushConstants),
            &push);
        vkCmdDispatch(cmd, groupsX, groupsY, 1);
        toSampledRead(target);
    };

    runBlur(_ssao.blurHorizontalDescriptor, _ssao.blurImage, 1, 0);
    if (_gpuTimingSupported) {
        vkCmdWriteTimestamp2(
            cmd,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            _timestampPool,
            timestampBase + 2);
    }

    runBlur(_ssao.blurVerticalDescriptor, _ssao.finalImage, 0, 1);
    if (_gpuTimingSupported) {
        vkCmdWriteTimestamp2(
            cmd,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            _timestampPool,
            timestampBase + 3);
        _timestampsPending[_frameNumber % FRAME_OVERLAP] = true;
    }
}

