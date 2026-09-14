#include "vk_engine.h"
#include "vk_engine_render_helpers.h"

#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_pipelines.h>

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

