#include "vk_engine.h"
#include "vk_engine_render_helpers.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/packing.hpp>

#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_loader.h>
#include <vk_pipelines.h>

namespace {

void set_previous_world_rows(
    GPUDrawPushConstants& pushConstants,
    const glm::mat4& previousWorld)
{
    pushConstants.previousWorldRow0 = glm::vec4(
        previousWorld[0][0], previousWorld[1][0],
        previousWorld[2][0], previousWorld[3][0]);
    pushConstants.previousWorldRow1 = glm::vec4(
        previousWorld[0][1], previousWorld[1][1],
        previousWorld[2][1], previousWorld[3][1]);
    pushConstants.previousWorldRow2 = glm::vec4(
        previousWorld[0][2], previousWorld[1][2],
        previousWorld[2][2], previousWorld[3][2]);
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

VkImageView VulkanEngine::ssao_occlusion_view() const
{
    return _ssaoFinalImage.imageView != VK_NULL_HANDLE
        ? _ssaoFinalImage.imageView
        : _shadowMapImage.imageView;
}

VkSampler VulkanEngine::ssao_occlusion_sampler() const
{
    return _ssaoSampler != VK_NULL_HANDLE ? _ssaoSampler : _prepassSampler;
}

void VulkanEngine::init_descriptor_pools()
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
}

void VulkanEngine::init_background_descriptors()
{
    // make the descriptor set layout for our compute to draw
    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        // The gradient shader ignores this binding, while the panorama sky
        // shader samples it as a regular 2D equirectangular texture.
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _drawImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
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
}

void VulkanEngine::init_post_process_descriptors()
{
    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _singleImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_FRAGMENT_BIT);

        // Both post-process passes want exactly that shape: one sampled
        // colour image.  Every image involved outlives every frame, so these
        // sets are written once here rather than rebuilt per frame.
        _tonemapInputDescriptor = globalDescriptorAllocator.allocate(
            _device, _singleImageDescriptorLayout);

        DescriptorWriter tonemapWriter;
        tonemapWriter.write_image(
            0,
            _drawImage.imageView,
            _postProcessSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        tonemapWriter.update_set(_device, _tonemapInputDescriptor);

        // Anti-aliasing reads the tonemap's output, not the draw image.  Its
        // edge search compares lumas against fixed thresholds, which only
        // mean anything once the values are in display range.
        _fxaaInputDescriptor = globalDescriptorAllocator.allocate(
            _device, _singleImageDescriptorLayout);

        DescriptorWriter fxaaWriter;
        fxaaWriter.write_image(
            0,
            _tonemapImage.imageView,
            _postProcessSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        fxaaWriter.update_set(_device, _fxaaInputDescriptor);
    }
}

void VulkanEngine::init_scene_descriptors()
{
    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        // The sunlight depth map lives beside the scene data because every
        // lit pass needs it, the portal cameras included.
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        // So does the finished ambient occlusion.  Portal cameras are bound
        // it as well, and a flag in the block itself is what stops them
        // reading occlusion computed for a different view.
        builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        // And the environment panorama.  It used to belong to the SSGI trace
        // alone, but a portal camera has no trace to fall back from and needs
        // the same sky directly, so it lives with the scene data every camera
        // already binds.  update_skybox_descriptors() is what fills it: the
        // panorama is loaded by init_default_data(), after this runs.
        builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        // Compute is here because both occlusion passes bind this same set
        // for the projection and its inverse rather than duplicating them.
        _gpuSceneDataDescriptorLayout = builder.build(
            _device,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                VK_SHADER_STAGE_COMPUTE_BIT);
    }

    // A device that could not provide an occlusion format still has to bind
    // something at binding 2.  The shadow map is the one image guaranteed to
    // exist by this point; the flag in the scene block keeps it unread.
    const VkImageView occlusionView = ssao_occlusion_view();
    const VkSampler occlusionSampler = ssao_occlusion_sampler();

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
        sceneWriter.write_image(
            2,
            occlusionView,
            occlusionSampler,
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
            // Bound so the layout is satisfied, never read: a portal camera
            // sets the flag in its own scene block to zero.
            portalSceneWriter.write_image(
                2,
                occlusionView,
                occlusionSampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            portalSceneWriter.update_set(
                _device, frame.portalSceneDescriptors[view]);
        }
    }
}

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

void VulkanEngine::init_descriptor_cleanup()
{
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
        vkDestroyDescriptorSetLayout(_device, _prepassImageDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssaoDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssaoBlurDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssaoDebugDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssgiDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssgiDebugDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssgiFilterDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(
            _device, _ssgiCompositeDescriptorLayout, nullptr);
    });
}

void VulkanEngine::init_descriptors()
{
    init_descriptor_pools();
    init_background_descriptors();
    init_post_process_descriptors();
    init_prepass_descriptors();
    init_ssao_descriptors();
    init_ssgi_descriptors();
    init_scene_descriptors();
    init_descriptor_cleanup();
}

void VulkanEngine::init_pipelines()
{
    init_background_pipelines();
    init_shadow_pipeline();
    init_depth_normal_pipeline();
    // Deferred to the end of this function: both alpha-tested pipelines take
    // metalRoughMaterial.materialLayout, which build_pipelines() creates.
    init_render_debug_pipeline();
    init_tonemap_pipeline();
    init_fxaa_pipeline();
    init_ssao_pipelines();
    metalRoughMaterial.build_pipelines(this);
    init_shadow_mask_pipeline();
    init_depth_normal_mask_pipeline();
    init_ssgi_pipelines();
}

void VulkanEngine::draw_geometry(
    VkCommandBuffer cmd,
    const DrawContext& drawContext,
    const glm::mat4& viewProjection,
    VkDescriptorSet sceneDescriptor,
    bool clearDepthAndStencil,
    MaterialPipeline* overridePipeline,
    MaterialPipeline* overrideMaskPipeline,
    uint32_t stencilReference,
    bool useFrustumCulling,
    uint32_t stencilCompareMask,
    bool writeGBuffer)
{
    const auto startTime = std::chrono::steady_clock::now();

    std::array<VkRenderingAttachmentInfo, 4> colorAttachments{};
    colorAttachments[0] = vkinit::attachment_info(
        _drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkClearValue clear{};
    if (writeGBuffer) {
        colorAttachments[1] = vkinit::attachment_info(
            _gbufferAlbedoImage.imageView, &clear,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        colorAttachments[2] = vkinit::attachment_info(
            _gbufferVelocityImage.imageView, &clear,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        colorAttachments[3] = vkinit::attachment_info(
            _directLightingImage.imageView, &clear,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    depthAttachment.loadOp = clearDepthAndStencil
        ? VK_ATTACHMENT_LOAD_OP_CLEAR
        : VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderingAttachmentInfo stencilAttachment = depthAttachment;

    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, colorAttachments.data(), &depthAttachment);
    renderInfo.colorAttachmentCount = writeGBuffer ? 4u : 1u;
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

        // An overriding pass still has to honour the alpha cutoff, or masked
        // geometry reappears as a solid rectangle the moment it is seen
        // through a portal.  Passes that write a stencil or a mask rather
        // than shading pass no mask pipeline and fall back to the one
        // override, which is what they want: the cutout's silhouette is not
        // what those passes are describing.
        MaterialPipeline* pipeline = renderObject.material->pipeline;
        if (overridePipeline != nullptr) {
            pipeline = overrideMaskPipeline != nullptr &&
                    renderObject.material->passType == MaterialPass::Mask
                ? overrideMaskPipeline
                : overridePipeline;
        }
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
        set_previous_world_rows(pushConstants, renderObject.previousTransform);
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
    // Portal cameras and the first main-camera frame default to zero motion.
    // update_scene() replaces this with the previous main view-projection for
    // the ordinary camera.
    data.previousViewProjection = data.viewproj;
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
    data.shadowFilterSettings = glm::vec4(
        _shadowFilterRadius, 0.0f, 0.0f, 0.0f);
    // Screen-space passes get a depth buffer rather than a position per
    // fragment; these are what turn one back into the other.
    data.inverseProjection = glm::inverse(data.proj);
    data.inverseViewProjection = glm::inverse(data.viewproj);

    // Only the main camera gets ambient occlusion.  build_portal_scene_data()
    // clears this again for every virtual camera, because the occlusion image
    // describes what is in front of the player, not what is through a portal.
    data.screenSpaceSettings = glm::vec4(
        ssao_active() ? 1.0f : 0.0f,
        _ssaoAmbientOnly ? 1.0f : 0.0f,
        _ssgiEnabled ? 1.0f : 0.0f,
        _ssgiHalfResolution ? 1.0f : 0.0f);
    // One multiply takes a fragment coordinate to its occlusion texel.  It
    // folds together the half resolution, the render scale, and the fact that
    // the image stays allocated at window size, none of which a fragment
    // shader can work out for itself.
    const VkExtent2D activeOcclusion = active_ssao_extent();
    const glm::vec2 occlusionAllocation(
        static_cast<float>(std::max(1u, _ssaoExtent.width)),
        static_cast<float>(std::max(1u, _ssaoExtent.height)));
    const glm::vec2 occlusionFraction(
        static_cast<float>(activeOcclusion.width) / occlusionAllocation.x,
        static_cast<float>(activeOcclusion.height) / occlusionAllocation.y);
    data.ambientOcclusionUV = glm::vec4(
        occlusionFraction.x / static_cast<float>(_drawExtent.width),
        occlusionFraction.y / static_cast<float>(_drawExtent.height),
        occlusionFraction.x - 0.5f / occlusionAllocation.x,
        occlusionFraction.y - 0.5f / occlusionAllocation.y);
    data.ssgiFallbackSettings = _traceLighting.environment;
    // Ambient and SSGI are two answers to one question, so how they divide it
    // travels with every camera rather than being decided in the shader.
    data.indirectSettings = glm::vec4(
        _ssgiAmbientRetention,
        _ssgiTraceEnvironmentMap ? 1.0f : 0.0f,
        _skyboxEnvironmentLod,
        _skyboxIndirectClamp);
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
    _shadowFormatName = string_VkFormat(shadowFormat);

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

void VulkanEngine::init_shadow_mask_pipeline()
{
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module(
            "../../shaders/shadow_depth_mask.vert.spv", _device, &vertexShader) ||
        !vkutil::load_shader_module(
            "../../shaders/shadow_depth_mask.frag.spv", _device, &fragmentShader)) {
        fmt::print("Error loading alpha-tested shadow shaders\n");
        if (vertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, vertexShader, nullptr);
        }
        if (fragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, fragmentShader, nullptr);
        }
        return;
    }

    // One descriptor set, and it is the per-material set the forward pass
    // binds at index 1.  Nothing about the sun's camera is needed here - it
    // is still folded into the push constant - so the scene set the other
    // passes carry would be dead weight, and set 0 is the material instead.
    VkPushConstantRange pushRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(ShadowPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &metalRoughMaterial.materialLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &layoutInfo, nullptr, &_shadowMaskPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _shadowMaskPipeline.layout;
    builder.set_shaders(vertexShader, fragmentShader);
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    // Every state below matches the opaque shadow pipeline exactly.  Where a
    // masked surface and an opaque one meet, any disagreement here would show
    // up as a seam between the two shadows they cast.
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    builder.disable_blending();
    builder.disable_color_attachment();
    builder.enable_depthtest(true, VK_COMPARE_OP_LESS_OR_EQUAL);
    builder.enable_depth_bias(1.25f, 2.75f);
    builder.set_depth_format(_shadowMapImage.imageFormat);
    _shadowMaskPipeline.pipeline = builder.build_pipeline(_device);

    vkDestroyShaderModule(_device, fragmentShader, nullptr);
    vkDestroyShaderModule(_device, vertexShader, nullptr);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _shadowMaskPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _shadowMaskPipeline.layout, nullptr);
    });
}

void VulkanEngine::draw_shadow_map(VkCommandBuffer cmd)
{
    const auto startTime = std::chrono::steady_clock::now();

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

        VkPipeline lastPipeline = _shadowPipeline.pipeline;
        MaterialInstance* lastMaterial = nullptr;
        VkBuffer lastIndexBuffer = VK_NULL_HANDLE;
        // portalViewDrawContext is the world plus the player's body, which is
        // what actually exists in the level.  The main camera's context is
        // not usable here: it also carries the portal quads, and it omits the
        // body the first-person camera sits inside.
        // Transparent surfaces intentionally do not cast yet: a blended
        // surface has no single silhouette to cast.  Masked ones do, and they
        // go through _shadowMaskPipeline so foliage and grates cast their
        // cutout rather than the rectangle it is painted on.
        for (const RenderObject& renderObject :
                portalViewDrawContext.OpaqueSurfaces) {
            // Cull against the light, never against the player's camera: an
            // object behind the camera can still drop a shadow into view.
            if (!is_visible(renderObject, _sunViewProjection)) {
                continue;
            }

            const bool masked = renderObject.material != nullptr &&
                renderObject.material->passType == MaterialPass::Mask &&
                _shadowMaskPipeline.pipeline != VK_NULL_HANDLE;
            const MaterialPipeline& active = masked
                ? _shadowMaskPipeline
                : _shadowPipeline;
            if (active.pipeline != lastPipeline) {
                lastPipeline = active.pipeline;
                vkCmdBindPipeline(
                    cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, active.pipeline);
                // The opaque pipeline's layout declares no sets at all, so
                // the two are compatible for nothing: whatever was bound for
                // the mask pipeline does not survive a round trip through it.
                lastMaterial = nullptr;
            }
            if (masked && renderObject.material != lastMaterial) {
                lastMaterial = renderObject.material;
                // Set 0 here, not 1.  This pipeline carries the material set
                // alone, because the sun's camera is already folded into the
                // push constant and nothing else about the scene is read.
                vkCmdBindDescriptorSets(
                    cmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    active.layout,
                    0,
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

            ShadowPushConstants pushConstants{};
            pushConstants.lightMatrix = _sunViewProjection * renderObject.transform;
            pushConstants.vertexBuffer = renderObject.vertexBufferAddress;
            vkCmdPushConstants(
                cmd,
                active.layout,
                VK_SHADER_STAGE_VERTEX_BIT,
                0,
                sizeof(ShadowPushConstants),
                &pushConstants);
            vkCmdDrawIndexed(
                cmd, renderObject.indexCount, 1, renderObject.firstIndex, 0, 0);

            ++stats.shadow_drawcall_count;
            stats.shadow_triangle_count +=
                static_cast<int>(renderObject.indexCount / 3);
        }
    }

    vkCmdEndRendering(cmd);

    vkutil::transition_image(
        cmd,
        _shadowMapImage.image,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    stats.shadow_record_time = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - startTime).count();
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

void VulkanEngine::init_render_debug_pipeline()
{
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module(
            "../../shaders/render_debug.vert.spv", _device, &vertexShader) ||
        !vkutil::load_shader_module(
            "../../shaders/render_debug.frag.spv", _device, &fragmentShader)) {
        fmt::print("Error loading render debug shaders\n");
        if (vertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, vertexShader, nullptr);
        }
        if (fragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, fragmentShader, nullptr);
        }
        return;
    }

    VkDescriptorSetLayout layouts[] = {
        _gpuSceneDataDescriptorLayout,
        _prepassImageDescriptorLayout,
        _ssaoDebugDescriptorLayout,
        _ssgiDebugDescriptorLayout};
    VkPushConstantRange settingsRange{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(RenderDebugPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
    layoutInfo.setLayoutCount = 4;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &settingsRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &layoutInfo, nullptr, &_renderDebugPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _renderDebugPipeline.layout;
    builder.set_shaders(vertexShader, fragmentShader);
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    builder.disable_blending();
    // It replaces the finished image outright, so it neither tests nor writes
    // the depth buffer the portal passes still depend on.
    builder.disable_depthtest();
    builder.set_color_attachment_format(_drawImage.imageFormat);
    builder.set_depth_format(VK_FORMAT_UNDEFINED);
    _renderDebugPipeline.pipeline = builder.build_pipeline(_device);

    vkDestroyShaderModule(_device, fragmentShader, nullptr);
    vkDestroyShaderModule(_device, vertexShader, nullptr);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _renderDebugPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _renderDebugPipeline.layout, nullptr);
    });
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
    _postProcessImage = create_image(
        _drawImage.imageExtent,
        _swapchainImageFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    // The tonemap's own target.  It is separate from _postProcessImage for
    // the same reason that one is separate from _drawImage: anti-aliasing
    // reads this and writes that, and no pass may do both to one image.
    _tonemapImage = create_image(
        _drawImage.imageExtent,
        _swapchainImageFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    // Linear, unlike the prepass sampler: the whole point of the final tap is
    // to land between two texels and let the hardware mix them.  Clamped
    // because a filter kernel always reaches past the image at its border.
    VkSamplerCreateInfo samplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(_device, &samplerInfo, nullptr, &_postProcessSampler));

    _mainDeletionQueue.push_function([this]() {
        vkDestroySampler(_device, _postProcessSampler, nullptr);
        destroy_image(_tonemapImage);
        destroy_image(_postProcessImage);
    });
}

void VulkanEngine::init_tonemap_pipeline()
{
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    // The same fullscreen triangle the anti-aliasing pass uses.  It binds
    // nothing and interpolates nothing, so there is no reason for a second
    // copy of it.
    if (!vkutil::load_shader_module(
            "../../shaders/fxaa.vert.spv", _device, &vertexShader) ||
        !vkutil::load_shader_module(
            "../../shaders/tonemap.frag.spv", _device, &fragmentShader)) {
        fmt::print("Error loading tonemap shaders\n");
        if (vertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, vertexShader, nullptr);
        }
        if (fragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, fragmentShader, nullptr);
        }
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
        _device, &layoutInfo, nullptr, &_tonemapPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _tonemapPipeline.layout;
    builder.set_shaders(vertexShader, fragmentShader);
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    builder.disable_blending();
    builder.disable_depthtest();
    builder.set_color_attachment_format(_tonemapImage.imageFormat);
    builder.set_depth_format(VK_FORMAT_UNDEFINED);
    _tonemapPipeline.pipeline = builder.build_pipeline(_device);

    vkDestroyShaderModule(_device, fragmentShader, nullptr);
    vkDestroyShaderModule(_device, vertexShader, nullptr);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _tonemapPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _tonemapPipeline.layout, nullptr);
    });
}

void VulkanEngine::init_fxaa_pipeline()
{
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module(
            "../../shaders/fxaa.vert.spv", _device, &vertexShader) ||
        !vkutil::load_shader_module(
            "../../shaders/fxaa.frag.spv", _device, &fragmentShader)) {
        fmt::print("Error loading FXAA shaders\n");
        if (vertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, vertexShader, nullptr);
        }
        if (fragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, fragmentShader, nullptr);
        }
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
        _device, &layoutInfo, nullptr, &_fxaaPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _fxaaPipeline.layout;
    builder.set_shaders(vertexShader, fragmentShader);
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    // It writes every pixel of its target from scratch, so there is nothing
    // to blend against and no depth to test.
    builder.disable_blending();
    builder.disable_depthtest();
    builder.set_color_attachment_format(_postProcessImage.imageFormat);
    builder.set_depth_format(VK_FORMAT_UNDEFINED);
    _fxaaPipeline.pipeline = builder.build_pipeline(_device);

    vkDestroyShaderModule(_device, fragmentShader, nullptr);
    vkDestroyShaderModule(_device, vertexShader, nullptr);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _fxaaPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _fxaaPipeline.layout, nullptr);
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

void VulkanEngine::draw_render_debug(VkCommandBuffer cmd)
{
    if (_renderDebugPipeline.pipeline == VK_NULL_HANDLE) {
        return;
    }

    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, nullptr);
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
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0x00);

    vkCmdBindPipeline(
        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _renderDebugPipeline.pipeline);
    const std::array<VkDescriptorSet, 4> sets{
        get_current_frame().sceneDescriptor,
        _prepassImageDescriptor,
        _ssaoDebugDescriptor,
        _ssgiDebugDescriptors[1u - _ssgiHistoryWriteIndex]};
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        _renderDebugPipeline.layout,
        0,
        static_cast<uint32_t>(sets.size()),
        sets.data(),
        0,
        nullptr);

    RenderDebugPushConstants pushConstants{};
    pushConstants.settings = glm::vec4(
        static_cast<float>(static_cast<int>(_renderDebugView)),
        static_cast<float>(_drawExtent.width),
        static_cast<float>(_drawExtent.height),
        _renderDebugDepthRange);
    vkCmdPushConstants(
        cmd,
        _renderDebugPipeline.layout,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(RenderDebugPushConstants),
        &pushConstants);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);
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
        _tonemapImage.image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _tonemapImage.imageView,
        nullptr,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, nullptr);
    vkCmdBeginRendering(cmd, &renderInfo);

    // _drawExtent rather than the allocation, for the same reason the
    // anti-aliasing pass uses it: below a resolution scale of 1 the rest of
    // the image holds nothing this frame produced.
    VkViewport viewport{};
    viewport.width = static_cast<float>(_drawExtent.width);
    viewport.height = static_cast<float>(_drawExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = _drawExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    // No stencil attachment here either, but the shared builder makes stencil
    // state dynamic on every pipeline it produces.
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0x00);

    vkCmdBindPipeline(
        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _tonemapPipeline.pipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        _tonemapPipeline.layout,
        0,
        1,
        &_tonemapInputDescriptor,
        0,
        nullptr);

    TonemapPushConstants pushConstants{};
    pushConstants.settings = glm::vec4(
        _tonemapExposure,
        _tonemapOperator == 1 ? 1.0f : 0.0f,
        _tonemapBypassCurve ? 1.0f : 0.0f,
        0.0f);
    vkCmdPushConstants(
        cmd,
        _tonemapPipeline.layout,
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
        _tonemapImage.image,
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
        _postProcessImage.image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _postProcessImage.imageView,
        nullptr,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, nullptr);
    vkCmdBeginRendering(cmd, &renderInfo);

    // _drawExtent, not the allocation: at a resolution scale below 1 the rest
    // of the image holds nothing this frame rendered, and the copy to the
    // swapchain reads only this region anyway.
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

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _fxaaPipeline.pipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        _fxaaPipeline.layout,
        0,
        1,
        &_fxaaInputDescriptor,
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
        texelSize.x, texelSize.y, _fxaaEdgeThreshold, _fxaaSubpixelStrength);
    pushConstants.limits = glm::vec4(
        renderedLimit.x, renderedLimit.y, _fxaaShowEdges ? 1.0f : 0.0f, 0.0f);
    vkCmdPushConstants(
        cmd,
        _fxaaPipeline.layout,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(FXAAPushConstants),
        &pushConstants);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);

    vkutil::transition_image(
        cmd,
        _postProcessImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}

void VulkanEngine::init_gpu_timestamps()
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(_chosenGPU, &properties);
    // A period of zero means the device records no timestamps at all, and the
    // queue this engine submits to has to be able to take them, which is what
    // timestampComputeAndGraphics reports.
    if (properties.limits.timestampPeriod <= 0.0f ||
        properties.limits.timestampComputeAndGraphics == VK_FALSE) {
        fmt::print("GPU timestamps unavailable; occlusion timings will report "
                   "CPU recording time instead\n");
        return;
    }
    _timestampPeriod = properties.limits.timestampPeriod;

    VkQueryPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    poolInfo.queryCount = FRAME_OVERLAP * TimestampsPerFrame;
    VK_CHECK(vkCreateQueryPool(_device, &poolInfo, nullptr, &_timestampPool));
    poolInfo.queryCount = FRAME_OVERLAP * SSGITimestampsPerFrame;
    VK_CHECK(vkCreateQueryPool(
        _device, &poolInfo, nullptr, &_ssgiTimestampPool));
    _gpuTimingSupported = true;

    _mainDeletionQueue.push_function([this]() {
        vkDestroyQueryPool(_device, _timestampPool, nullptr);
        vkDestroyQueryPool(_device, _ssgiTimestampPool, nullptr);
    });
}

void VulkanEngine::read_gpu_timestamps(uint32_t frameIndex)
{
    // Called only after this frame slot's fence has been waited on, so the
    // results being read belong to a submission that has certainly finished.
    if (!_gpuTimingSupported) {
        return;
    }
    if (_timestampsPending[frameIndex]) {
        _timestampsPending[frameIndex] = false;

    std::array<uint64_t, TimestampsPerFrame> ticks{};
    const VkResult result = vkGetQueryPoolResults(
        _device,
        _timestampPool,
        frameIndex * TimestampsPerFrame,
        TimestampsPerFrame,
        sizeof(ticks),
        ticks.data(),
        sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS) {
        return;
    }

    // timestampPeriod is nanoseconds per tick; the panel reports milliseconds
    // like every other timing beside it.
    const auto toMilliseconds = [this](uint64_t from, uint64_t to) {
        return static_cast<float>(to - from) * _timestampPeriod * 1e-6f;
    };
    stats.ssao_raw_time = toMilliseconds(ticks[0], ticks[1]);
    stats.ssao_blur_horizontal_time = toMilliseconds(ticks[1], ticks[2]);
    stats.ssao_blur_vertical_time = toMilliseconds(ticks[2], ticks[3]);
    stats.ssao_total_time = toMilliseconds(ticks[0], ticks[3]);
    stats.ssao_time_is_gpu = true;
    }

    if (_ssgiTimingWritten[frameIndex]) {
        _ssgiTimingWritten[frameIndex] = false;
        std::array<uint64_t, SSGITimestampsPerFrame> ssgiTicks{};
        const VkResult ssgiResult = vkGetQueryPoolResults(
            _device, _ssgiTimestampPool,
            frameIndex * SSGITimestampsPerFrame,
            SSGITimestampsPerFrame, sizeof(ssgiTicks), ssgiTicks.data(),
            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (ssgiResult == VK_SUCCESS) {
            const auto milliseconds = [&](uint64_t from, uint64_t to) {
                return static_cast<float>(to - from) *
                    _timestampPeriod * 1e-6f;
            };
            stats.ssgi_raw_time = milliseconds(ssgiTicks[0], ssgiTicks[1]);
            stats.ssgi_temporal_time = milliseconds(ssgiTicks[1], ssgiTicks[2]);
            stats.ssgi_filter_time = milliseconds(ssgiTicks[2], ssgiTicks[3]);
            stats.ssgi_composite_time = milliseconds(ssgiTicks[3], ssgiTicks[4]);
            stats.ssgi_total_time = milliseconds(ssgiTicks[0], ssgiTicks[4]);
            stats.ssgi_time_is_gpu = true;
        }
    }
}
