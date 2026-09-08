#include "vk_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>

#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_loader.h>
#include <vk_pipelines.h>

namespace {

glm::vec3 normalized_sun_direction(const glm::vec3& direction)
{
    if (glm::dot(direction, direction) < 0.000001f) {
        return glm::normalize(glm::vec3(0.0f, 1.0f, 0.5f));
    }
    return glm::normalize(direction);
}

} // namespace

bool is_visible(const RenderObject& object, const glm::mat4& viewProjection)
{
    const std::array<glm::vec3, 8> corners = {
        glm::vec3{1, 1, 1}, glm::vec3{1, 1, -1},
        glm::vec3{1, -1, 1}, glm::vec3{1, -1, -1},
        glm::vec3{-1, 1, 1}, glm::vec3{-1, 1, -1},
        glm::vec3{-1, -1, 1}, glm::vec3{-1, -1, -1}};

    const glm::mat4 objectMatrix = viewProjection * object.transform;
    glm::vec3 minClip{1.5f};
    glm::vec3 maxClip{-1.5f};

    for (const glm::vec3& corner : corners) {
        glm::vec4 projected = objectMatrix * glm::vec4(
            object.bounds.origin + corner * object.bounds.extents,
            1.0f);
        if (projected.w <= 0.0f) {
            return true;
        }
        projected /= projected.w;
        minClip = glm::min(minClip, glm::vec3(projected));
        maxClip = glm::max(maxClip, glm::vec3(projected));
    }

    return !(minClip.z > 1.0f || maxClip.z < 0.0f ||
             minClip.x > 1.0f || maxClip.x < -1.0f ||
             minClip.y > 1.0f || maxClip.y < -1.0f);
}

void VulkanEngine::draw_background(VkCommandBuffer cmd)
{
	//make a clear-color from frame number. This will flash with a 120 frame period.
	VkClearColorValue clearValue;
	float flash = std::abs(std::sin(_frameNumber / 120.f));
	clearValue = { { 0.0f, 0.0f, flash, 1.0f } };

	VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);

	//clear image
	vkCmdClearColorImage(cmd, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &clearRange);
}

void VulkanEngine::init_descriptors()
{
    std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes =
    {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 }
    };

    globalDescriptorAllocator.init(_device, 10, sizes);

    for (FrameData& frame : _frames) {
        frame._frameDescriptors.init(_device, 1000, sizes);
    }

    // make the descriptor set layout for our compute to draw
    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        // The gradient shader ignores this binding, while the panorama sky
        // shader samples it as a regular 2D equirectangular texture.
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _drawImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
    }

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _singleImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_FRAGMENT_BIT);
    }

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        // The sunlight depth map lives beside the scene data because every
        // lit pass needs it, the portal cameras included.
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _gpuSceneDataDescriptorLayout = builder.build(
            _device,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    }

    for (FrameData& frame : _frames) {
        frame.sceneBuffer = create_buffer(
            sizeof(GPUSceneData),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU);
        // These sets live for the frame's lifetime.  They cannot come from
        // _frameDescriptors because that allocator is reset every frame.
        frame.sceneDescriptor = globalDescriptorAllocator.allocate(
            _device, _gpuSceneDataDescriptorLayout);
        DescriptorWriter sceneWriter;
        sceneWriter.write_buffer(
            0,
            frame.sceneBuffer.buffer,
            sizeof(GPUSceneData),
            0,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        sceneWriter.write_image(
            1,
            _shadowMapImage.imageView,
            _shadowSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        sceneWriter.update_set(_device, frame.sceneDescriptor);

        for (uint32_t view = 0; view < PortalViewCount; ++view) {
            frame.portalSceneBuffers[view] = create_buffer(
                sizeof(GPUSceneData),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VMA_MEMORY_USAGE_CPU_TO_GPU);
            frame.portalSceneDescriptors[view] = globalDescriptorAllocator.allocate(
                _device, _gpuSceneDataDescriptorLayout);

            DescriptorWriter portalSceneWriter;
            portalSceneWriter.write_buffer(
                0,
                frame.portalSceneBuffers[view].buffer,
                sizeof(GPUSceneData),
                0,
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            portalSceneWriter.write_image(
                1,
                _shadowMapImage.imageView,
                _shadowSampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            portalSceneWriter.update_set(
                _device, frame.portalSceneDescriptors[view]);
        }
    }

    // allocate a descriptor set for our draw image

    _drawImageDescriptors = globalDescriptorAllocator.allocate(_device, _drawImageDescriptorLayout);

    DescriptorWriter writer;
    writer.write_image(
        0,
        _drawImage.imageView,
        VK_NULL_HANDLE,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    writer.update_set(_device, _drawImageDescriptors);

    //make sure both the des alloc and new layout get cleaned up properly
    _mainDeletionQueue.push_function([&](){
        globalDescriptorAllocator.destroy_pools(_device);
        for (FrameData& frame : _frames) {
            frame._frameDescriptors.destroy_pools(_device);
            destroy_buffer(frame.sceneBuffer);
            for (AllocatedBuffer& portalSceneBuffer : frame.portalSceneBuffers) {
                destroy_buffer(portalSceneBuffer);
            }
        }

        vkDestroyDescriptorSetLayout(_device, _drawImageDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _singleImageDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _gpuSceneDataDescriptorLayout, nullptr);
    });
}

void VulkanEngine::init_pipelines()
{
    init_background_pipelines();
    init_shadow_pipeline();
    metalRoughMaterial.build_pipelines(this);
}

void VulkanEngine::draw_geometry(
    VkCommandBuffer cmd,
    const DrawContext& drawContext,
    const glm::mat4& viewProjection,
    VkDescriptorSet sceneDescriptor,
    bool clearDepthAndStencil,
    MaterialPipeline* overridePipeline,
    uint32_t stencilReference,
    bool useFrustumCulling,
    uint32_t stencilCompareMask)
{
    const auto startTime = std::chrono::steady_clock::now();

    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    depthAttachment.loadOp = clearDepthAndStencil
        ? VK_ATTACHMENT_LOAD_OP_CLEAR
        : VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderingAttachmentInfo stencilAttachment = depthAttachment;

    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, &depthAttachment);
    renderInfo.pStencilAttachment = &stencilAttachment;

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
    vkCmdSetStencilReference(
        cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencilReference);
    vkCmdSetStencilCompareMask(
        cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencilCompareMask);
    vkCmdSetStencilWriteMask(
        cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0x00);

    std::vector<uint32_t> opaqueDraws;
    opaqueDraws.reserve(drawContext.OpaqueSurfaces.size());
    for (uint32_t i = 0; i < drawContext.OpaqueSurfaces.size(); ++i) {
        // Oblique portal projections replace their near plane.  The simple
        // CPU frustum test above assumes an ordinary projection, so using it
        // there wrongly throws away visible destination walls and exposes the
        // background.  The GPU still performs the real clip/depth tests.
        if (!useFrustumCulling ||
            is_visible(drawContext.OpaqueSurfaces[i], viewProjection)) {
            opaqueDraws.push_back(i);
        }
    }

    std::sort(
        opaqueDraws.begin(),
        opaqueDraws.end(),
        [&](uint32_t leftIndex, uint32_t rightIndex) {
            const RenderObject& left = drawContext.OpaqueSurfaces[leftIndex];
            const RenderObject& right = drawContext.OpaqueSurfaces[rightIndex];
            if (left.material == right.material) {
                return left.indexBuffer < right.indexBuffer;
            }
            return std::less<MaterialInstance*>{}(left.material, right.material);
        });

    MaterialPipeline* lastPipeline = nullptr;
    MaterialInstance* lastMaterial = nullptr;
    VkBuffer lastIndexBuffer = VK_NULL_HANDLE;

    auto draw = [&](const RenderObject& renderObject) {
        if (renderObject.material == nullptr) {
            return;
        }

        MaterialPipeline* pipeline = overridePipeline != nullptr
            ? overridePipeline
            : renderObject.material->pipeline;
        if (pipeline != lastPipeline) {
            lastPipeline = pipeline;
            vkCmdBindPipeline(
                cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
            vkCmdBindDescriptorSets(
                cmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline->layout,
                0,
                1,
                &sceneDescriptor,
                0,
                nullptr);
        }
        if (renderObject.material != lastMaterial) {
            lastMaterial = renderObject.material;
            vkCmdBindDescriptorSets(
                cmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline->layout,
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
            pipeline->layout,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(GPUDrawPushConstants),
            &pushConstants);
        vkCmdDrawIndexed(
            cmd, renderObject.indexCount, 1, renderObject.firstIndex, 0, 0);

        ++stats.drawcall_count;
        stats.triangle_count += static_cast<int>(renderObject.indexCount / 3);
    };

    for (uint32_t drawIndex : opaqueDraws) {
        draw(drawContext.OpaqueSurfaces[drawIndex]);
    }
    for (const RenderObject& renderObject : drawContext.TransparentSurfaces) {
        draw(renderObject);
    }

    vkCmdEndRendering(cmd);

    stats.mesh_draw_time += std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - startTime).count();
}

GPUSceneData VulkanEngine::build_scene_data(const glm::mat4& view) const
{
    GPUSceneData data{};
    data.view = view;
    // Passing far, then near, intentionally builds the reversed-depth
    // projection used by the existing GREATER_OR_EQUAL depth pipeline.
    data.proj = glm::perspective(
        glm::radians(70.0f),
        static_cast<float>(_drawExtent.width) / static_cast<float>(_drawExtent.height),
        10000.0f,
        0.1f);
    data.proj[1][1] *= -1.0f;
    data.viewproj = data.proj * data.view;
    // Keep authored interiors readable even when a face is turned away from
    // the single directional sun light.
    data.ambientColor = glm::vec4(0.28f);
    data.sunlightDirection = glm::vec4(
        normalized_sun_direction(_sunlightDirection), 1.0f);
    data.sunlightColor = glm::vec4(1.0f);
    // update_scene() settles this before any camera's buffer is filled, so
    // the portal views inherit exactly the main camera's shadow map.
    data.sunViewProjection = _sunViewProjection;
    data.shadowSettings = glm::vec4(
        _shadowDepthBias,
        _shadowNormalBias,
        1.0f / static_cast<float>(ShadowMapResolution),
        _shadowsEnabled ? 1.0f : 0.0f);
    return data;
}

glm::mat4 VulkanEngine::compute_sun_view_projection(const glm::vec3& focusPoint) const
{
    const glm::vec3 toLight = normalized_sun_direction(_sunlightDirection);
    // glm::lookAt degenerates when its up vector lies along the view axis.
    const glm::vec3 up = std::abs(toLight.y) > 0.99f
        ? glm::vec3(0.0f, 0.0f, 1.0f)
        : glm::vec3(0.0f, 1.0f, 0.0f);
    // Rotation only.  Pinning the light's origin to the world origin is what
    // gives the snap below a texel grid that does not move with the player.
    const glm::mat4 lightRotation = glm::lookAt(
        glm::vec3(0.0f), -toLight, up);

    glm::vec3 focus = glm::vec3(lightRotation * glm::vec4(focusPoint, 1.0f));
    // Without this the grid slides continuously under the world as the player
    // walks, and every shadow edge crawls.
    const float texelWorldSize =
        (2.0f * _shadowRadius) / static_cast<float>(ShadowMapResolution);
    focus.x = std::floor(focus.x / texelWorldSize) * texelWorldSize;
    focus.y = std::floor(focus.y / texelWorldSize) * texelWorldSize;

    // The extra depth reaches casters standing outside the lit box; only the
    // x/y extent decides how much ground can receive a shadow.
    const float depthHalfRange = _shadowRadius + _shadowDepthMargin;
    glm::mat4 projection = glm::ortho(
        focus.x - _shadowRadius,
        focus.x + _shadowRadius,
        focus.y - _shadowRadius,
        focus.y + _shadowRadius,
        -focus.z - depthHalfRange,
        -focus.z + depthHalfRange);
    // Vulkan's framebuffer Y runs opposite glm's, exactly as the main camera
    // projection corrects it.
    projection[1][1] *= -1.0f;
    return projection * lightRotation;
}

void VulkanEngine::init_shadow_resources()
{
    // Comparison sampling is a format capability, not a guarantee of every
    // depth format. Prefer D32 with bilinear comparison filtering, then fall
    // back through other depth-only formats and nearest filtering if needed.
    const std::array<VkFormat, 3> candidates{
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D16_UNORM,
        VK_FORMAT_X8_D24_UNORM_PACK32};
    constexpr VkFormatFeatureFlags2 requiredFeatures =
        VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT;
    VkFormat shadowFormat = VK_FORMAT_UNDEFINED;
    VkFilter shadowFilter = VK_FILTER_NEAREST;
    VkFormat nearestFallback = VK_FORMAT_UNDEFINED;
    for (VkFormat candidate : candidates) {
        VkFormatProperties3 properties3{
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 properties2{
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
            .pNext = &properties3};
        vkGetPhysicalDeviceFormatProperties2(
            _chosenGPU, candidate, &properties2);
        if ((properties3.optimalTilingFeatures & requiredFeatures) !=
            requiredFeatures) {
            continue;
        }
        if (nearestFallback == VK_FORMAT_UNDEFINED) {
            nearestFallback = candidate;
        }
        if ((properties3.optimalTilingFeatures &
             VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0) {
            shadowFormat = candidate;
            shadowFilter = VK_FILTER_LINEAR;
            break;
        }
    }
    if (shadowFormat == VK_FORMAT_UNDEFINED) {
        shadowFormat = nearestFallback;
    }
    if (shadowFormat == VK_FORMAT_UNDEFINED) {
        fmt::print("No depth format supports sampled shadow comparison\n");
        abort();
    }

    _shadowMapImage = create_image(
        VkExtent3D{ShadowMapResolution, ShadowMapResolution, 1},
        shadowFormat,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    // A comparison sampler runs the depth test in hardware and filters the
    // results, which is what makes each PCF tap cheap.  The white border
    // leaves everything outside the shadowed box lit, instead of stamping a
    // dark square edge onto the world.
    VkSamplerCreateInfo samplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = shadowFilter;
    samplerInfo.minFilter = shadowFilter;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    samplerInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(_device, &samplerInfo, nullptr, &_shadowSampler));

    _mainDeletionQueue.push_function([this]() {
        vkDestroySampler(_device, _shadowSampler, nullptr);
        destroy_image(_shadowMapImage);
    });
}

void VulkanEngine::init_shadow_pipeline()
{
    VkShaderModule shadowVertexShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module(
            "../../shaders/shadow_depth.vert.spv", _device, &shadowVertexShader)) {
        fmt::print("Error loading shadow depth shader\n");
        return;
    }

    // No descriptor sets at all: the light matrix arrives already multiplied
    // into the push constant, and a depth-only pass reads no material.
    VkPushConstantRange pushRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(ShadowPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &layoutInfo, nullptr, &_shadowPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _shadowPipeline.layout;
    builder.set_vertex_only_shader(shadowVertexShader);
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    // The level's walls and floors are single-sided quads, so culling here
    // would simply delete their shadows.  Depth bias, not back-face culling,
    // is what stops a surface from shadowing itself.
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    builder.disable_blending();
    builder.disable_color_attachment();
    // Conventional depth, unlike the reversed-depth main camera: this pass
    // clears to 1 and keeps whatever lies nearest the sun.
    builder.enable_depthtest(true, VK_COMPARE_OP_LESS_OR_EQUAL);
    builder.enable_depth_bias(1.25f, 2.75f);
    builder.set_depth_format(_shadowMapImage.imageFormat);
    _shadowPipeline.pipeline = builder.build_pipeline(_device);

    vkDestroyShaderModule(_device, shadowVertexShader, nullptr);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _shadowPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _shadowPipeline.layout, nullptr);
    });
}

void VulkanEngine::draw_shadow_map(VkCommandBuffer cmd)
{
    vkutil::transition_image(
        cmd,
        _shadowMapImage.image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _shadowMapImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    // The main camera clears to 0 because its depth is reversed.  Here 1 is
    // the far value, and it means nothing stands between this texel and the
    // sun.
    depthAttachment.clearValue.depthStencil.depth = 1.0f;

    const VkExtent2D shadowExtent{ShadowMapResolution, ShadowMapResolution};
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        shadowExtent, nullptr, &depthAttachment);

    vkCmdBeginRendering(cmd, &renderInfo);

    VkViewport viewport{};
    viewport.width = static_cast<float>(shadowExtent.width);
    viewport.height = static_cast<float>(shadowExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = shadowExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    // This pass has no stencil attachment, but the shared pipeline builder
    // declares the stencil state dynamic on every pipeline it produces.
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0x00);

    if (_shadowsEnabled && _shadowPipeline.pipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(
            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _shadowPipeline.pipeline);

        VkBuffer lastIndexBuffer = VK_NULL_HANDLE;
        // portalViewDrawContext is the world plus the player's body, which is
        // what actually exists in the level.  The main camera's context is
        // not usable here: it also carries the portal quads, and it omits the
        // body the first-person camera sits inside.
        // Transparent surfaces intentionally do not cast yet. Supporting
        // foliage or grates requires a material-aware alpha-tested pass.
        for (const RenderObject& renderObject :
                portalViewDrawContext.OpaqueSurfaces) {
            // Cull against the light, never against the player's camera: an
            // object behind the camera can still drop a shadow into view.
            if (!is_visible(renderObject, _sunViewProjection)) {
                continue;
            }

            if (renderObject.indexBuffer != lastIndexBuffer) {
                lastIndexBuffer = renderObject.indexBuffer;
                vkCmdBindIndexBuffer(
                    cmd, renderObject.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            }

            ShadowPushConstants pushConstants{};
            pushConstants.lightMatrix = _sunViewProjection * renderObject.transform;
            pushConstants.vertexBuffer = renderObject.vertexBufferAddress;
            vkCmdPushConstants(
                cmd,
                _shadowPipeline.layout,
                VK_SHADER_STAGE_VERTEX_BIT,
                0,
                sizeof(ShadowPushConstants),
                &pushConstants);
            vkCmdDrawIndexed(
                cmd, renderObject.indexCount, 1, renderObject.firstIndex, 0, 0);
        }
    }

    vkCmdEndRendering(cmd);

    vkutil::transition_image(
        cmd,
        _shadowMapImage.image,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_ASPECT_DEPTH_BIT);
}

void VulkanEngine::init_background_pipelines()
{
    VkPipelineLayoutCreateInfo computeLayout{};
    computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    computeLayout.pNext = nullptr;
    computeLayout.pSetLayouts = &_drawImageDescriptorLayout;
    computeLayout.setLayoutCount = 1;

    VkPushConstantRange pushConstant{};
    pushConstant.offset = 0;
    pushConstant.size = sizeof(ComputePushConstants);
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    computeLayout.pPushConstantRanges = &pushConstant;
    computeLayout.pushConstantRangeCount = 1;

    VkShaderModule gradientShader;
	if (!vkutil::load_shader_module("../../shaders/gradient_color.comp.spv", _device, &gradientShader))
	{
		fmt::print("Error loading gradient_color compute shader\n");
		return;
	}

    VkShaderModule skyShader;
	if (!vkutil::load_shader_module("../../shaders/sky.comp.spv", _device, &skyShader))
	{
		fmt::print("Error loading sky compute shader\n");
		vkDestroyShaderModule(_device, gradientShader, nullptr);
		return;
	}

    VkPipelineShaderStageCreateInfo stageinfo{};
	stageinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageinfo.pNext = nullptr;
	stageinfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageinfo.module = gradientShader;
	stageinfo.pName = "main";

    VK_CHECK(vkCreatePipelineLayout(_device, &computeLayout, nullptr, &_gradientPipelineLayout));

    VkComputePipelineCreateInfo computePipelineCreateInfo{};
	computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	computePipelineCreateInfo.pNext = nullptr;
	computePipelineCreateInfo.layout = _gradientPipelineLayout;
	computePipelineCreateInfo.stage = stageinfo;

    ComputeEffect gradient{};
    gradient.name = "gradient";
    gradient.layout = _gradientPipelineLayout;
    gradient.data.data1 = glm::vec4(1, 0, 0, 1);
    gradient.data.data2 = glm::vec4(0, 0, 1, 1);
    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &gradient.pipeline));

    stageinfo.module = skyShader;
    computePipelineCreateInfo.stage = stageinfo;

    ComputeEffect sky{};
    sky.name = "sky";
    sky.layout = _gradientPipelineLayout;
    sky.data.data1 = glm::vec4(0.1f, 0.2f, 0.4f, 0.97f);
    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &sky.pipeline));

    backgroundEffects.clear();
    backgroundEffects.push_back(gradient);
    backgroundEffects.push_back(sky);

    vkDestroyShaderModule(_device, gradientShader, nullptr);
    vkDestroyShaderModule(_device, skyShader, nullptr);

	_mainDeletionQueue.push_function([this, gradientPipeline = gradient.pipeline, skyPipeline = sky.pipeline]() {
		vkDestroyPipeline(_device, gradientPipeline, nullptr);
		vkDestroyPipeline(_device, skyPipeline, nullptr);
		vkDestroyPipelineLayout(_device, _gradientPipelineLayout, nullptr);
		});
}

void VulkanEngine::draw_collider_debug_bounds(VkCommandBuffer cmd)
{
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderingAttachmentInfo stencilAttachment = depthAttachment;
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, &depthAttachment);
    renderInfo.pStencilAttachment = &stencilAttachment;

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

    vkCmdBindPipeline(
        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _colliderDebugPipeline.pipeline);
    for (const SceneObject& object : _scene.objects) {
        if (!_showColliderBounds || !object.alive || !object.hasCollision) {
            continue;
        }

        // Draw the final world AABB used by physics—not merely the object's
        // local box—so a rotated object still shows the conservative bounds
        // the player and portal raycast actually use.
        AABB collider{};
        if (object.collisionShape == CollisionShape::GroundPlane) {
            // Ground-plane physics is intentionally zero-thickness. Give it a
            // thin debug-only box so platform colliders are still visible.
            const glm::mat4 world = _scene.world_matrix(object.id);
            const glm::vec3 planeCenter = glm::vec3(world[3]);
            const glm::vec3 planeHalfExtents{
                glm::length(glm::vec3(world[0])) * 0.5f,
                0.01f,
                glm::length(glm::vec3(world[2])) * 0.5f};
            collider.min = planeCenter - planeHalfExtents;
            collider.max = planeCenter + planeHalfExtents;
        } else {
            collider = collider_from_object(_scene, object.id);
        }
        const glm::vec3 center = (collider.min + collider.max) * 0.5f;
        const glm::vec3 fullExtents = (collider.max - collider.min) * 1.005f;
        const glm::mat4 colliderTransform = glm::translate(
            glm::mat4(1.0f), center) * glm::scale(
            glm::mat4(1.0f), fullExtents);

        ColliderDebugPushConstants pushConstants{};
        pushConstants.viewProjection = sceneData.viewproj;
        pushConstants.model = colliderTransform;
        vkCmdPushConstants(
            cmd,
            _colliderDebugPipeline.layout,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(ColliderDebugPushConstants),
            &pushConstants);
        vkCmdDraw(cmd, 24, 1, 0, 0);
        ++stats.drawcall_count;
    }

    if (_showShadowBounds) {
        // Convert the unit debug cube into the exact orthographic volume used
        // by the shadow map: clip x/y are [-1, 1], while Vulkan depth is [0, 1].
        const glm::mat4 clipBox = glm::translate(
            glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.5f)) * glm::scale(
            glm::mat4(1.0f), glm::vec3(2.0f, 2.0f, 1.0f));
        ColliderDebugPushConstants pushConstants{};
        pushConstants.viewProjection = sceneData.viewproj;
        pushConstants.model = glm::inverse(_sunViewProjection) * clipBox;
        vkCmdPushConstants(
            cmd,
            _colliderDebugPipeline.layout,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(ColliderDebugPushConstants),
            &pushConstants);
        vkCmdDraw(cmd, 24, 1, 0, 0);
        ++stats.drawcall_count;
    }
    vkCmdEndRendering(cmd);
}
