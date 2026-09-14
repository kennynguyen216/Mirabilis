#include "vk_engine.h"
#include "vk_engine_render_helpers.h"

#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_pipelines.h>

void VulkanEngine::init_prepass_descriptors()
{
    {
        // Camera depth and view-space normals, the inputs every screen-space
        // pass shares.  Neither image is recreated at runtime, so one set
        // describes them for the lifetime of the engine.
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _prepassImageDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
        _prepassImageDescriptor = globalDescriptorAllocator.allocate(
            _device, _prepassImageDescriptorLayout);

        DescriptorWriter writer;
        writer.write_image(
            0,
            _prepassDepthImage.imageView,
            _prepassSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writer.write_image(
            1,
            _prepassNormalImage.imageView,
            _prepassSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writer.update_set(_device, _prepassImageDescriptor);
    }
}

void VulkanEngine::init_depth_normal_resources()
{
    // Screen-space effects sample this depth, so an attachment-only format is
    // not enough. Ask the device which candidate supports both uses instead
    // of assuming, exactly as the shadow map and the main depth buffer do.
    static constexpr std::array<VkFormat, 2> depthCandidates{
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D16_UNORM};
    constexpr VkFormatFeatureFlags requiredDepthFeatures =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    VkFormat prepassDepthFormat = VK_FORMAT_UNDEFINED;
    for (VkFormat candidate : depthCandidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(_chosenGPU, candidate, &properties);
        if ((properties.optimalTilingFeatures & requiredDepthFeatures) ==
            requiredDepthFeatures) {
            prepassDepthFormat = candidate;
            break;
        }
    }
    if (prepassDepthFormat == VK_FORMAT_UNDEFINED) {
        fmt::print("No depth format supports a sampled depth prepass\n");
        abort();
    }

    // Allocated at the full window size and only partly written when the
    // resolution scale is below 1, which is what lets renderScale change
    // without recreating any of these images.
    const VkExtent3D prepassExtent = _drawImage.imageExtent;
    _prepassDepthImage = create_image(
        prepassExtent,
        prepassDepthFormat,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    // Wider than normals need, but trivial to read in the debug views. Two
    // signed 16-bit channels with octahedral encoding is the later saving.
    _prepassNormalImage = create_image(
        prepassExtent,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    // Nearest and clamped: these buffers are read per pixel, and filtering
    // across a silhouette would blend two unrelated surfaces into a position
    // that exists nowhere in the scene.
    VkSamplerCreateInfo samplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(_device, &samplerInfo, nullptr, &_prepassSampler));

    _mainDeletionQueue.push_function([this]() {
        vkDestroySampler(_device, _prepassSampler, nullptr);
        destroy_image(_prepassNormalImage);
        destroy_image(_prepassDepthImage);
    });
}

void VulkanEngine::init_depth_normal_pipeline()
{
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module(
            "../../shaders/depth_normal.vert.spv", _device, &vertexShader) ||
        !vkutil::load_shader_module(
            "../../shaders/depth_normal.frag.spv", _device, &fragmentShader)) {
        fmt::print("Error loading depth/normal prepass shaders\n");
        if (vertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, vertexShader, nullptr);
        }
        if (fragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, fragmentShader, nullptr);
        }
        return;
    }

    // Scene data only. The prepass reads no material, and it takes the same
    // push constants as the main pass so both can share one draw loop.
    VkPushConstantRange matrixRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(GPUDrawPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &_gpuSceneDataDescriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &matrixRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &layoutInfo, nullptr, &_depthNormalPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _depthNormalPipeline.layout;
    builder.set_shaders(vertexShader, fragmentShader);
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    // Every rasterizer state below matches the main opaque pipeline. If the
    // two disagree, occlusion computed here lands on the wrong pixels of the
    // shaded image.
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    builder.disable_blending();
    builder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
    builder.set_color_attachment_format(_prepassNormalImage.imageFormat);
    builder.set_depth_format(_prepassDepthImage.imageFormat);
    _depthNormalPipeline.pipeline = builder.build_pipeline(_device);

    vkDestroyShaderModule(_device, fragmentShader, nullptr);
    vkDestroyShaderModule(_device, vertexShader, nullptr);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _depthNormalPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _depthNormalPipeline.layout, nullptr);
    });
}

void VulkanEngine::init_depth_normal_mask_pipeline()
{
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module(
            "../../shaders/depth_normal_mask.vert.spv", _device, &vertexShader) ||
        !vkutil::load_shader_module(
            "../../shaders/depth_normal_mask.frag.spv", _device, &fragmentShader)) {
        fmt::print("Error loading alpha-tested prepass shaders\n");
        if (vertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, vertexShader, nullptr);
        }
        if (fragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, fragmentShader, nullptr);
        }
        return;
    }

    // Scene data and the material, in the same order the forward pass uses
    // them, so a material set written for that pass binds here unchanged.
    VkDescriptorSetLayout layouts[] = {
        _gpuSceneDataDescriptorLayout,
        metalRoughMaterial.materialLayout};
    VkPushConstantRange matrixRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(GPUDrawPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &matrixRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &layoutInfo, nullptr, &_depthNormalMaskPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _depthNormalMaskPipeline.layout;
    builder.set_shaders(vertexShader, fragmentShader);
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    // Matching the opaque prepass for the same reason it matches the main
    // pass: occlusion computed here has to land on the shaded image's pixels.
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    builder.disable_blending();
    builder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
    builder.set_color_attachment_format(_prepassNormalImage.imageFormat);
    builder.set_depth_format(_prepassDepthImage.imageFormat);
    _depthNormalMaskPipeline.pipeline = builder.build_pipeline(_device);

    vkDestroyShaderModule(_device, fragmentShader, nullptr);
    vkDestroyShaderModule(_device, vertexShader, nullptr);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _depthNormalMaskPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _depthNormalMaskPipeline.layout, nullptr);
    });
}

void VulkanEngine::draw_depth_normal_prepass(VkCommandBuffer cmd)
{
    const auto startTime = std::chrono::steady_clock::now();

    vkutil::transition_image(
        cmd,
        _prepassNormalImage.image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkutil::transition_image(
        cmd,
        _prepassDepthImage.image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    // A zero normal marks a pixel no surface covered, which is how the debug
    // views and later the occlusion pass recognise the background.
    VkClearValue normalClear{};
    normalClear.color.float32[0] = 0.0f;
    normalClear.color.float32[1] = 0.0f;
    normalClear.color.float32[2] = 0.0f;
    normalClear.color.float32[3] = 0.0f;
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _prepassNormalImage.imageView,
        &normalClear,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    // depth_attachment_info already clears to 0, the far value under the main
    // camera's reversed depth. This pass shares that convention deliberately,
    // unlike the shadow map, so its depth could later replace the main one.
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _prepassDepthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, &depthAttachment);
    vkCmdBeginRendering(cmd, &renderInfo);

    VkViewport viewport{};
    viewport.width = static_cast<float>(_drawExtent.width);
    viewport.height = static_cast<float>(_drawExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = _drawExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    // No stencil attachment here, but the shared builder makes stencil state
    // dynamic on every pipeline it produces.
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0x00);

    if (_depthNormalPipeline.pipeline != VK_NULL_HANDLE) {
        // Both prepass layouts declare the scene set at index 0 with the same
        // push constant range, which makes them compatible for that set: it
        // survives a switch between the two pipelines and is bound once.
        vkCmdBindPipeline(
            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _depthNormalPipeline.pipeline);
        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            _depthNormalPipeline.layout,
            0,
            1,
            &get_current_frame().sceneDescriptor,
            0,
            nullptr);

        VkPipeline lastPipeline = _depthNormalPipeline.pipeline;
        MaterialInstance* lastMaterial = nullptr;
        VkBuffer lastIndexBuffer = VK_NULL_HANDLE;
        // mainDrawContext and the main camera's frustum test, because these
        // buffers must describe the image the player is actually looking at.
        // Transparent surfaces are left out for now: one depth and one normal
        // per pixel cannot describe a surface seen through another.  Masked
        // surfaces are not in that category - they are opaque wherever they
        // draw at all - so they belong here, drawn through a pipeline that
        // runs their cutoff test.
        for (const RenderObject& renderObject : mainDrawContext.OpaqueSurfaces) {
            if (!is_visible(renderObject, sceneData.viewproj)) {
                continue;
            }

            const bool masked = renderObject.material != nullptr &&
                renderObject.material->passType == MaterialPass::Mask &&
                _depthNormalMaskPipeline.pipeline != VK_NULL_HANDLE;
            const MaterialPipeline& active = masked
                ? _depthNormalMaskPipeline
                : _depthNormalPipeline;
            if (active.pipeline != lastPipeline) {
                lastPipeline = active.pipeline;
                vkCmdBindPipeline(
                    cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, active.pipeline);
            }
            if (masked && renderObject.material != lastMaterial) {
                lastMaterial = renderObject.material;
                vkCmdBindDescriptorSets(
                    cmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    active.layout,
                    1,
                    1,
                    &renderObject.material->materialSet,
                    0,
                    nullptr);
            }

            if (renderObject.indexBuffer != lastIndexBuffer) {
                lastIndexBuffer = renderObject.indexBuffer;
                vkCmdBindIndexBuffer(
                    cmd, renderObject.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            }

            GPUDrawPushConstants pushConstants{};
            pushConstants.worldMatrix = renderObject.transform;
            pushConstants.vertexBuffer = renderObject.vertexBufferAddress;
            vkCmdPushConstants(
                cmd,
                active.layout,
                VK_SHADER_STAGE_VERTEX_BIT,
                0,
                sizeof(GPUDrawPushConstants),
                &pushConstants);
            vkCmdDrawIndexed(
                cmd, renderObject.indexCount, 1, renderObject.firstIndex, 0, 0);

            ++stats.prepass_drawcall_count;
            stats.prepass_triangle_count +=
                static_cast<int>(renderObject.indexCount / 3);
        }
    }

    vkCmdEndRendering(cmd);

    vkutil::transition_image(
        cmd,
        _prepassNormalImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkutil::transition_image(
        cmd,
        _prepassDepthImage.image,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    stats.prepass_record_time = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - startTime).count();
}

