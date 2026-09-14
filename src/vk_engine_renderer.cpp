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
    return _ssao.finalImage.imageView != VK_NULL_HANDLE
        ? _ssao.finalImage.imageView
        : _shadow.mapImage.imageView;
}

VkSampler VulkanEngine::ssao_occlusion_sampler() const
{
    return _ssao.sampler != VK_NULL_HANDLE ? _ssao.sampler : _prepass.sampler;
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
            _shadow.mapImage.imageView,
            _shadow.sampler,
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
                _shadow.mapImage.imageView,
                _shadow.sampler,
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
        vkDestroyDescriptorSetLayout(_device, _prepass.imageDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssao.descriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssao.blurDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssao.debugDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssgi.descriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssgi.debugDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssgi.filterDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(
            _device, _ssgi.compositeDescriptorLayout, nullptr);
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
        normalized_sun_direction(_shadow.sunlightDirection), 1.0f);
    data.sunlightColor = glm::vec4(1.0f);
    // update_scene() settles this before any camera's buffer is filled, so
    // the portal views inherit exactly the main camera's shadow map.
    data.sunViewProjection = _shadow.sunViewProjection;
    data.shadowSettings = glm::vec4(
        _shadow.depthBias,
        _shadow.normalBias,
        1.0f / static_cast<float>(ShadowMapResolution),
        _shadow.enabled ? 1.0f : 0.0f);
    data.shadowFilterSettings = glm::vec4(
        _shadow.filterRadius, 0.0f, 0.0f, 0.0f);
    // Screen-space passes get a depth buffer rather than a position per
    // fragment; these are what turn one back into the other.
    data.inverseProjection = glm::inverse(data.proj);
    data.inverseViewProjection = glm::inverse(data.viewproj);

    // Only the main camera gets ambient occlusion.  build_portal_scene_data()
    // clears this again for every virtual camera, because the occlusion image
    // describes what is in front of the player, not what is through a portal.
    data.screenSpaceSettings = glm::vec4(
        ssao_active() ? 1.0f : 0.0f,
        _ssao.ambientOnly ? 1.0f : 0.0f,
        _ssgi.enabled ? 1.0f : 0.0f,
        _ssgi.halfResolution ? 1.0f : 0.0f);
    // One multiply takes a fragment coordinate to its occlusion texel.  It
    // folds together the half resolution, the render scale, and the fact that
    // the image stays allocated at window size, none of which a fragment
    // shader can work out for itself.
    const VkExtent2D activeOcclusion = active_ssao_extent();
    const glm::vec2 occlusionAllocation(
        static_cast<float>(std::max(1u, _ssao.extent.width)),
        static_cast<float>(std::max(1u, _ssao.extent.height)));
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
        _ssgi.ambientRetention,
        _ssgi.traceEnvironmentMap ? 1.0f : 0.0f,
        _skyboxEnvironmentLod,
        _skyboxIndirectClamp);
    return data;
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

    ScopedShaderModule gradientShader(_device);
	if (!gradientShader.load("../../shaders/gradient_color.comp.spv"))
	{
		fmt::print("Error loading gradient_color compute shader\n");
		return;
	}

    ScopedShaderModule skyShader(_device);
	if (!skyShader.load("../../shaders/sky.comp.spv"))
	{
		fmt::print("Error loading sky compute shader\n");
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

	_mainDeletionQueue.push_function([this, gradientPipeline = gradient.pipeline, skyPipeline = sky.pipeline]() {
		vkDestroyPipeline(_device, gradientPipeline, nullptr);
		vkDestroyPipeline(_device, skyPipeline, nullptr);
		vkDestroyPipelineLayout(_device, _gradientPipelineLayout, nullptr);
		});
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
        _device, &poolInfo, nullptr, &_ssgi.timestampPool));
    _gpuTimingSupported = true;

    _mainDeletionQueue.push_function([this]() {
        vkDestroyQueryPool(_device, _timestampPool, nullptr);
        vkDestroyQueryPool(_device, _ssgi.timestampPool, nullptr);
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

    if (_ssgi.timingWritten[frameIndex]) {
        _ssgi.timingWritten[frameIndex] = false;
        std::array<uint64_t, SSGITimestampsPerFrame> ssgiTicks{};
        const VkResult ssgiResult = vkGetQueryPoolResults(
            _device, _ssgi.timestampPool,
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
