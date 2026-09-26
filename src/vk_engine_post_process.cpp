#include "vk_engine.h"
#include "vk_engine_render_helpers.h"
#include "env_flags.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_pipelines.h>

namespace {

// IQ3 auto-exposure metering, the CPU half of auto_exposure.comp: its bins
// are black (0) and log2 luminance [-16, 8) in 255 steps (1-255).
constexpr uint32_t ExposureBins = 256;
constexpr float ExposureMinLog2 = -16.0f;
constexpr float ExposureLog2Range = 24.0f;
// "Ignoring the darkest and brightest few percent": 5% at each end.
constexpr float ExposureTrim = 0.05f;

// Mean log2 luminance of the pixels left once the darkest and brightest
// ExposureTrim of them are dropped, from bin centres (black counts as the
// histogram's floor).  Returns the floor for an empty histogram.
constexpr float auto_exposure_mean_log2(const std::array<uint32_t, ExposureBins>& bins)
{
    double total = 0.0;
    for (uint32_t count : bins) total += count;
    const double low = total * ExposureTrim;
    const double high = total * (1.0 - ExposureTrim);
    double sum = 0.0;
    double start = 0.0;
    for (uint32_t bin = 0; bin < ExposureBins; ++bin) {
        const double end = start + bins[bin];
        const double kept = (end < high ? end : high) - (start > low ? start : low);
        if (kept > 0.0) {
            const double centre = bin == 0 ? ExposureMinLog2
                : ExposureMinLog2 + (bin - 0.5) * (ExposureLog2Range / 255.0);
            sum += kept * centre;
        }
        start = end;
    }
    return high > low ? static_cast<float>(sum / (high - low)) : ExposureMinLog2;
}

// The build is the test, as in env_flags.h.
constexpr bool near(float a, float b) { return a - b < 1e-4f && b - a < 1e-4f; }
constexpr float bin_centre(uint32_t bin)
{
    return ExposureMinLog2 + (bin - 0.5f) * (ExposureLog2Range / 255.0f);
}
constexpr std::array<uint32_t, ExposureBins> histogram(
    uint32_t binA, uint32_t countA, uint32_t binB = 0, uint32_t countB = 0)
{
    std::array<uint32_t, ExposureBins> bins{};
    bins[binA] += countA;
    bins[binB] += countB;
    return bins;
}
// One bin: its centre.
static_assert(near(auto_exposure_mean_log2(histogram(128, 1000)), bin_centre(128)));
// Half black, half one bin: 5% of each end goes, the mean splits the rest.
static_assert(near(auto_exposure_mean_log2(histogram(0, 500, 200, 500)),
    0.5f * (ExposureMinLog2 + bin_centre(200))));
// Exactly the darkest 5% black: dropped entirely.
static_assert(near(auto_exposure_mean_log2(histogram(0, 50, 200, 950)), bin_centre(200)));
static_assert(near(auto_exposure_mean_log2(histogram(0, 0)), ExposureMinLog2));

}  // namespace

void VulkanEngine::init_post_process_descriptors()
{
    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _singleImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_FRAGMENT_BIT);

        // Both post-process passes want exactly that shape: one sampled
        // colour image.  Every image involved outlives every frame, so these
        // sets are written once here rather than rebuilt per frame.
        _postProcess.tonemapInputDescriptor = globalDescriptorAllocator.allocate(
            _device, _singleImageDescriptorLayout);

        DescriptorWriter tonemapWriter;
        tonemapWriter.write_image(
            0,
            _drawImage.imageView,
            _postProcess.sampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        tonemapWriter.update_set(_device, _postProcess.tonemapInputDescriptor);

        // Anti-aliasing reads the tonemap's output, not the draw image.  Its
        // edge search compares lumas against fixed thresholds, which only
        // mean anything once the values are in display range.
        _postProcess.fxaaInputDescriptor = globalDescriptorAllocator.allocate(
            _device, _singleImageDescriptorLayout);

        DescriptorWriter fxaaWriter;
        fxaaWriter.write_image(
            0,
            _postProcess.tonemapImage.imageView,
            _postProcess.sampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        fxaaWriter.update_set(_device, _postProcess.fxaaInputDescriptor);
    }
}

void VulkanEngine::init_post_process_resources()
{
    // A filter cannot read and write one image in the same pass, so the
    // composed frame is read from _drawImage and resolved into this one.  It
    // matches the draw image exactly, including being allocated at window size
    // and only partly used when the resolution scale is below 1.
    // Both post-process targets carry the swapchain's format rather than the
    // draw image's.  By the time the frame reaches them the tonemap has
    // already reduced it to display range, so the extra twelve bits per pixel
    // of the HDR format would store nothing, and matching the swapchain lets
    // the final blit copy rather than convert.
    _postProcess.image = create_image(
        _drawImage.imageExtent,
        _swapchainImageFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    // The tonemap's own target.  It is separate from _postProcess.image for
    // the same reason that one is separate from _drawImage: anti-aliasing
    // reads this and writes that, and no pass may do both to one image.
    _postProcess.tonemapImage = create_image(
        _drawImage.imageExtent,
        _swapchainImageFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    // Linear, unlike the prepass sampler: the whole point of the final tap is
    // to land between two texels and let the hardware mix them.  Clamped
    // because a filter kernel always reaches past the image at its border.
    VkSamplerCreateInfo samplerInfo = sampler_info(
        VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    VK_CHECK(vkCreateSampler(_device, &samplerInfo, nullptr, &_postProcess.sampler));

    _mainDeletionQueue.push_function([this]() {
        vkDestroySampler(_device, _postProcess.sampler, nullptr);
        destroy_image(_postProcess.tonemapImage);
        destroy_image(_postProcess.image);
    });
}

void VulkanEngine::init_tonemap_pipeline()
{
    ScopedShaderModule vertexShader(_device);
    ScopedShaderModule fragmentShader(_device);
    // The same fullscreen triangle the anti-aliasing pass uses.  It binds
    // nothing and interpolates nothing, so there is no reason for a second
    // copy of it.
    if (!vertexShader.load("../../shaders/fxaa.vert.spv") ||
        !fragmentShader.load("../../shaders/tonemap.frag.spv")) {
        fmt::print("Error loading tonemap shaders\n");
        return;
    }

    VkPushConstantRange settingsRange{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(TonemapPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &_singleImageDescriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &settingsRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &layoutInfo, nullptr, &_postProcess.tonemapPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _postProcess.tonemapPipeline.layout;
    builder.set_shaders(vertexShader.get(), fragmentShader.get());
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    builder.disable_blending();
    builder.disable_depthtest();
    builder.set_color_attachment_format(_postProcess.tonemapImage.imageFormat);
    builder.set_depth_format(VK_FORMAT_UNDEFINED);
    _postProcess.tonemapPipeline.pipeline = builder.build_pipeline(_device);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _postProcess.tonemapPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _postProcess.tonemapPipeline.layout, nullptr);
    });
}

void VulkanEngine::init_fxaa_pipeline()
{
    ScopedShaderModule vertexShader(_device);
    ScopedShaderModule fragmentShader(_device);
    if (!vertexShader.load("../../shaders/fxaa.vert.spv") ||
        !fragmentShader.load("../../shaders/fxaa.frag.spv")) {
        fmt::print("Error loading FXAA shaders\n");
        return;
    }

    // One sampled image and one push constant block.  The filter works purely
    // in screen space, so it needs neither scene data nor a camera.
    VkPushConstantRange settingsRange{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(FXAAPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &_singleImageDescriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &settingsRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &layoutInfo, nullptr, &_postProcess.fxaaPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _postProcess.fxaaPipeline.layout;
    builder.set_shaders(vertexShader.get(), fragmentShader.get());
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    // It writes every pixel of its target from scratch, so there is nothing
    // to blend against and no depth to test.
    builder.disable_blending();
    builder.disable_depthtest();
    builder.set_color_attachment_format(_postProcess.image.imageFormat);
    builder.set_depth_format(VK_FORMAT_UNDEFINED);
    _postProcess.fxaaPipeline.pipeline = builder.build_pipeline(_device);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _postProcess.fxaaPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _postProcess.fxaaPipeline.layout, nullptr);
    });
}

void VulkanEngine::init_auto_exposure()
{
    const char* value = SDL_getenv("MIRABILIS_AUTO_EXPOSURE");
    const EnvBool flag = parse_env_bool(value);
    if (flag == EnvBool::Invalid) {
        fmt::print("MIRABILIS_AUTO_EXPOSURE='{}' is not 0/1/true/false/on/off/yes/no\n", value);
        abort();
    }
    // Bounded runs keep the fixed exposure every capture was measured with.
    const bool bounded = SDL_getenv("MIRABILIS_TEST_FRAMES") != nullptr;
    _autoExposure.enabled = value ? flag == EnvBool::On : !bounded;
    _autoExposure.logFrames = _autoExposure.enabled && bounded;

    ScopedShaderModule shader(_device);
    if (!shader.load("../../shaders/auto_exposure.comp.spv")) {
        fmt::print("Error loading auto_exposure.comp.spv; auto-exposure unavailable\n");
        _autoExposure.enabled = false;
        return;
    }
    DescriptorLayoutBuilder builder;
    builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    _autoExposure.setLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);

    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(glm::uvec4)};
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &_autoExposure.setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &layoutInfo, nullptr, &_autoExposure.pipeline.layout));
    VkComputePipelineCreateInfo pipelineInfo{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.layout = _autoExposure.pipeline.layout;
    pipelineInfo.stage = vkinit::pipeline_shader_stage_create_info(
        VK_SHADER_STAGE_COMPUTE_BIT, shader.get());
    VK_CHECK(vkCreateComputePipelines(
        _device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &_autoExposure.pipeline.pipeline));

    const size_t bytes = FRAME_OVERLAP * ExposureBins * sizeof(uint32_t);
    _autoExposure.histogram = create_buffer(bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);
    _autoExposure.readback = create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    _autoExposure.set = globalDescriptorAllocator.allocate(_device, _autoExposure.setLayout);
    DescriptorWriter writer;
    writer.write_image(0, _drawImage.imageView, _postProcess.sampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.write_buffer(1, _autoExposure.histogram.buffer, bytes, 0,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.update_set(_device, _autoExposure.set);
    VkQueryPoolCreateInfo query{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query.queryCount = 2 * FRAME_OVERLAP;
    VK_CHECK(vkCreateQueryPool(_device, &query, nullptr, &_autoExposure.timestampPool));

    _mainDeletionQueue.push_function([this]() {
        vkDestroyQueryPool(_device, _autoExposure.timestampPool, nullptr);
        destroy_buffer(_autoExposure.readback);
        destroy_buffer(_autoExposure.histogram);
        vkDestroyPipeline(_device, _autoExposure.pipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _autoExposure.pipeline.layout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _autoExposure.setLayout, nullptr);
    });
    fmt::print("IQ3 auto-exposure: {}\n", _autoExposure.enabled ? "on"
        : bounded && !value ? "off (bounded run; MIRABILIS_AUTO_EXPOSURE=1 enables it)" : "off");
}

void VulkanEngine::record_auto_exposure(VkCommandBuffer cmd)
{
    // Called with the draw image already readable.  This slot's histogram is
    // cleared, filled and copied out for update_auto_exposure() to read once
    // the slot's fence has passed.
    const uint32_t slot = _frameNumber % FRAME_OVERLAP;
    const VkDeviceSize size = ExposureBins * sizeof(uint32_t);
    const VkDeviceSize offset = slot * size;
    const auto barrier = [&](VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
            VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkMemoryBarrier2 memory{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        memory.srcStageMask = srcStage;
        memory.srcAccessMask = srcAccess;
        memory.dstStageMask = dstStage;
        memory.dstAccessMask = dstAccess;
        VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &memory;
        vkCmdPipelineBarrier2(cmd, &dependency);
    };
    if (_gpuTiming.supported) {
        vkCmdResetQueryPool(cmd, _autoExposure.timestampPool, slot * 2, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            _autoExposure.timestampPool, slot * 2);
    }
    vkCmdFillBuffer(cmd, _autoExposure.histogram.buffer, offset, size, 0);
    barrier(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _autoExposure.pipeline.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        _autoExposure.pipeline.layout, 0, 1, &_autoExposure.set, 0, nullptr);
    const glm::uvec4 push(_drawExtent.width, _drawExtent.height, slot * ExposureBins, 0);
    vkCmdPushConstants(cmd, _autoExposure.pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(push), &push);
    vkCmdDispatch(cmd, (_drawExtent.width + 15) / 16, (_drawExtent.height + 15) / 16, 1);
    barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkBufferCopy copy{offset, offset, size};
    vkCmdCopyBuffer(cmd, _autoExposure.histogram.buffer, _autoExposure.readback.buffer, 1, &copy);
    barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    if (_gpuTiming.supported) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            _autoExposure.timestampPool, slot * 2 + 1);
    }
    _autoExposure.written[slot] = true;
}

void VulkanEngine::update_auto_exposure(float deltaTime)
{
    // After this slot's fence: its last histogram and timing are final.
    const uint32_t slot = _frameNumber % FRAME_OVERLAP;
    if (!_autoExposure.written[slot]) {
        return;
    }
    _autoExposure.written[slot] = false;
    std::array<uint64_t, 2> ticks{};
    if (_gpuTiming.supported && vkGetQueryPoolResults(_device, _autoExposure.timestampPool,
            slot * 2, 2, sizeof(ticks), ticks.data(), sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        _autoExposure.gpuMilliseconds =
            float(ticks[1] - ticks[0]) * _gpuTiming.timestampPeriod * 1e-6f;
    }
    if (!_autoExposure.enabled) {
        return;
    }
    vmaInvalidateAllocation(_allocator, _autoExposure.readback.allocation, 0, VK_WHOLE_SIZE);
    std::array<uint32_t, ExposureBins> bins{};
    std::memcpy(bins.data(), static_cast<const uint32_t*>(
        _autoExposure.readback.info.pMappedData) + slot * ExposureBins, sizeof(bins));
    // Map the trimmed mean to 18% grey.
    const float meanLog2 = auto_exposure_mean_log2(bins);
    const float target = std::clamp(std::log2(0.18f) - meanLog2 + _autoExposure.compensation,
        _autoExposure.minEv, std::max(_autoExposure.minEv, _autoExposure.maxEv));
    _autoExposure.ev = _autoExposure.snap ? target : _autoExposure.ev +
        (target - _autoExposure.ev) * (1.0f - std::exp(-_autoExposure.speed * deltaTime));
    _autoExposure.snap = false;
    _postProcess.tonemapExposure = std::exp2(_autoExposure.ev);
    if (_autoExposure.logFrames) {
        fmt::print("Auto-exposure frame {}: EV {:.3f} (target {:.3f}, trimmed mean log2 Y {:.3f}, "
            "exposure {:.4f})\n", _frameNumber, _autoExposure.ev, target, meanLog2,
            _postProcess.tonemapExposure);
    }
}

void VulkanEngine::draw_tonemap(VkCommandBuffer cmd)
{
    // The composed linear frame becomes a texture and the pass resolves it
    // into the display-range image beside it.  Everything downstream of here
    // - anti-aliasing, the blit, ImGui - works in that range.
    vkutil::transition_image(
        cmd,
        _drawImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkutil::transition_image(
        cmd,
        _postProcess.tonemapImage.image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (_autoExposure.enabled && _autoExposure.pipeline.pipeline != VK_NULL_HANDLE) {
        record_auto_exposure(cmd);
    }

    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _postProcess.tonemapImage.imageView,
        nullptr,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, nullptr);
    vkCmdBeginRendering(cmd, &renderInfo);

    // _drawExtent rather than the allocation, for the same reason the
    // anti-aliasing pass uses it: below a resolution scale of 1 the rest of
    // the image holds nothing this frame produced.
    set_fullscreen_dynamic_state(cmd, _drawExtent);

    vkCmdBindPipeline(
        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _postProcess.tonemapPipeline.pipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        _postProcess.tonemapPipeline.layout,
        0,
        1,
        &_postProcess.tonemapInputDescriptor,
        0,
        nullptr);

    TonemapPushConstants pushConstants{};
    pushConstants.settings = glm::vec4(
        _postProcess.tonemapExposure,
        _postProcess.tonemapOperator == 1 ? 1.0f : 0.0f,
        _postProcess.tonemapBypassCurve ? 1.0f : 0.0f,
        0.0f);
    vkCmdPushConstants(
        cmd,
        _postProcess.tonemapPipeline.layout,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(TonemapPushConstants),
        &pushConstants);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);

    // Left readable rather than transfer-ready: anti-aliasing may still want
    // it as a texture.  draw() transitions it for the blit if it does not.
    vkutil::transition_image(
        cmd,
        _postProcess.tonemapImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanEngine::draw_fxaa(VkCommandBuffer cmd)
{
    // Its source is the tonemap's output, which that pass already left in a
    // readable layout, so only the target needs transitioning here.  Reading
    // and writing one image within a single draw has no defined result, which
    // is why the pair exists.
    vkutil::transition_image(
        cmd,
        _postProcess.image.image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _postProcess.image.imageView,
        nullptr,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, nullptr);
    vkCmdBeginRendering(cmd, &renderInfo);

    // _drawExtent, not the allocation: at a resolution scale below 1 the rest
    // of the image holds nothing this frame rendered, and the copy to the
    // swapchain reads only this region anyway.
    set_fullscreen_dynamic_state(cmd, _drawExtent);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _postProcess.fxaaPipeline.pipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        _postProcess.fxaaPipeline.layout,
        0,
        1,
        &_postProcess.fxaaInputDescriptor,
        0,
        nullptr);

    // The texel step is one pixel of the allocation, because that is what a
    // UV offset of one pixel means in this texture.  The limit is the last
    // pixel centre the render scale actually filled, so a search along a long
    // edge stops at the live region instead of sampling last frame's leftover.
    const glm::vec2 allocationSize(
        static_cast<float>(_drawImage.imageExtent.width),
        static_cast<float>(_drawImage.imageExtent.height));
    const glm::vec2 texelSize = 1.0f / allocationSize;
    const glm::vec2 renderedLimit = glm::vec2(
        (static_cast<float>(_drawExtent.width) - 0.5f) / allocationSize.x,
        (static_cast<float>(_drawExtent.height) - 0.5f) / allocationSize.y);

    FXAAPushConstants pushConstants{};
    pushConstants.settings = glm::vec4(
        texelSize.x, texelSize.y, _postProcess.fxaaEdgeThreshold, _postProcess.fxaaSubpixelStrength);
    pushConstants.limits = glm::vec4(
        renderedLimit.x, renderedLimit.y, _postProcess.fxaaShowEdges ? 1.0f : 0.0f, 0.0f);
    vkCmdPushConstants(
        cmd,
        _postProcess.fxaaPipeline.layout,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(FXAAPushConstants),
        &pushConstants);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);

    vkutil::transition_image(
        cmd,
        _postProcess.image.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}

