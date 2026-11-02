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

VkDeviceAddress VulkanEngine::upload_draw_instances(std::span<const GPUDrawInstance> instances)
{
    if(instances.empty())return 0;
    auto& frame=get_current_frame();const size_t page=frame.instancePageCursor++;
    if(page==frame.instancePages.size())frame.instancePages.emplace_back();
    auto& buffer=frame.instancePages[page];const size_t bytes=instances.size_bytes();
    if(buffer.buffer==VK_NULL_HANDLE || buffer.info.size<bytes) {
        if(buffer.buffer!=VK_NULL_HANDLE)destroy_buffer(buffer);
        buffer=create_buffer(std::max<size_t>(bytes,64*1024),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VMA_MEMORY_USAGE_CPU_TO_GPU);
    }
    std::memcpy(buffer.info.pMappedData,instances.data(),bytes);
    vmaFlushAllocation(_allocator,buffer.allocation,0,bytes);
    VkBufferDeviceAddressInfo info{.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};info.buffer=buffer.buffer;
    return vkGetBufferDeviceAddress(_device,&info);
}

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
        // A scene set holds five: shadow map, occlusion, panorama, and the two
        // image-based lighting images.
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 }
    };
    if (_rayQueryShadows) {
        sizes.push_back({ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 });
    }

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
        // Image-based lighting: the prefiltered environment chain and the
        // split-sum BRDF table, also written by update_skybox_descriptors().
        builder.add_binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        // Emitter light at the visible surface (R4.9, R4.12): the path
        // tracer's triangles, materials and emitter list (7-9), and the
        // surface cache's albedo, emissive, depth and emitter pages (6,
        // 10-12) with its lookup buffers (13-15).  Written every frame by
        // write_emitter_descriptors().
        builder.add_binding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        builder.add_binding(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        builder.add_binding(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        builder.add_binding(10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        builder.add_binding(14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        builder.add_binding(15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        builder.add_binding(22, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(23, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        for (uint32_t binding = 17; binding <= 21; ++binding)
            builder.add_binding(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        // IQ2: the sun casters' TLAS, only on the ray-query device, where
        // write_emitter_descriptors() binds it every frame.
        if (_rayQueryShadows) {
            builder.add_binding(16, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);
        }
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
            for(auto page:frame.instancePages)destroy_buffer(page);
            for(auto buffer:{frame.clusterInput,frame.clusterInstances,frame.clusterCommands})if(buffer.buffer)destroy_buffer(buffer);
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
    init_auto_exposure();
    init_fxaa_pipeline();
    init_ssao_pipelines();
    metalRoughMaterial.build_pipelines(this);
    init_shadow_mask_pipeline();
    init_depth_normal_mask_pipeline();
    init_ssgi_pipelines();
    init_mesh_visibility();
}

void VulkanEngine::copy_refraction_source(VkCommandBuffer cmd,const AllocatedImage& source,VkExtent2D extent)
{
    vkutil::transition_image(cmd,source.image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkutil::transition_image(cmd,_postProcess.sceneCopy.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};copy.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.dstSubresource=copy.srcSubresource;
    copy.extent={std::min(extent.width,_postProcess.sceneCopy.imageExtent.width),std::min(extent.height,_postProcess.sceneCopy.imageExtent.height),1};
    vkCmdCopyImage(cmd,source.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,_postProcess.sceneCopy.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    vkutil::transition_image(cmd,_postProcess.sceneCopy.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkutil::transition_image(cmd,source.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
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
    auto profileScope = _profiler.scope(mirabilis::CpuZone::Geometry);
    const auto startTime = std::chrono::steady_clock::now();

    std::array<VkRenderingAttachmentInfo, 4> colorAttachments{};
    colorAttachments[0] = vkinit::attachment_info(
        _drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkClearValue clear{};
    if (writeGBuffer) {
        colorAttachments[1] = vkinit::attachment_info(
            _sceneTargets.gbufferAlbedo.imageView, &clear,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        colorAttachments[2] = vkinit::attachment_info(
            _sceneTargets.gbufferVelocity.imageView, &clear,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        colorAttachments[3] = vkinit::attachment_info(
            _sceneTargets.directLighting.imageView, &clear,
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

    auto& opaqueDraws = _opaqueDrawScratch;
    opaqueDraws.clear();
    if (opaqueDraws.capacity() < drawContext.OpaqueSurfaces.size()) {
        opaqueDraws.reserve(drawContext.OpaqueSurfaces.size());
        ++stats.draw_index_buffer_grows;
    }
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

    VkPipeline lastPipeline=VK_NULL_HANDLE;
    MaterialInstance* lastMaterial = nullptr;
    VkBuffer lastIndexBuffer = VK_NULL_HANDLE;

    auto& instances=_instanceScratch;instances.clear();instances.reserve(opaqueDraws.size()+drawContext.TransparentSurfaces.size());
    for(auto index:opaqueDraws){const auto& object=drawContext.OpaqueSurfaces[index];instances.push_back(make_draw_instance(object.transform,object.previousTransform,object.material&&object.material->doubleSided));}
    for(const auto& object:drawContext.TransparentSurfaces)instances.push_back(make_draw_instance(object.transform,object.previousTransform,object.material&&object.material->doubleSided));
    const auto instanceAddress=upload_draw_instances(instances);
    auto draw = [&](const RenderObject& renderObject,uint32_t firstInstance,uint32_t instanceCount) {
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
        if(renderObject.material->passType==MaterialPass::Transparent && overridePipeline==&metalRoughMaterial.portalViewPipeline)
            pipeline=&metalRoughMaterial.portalTransparentPipeline;
        const VkPipeline selected=instanceCount>1 && pipeline->instancedPipeline ? pipeline->instancedPipeline : pipeline->pipeline;
        if (selected != lastPipeline) {
            lastPipeline = selected;
            vkCmdBindPipeline(
                cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, selected);
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
        pushConstants.alignmentPadding=instanceAddress+firstInstance*sizeof(GPUDrawInstance);
        vkCmdPushConstants(
            cmd,
            pipeline->layout,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(GPUDrawPushConstants),
            &pushConstants);
        const auto& ranges=get_current_frame().clusterRanges;
        const auto objectIndex=writeGBuffer&&&drawContext==&mainDrawContext?size_t(&renderObject-drawContext.OpaqueSurfaces.data()):SIZE_MAX;
        if(instanceCount==1 && objectIndex<ranges.size() && ranges[objectIndex].second) {
            const auto [first,count]=ranges[objectIndex];
            for(uint32_t offset=0;offset<count;) {
                const auto draws=std::min(count-offset,_meshVisibility.maxIndirectCount);
                vkCmdDrawIndexedIndirect(cmd,get_current_frame().clusterCommands.buffer,VkDeviceSize(first+offset)*sizeof(VkDrawIndexedIndirectCommand),draws,sizeof(VkDrawIndexedIndirectCommand));
                offset+=draws;
            }
        } else vkCmdDrawIndexed(cmd,renderObject.indexCount,instanceCount,renderObject.firstIndex,0,0);

        ++stats.drawcall_count;
        stats.triangle_count += static_cast<int>(renderObject.indexCount / 3)*instanceCount;
    };

    const bool allowInstancing=SDL_getenv("MIRABILIS_TEST_DISABLE_INSTANCING")==nullptr;
    for(uint32_t i=0;i<opaqueDraws.size();) {
        const auto& object=drawContext.OpaqueSurfaces[opaqueDraws[i]];
        uint32_t end=i+1;
        while(overridePipeline==nullptr && allowInstancing && end<opaqueDraws.size()) {
            const auto& next=drawContext.OpaqueSurfaces[opaqueDraws[end]];
            if(next.material!=object.material || next.indexBuffer!=object.indexBuffer || next.vertexBufferAddress!=object.vertexBufferAddress || next.firstIndex!=object.firstIndex || next.indexCount!=object.indexCount)break;
            ++end;
        }
        draw(object,i,end-i);i=end;
    }
    if(!writeGBuffer && !drawContext.TransparentSurfaces.empty()) {
        vkCmdEndRendering(cmd);copy_refraction_source(cmd,_drawImage,_drawExtent);
        depthAttachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;stencilAttachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
        vkCmdBeginRendering(cmd,&renderInfo);lastPipeline=VK_NULL_HANDLE;lastMaterial=nullptr;
    }
    std::vector<uint32_t> transparentOrder;
    if(!writeGBuffer){transparentOrder.resize(drawContext.TransparentSurfaces.size());for(uint32_t i=0;i<transparentOrder.size();++i)transparentOrder[i]=i;
        std::stable_sort(transparentOrder.begin(),transparentOrder.end(),[&](uint32_t a,uint32_t b){const auto& left=drawContext.TransparentSurfaces[a];const auto& right=drawContext.TransparentSurfaces[b];
            return (viewProjection*left.transform*glm::vec4(left.bounds.origin,1)).w>(viewProjection*right.transform*glm::vec4(right.bounds.origin,1)).w;});}
    for (uint32_t i:transparentOrder) {
        const auto& object=drawContext.TransparentSurfaces[i];
        draw(object,uint32_t(opaqueDraws.size())+i,1);
    }

    vkCmdEndRendering(cmd);

    stats.mesh_draw_time += std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - startTime).count();
}

GPUSceneData VulkanEngine::build_scene_data(const glm::mat4& view) const
{
    GPUSceneData data{};
    data.view = view;
    build_local_lights(data);
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
    // One sun for both renderers.  Raster diffuse is baseColor * cos * S and
    // the path tracer's is albedo / pi * cos * E, so S = E / pi, with E the
    // scene's referenceLighting.sunRadiance.
    constexpr float Pi = 3.14159265358979323846f;
    data.sunlightColor = glm::vec4(
        glm::vec3(_traceSettings.lighting.sunRadiance) / Pi, 1.0f);
    // update_scene() settles this before any camera's buffer is filled, so
    // the portal views inherit exactly the main camera's shadow map.
    data.sunViewProjection = _shadow.sunViewProjection;
    const float shadowTexelWorld = 2.0f * _shadow.radius / float(ShadowMapResolution);
    const bool authoredShadowControl = SDL_getenv("MIRABILIS_R4_AUTHORED_SHADOW_CONTROL") != nullptr;
    data.shadowSettings = glm::vec4(
        authoredShadowControl ? _shadow.depthBias
            : shadowTexelWorld / (2.0f * (_shadow.radius + _shadow.depthMargin)),
        authoredShadowControl ? _shadow.normalBias : shadowTexelWorld,
        1.0f / static_cast<float>(ShadowMapResolution),
        _shadow.enabled ? 1.0f : 0.0f);
    // z: IQ1 diagnostic bitmask for sun_shadow.glsl, unset in normal use.
    const char* shadowDiagnostic = SDL_getenv("MIRABILIS_IQ_SHADOW_DIAG");
    data.shadowFilterSettings = glm::vec4(
        _shadow.filterRadius, authoredShadowControl ? 1.0f : 0.0f,
        shadowDiagnostic ? float(std::atoi(shadowDiagnostic) & 7) : 0.0f, 0.0f);
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
    data.ssgiFallbackSettings = _traceSettings.lighting.environment;
    data.environmentTint = _traceSettings.lighting.environmentTint;
    data.environmentTint.w = _ssgi.colorSaturation;
    // x is unused since R4: SSGI owns diffuse environment outright.
    data.indirectSettings = glm::vec4(
        0.0f,
        _ssgi.traceEnvironmentMap ? 1.0f : 0.0f,
        _skyboxEnvironmentLod,
        _skyboxIndirectClamp);
    // The inverse view's translation.  For a portal's virtual view that is the
    // virtual camera, so build_portal_scene_data() inherits the right answer.
    // Done once per camera here rather than as a 4x4 inverse per fragment.
    data.cameraPosition = glm::vec4(glm::vec3(glm::inverse(view)[3]), 1.0f);
    data.materialSettings = glm::vec4(
        _materialShading.normalMaps ? 1.0f : 0.0f,
        _materialShading.metalRoughTextures ? 1.0f : 0.0f,
        _materialShading.specularAntiAliasing ? 1.0f : 0.0f,
        _materialShading.specular ? 1.0f : 0.0f);
    data.materialDebug.x = is_forward_material_debug_view(_debugViews.view)
        ? static_cast<float>(static_cast<int>(_debugViews.view))
        : 0.0f;
    // R4 forward owners kept in the final image (src/r4_contributions.h);
    // exact in a float, and all six unless an R4 capture removed some.
    data.materialDebug.y = static_cast<float>(_r4Contributions.forward);
    data.iblSettings = glm::vec4(
        _materialShading.imageBasedLighting ? 1.0f : 0.0f,
        static_cast<float>(IblPrefilterLevels - 1),
        0.0f,
        0.0f);
    for (size_t i = 0; i < _ibl.environmentSH.size(); ++i) {
        data.environmentSH[i] = _ibl.environmentSH[i];
    }
    // R4.12: cached emitter diffuse, and four unshadowed specular samples
    // per pixel per frame, rotated by the frame.
    data.emitterSettings = glm::vec4(
        static_cast<float>(sampled_emitter_count()),
        static_cast<float>(_frameNumber % 16777216u),
        4.0f,
        _r4Contributions.rayCoverage ? 1.0f : 0.0f);
    return data;
}

uint32_t VulkanEngine::sampled_emitter_count() const
{
    // The shadowed term comes from the surface cache, which only Lumen-lite
    // builds, and the emitters from the software trace scene.  Plain SSGI
    // therefore samples none, and its Cornell result stays blocked (R4.3).
    const bool available = _ssgi.enabled && _ssgi.lumenEnabled &&
        _surfaceCache.valid && _surfaceCache.lightingValid && _surfaceCache.lookupValid &&
        _traceSupported && _traceEmitterBuffer.buffer != VK_NULL_HANDLE;
    return available ? static_cast<uint32_t>(_traceEmitters.size()) : 0u;
}

void VulkanEngine::write_emitter_descriptors(FrameData& frame)
{
    if (_portalGiPlaceholder.image == VK_NULL_HANDLE) {
        uint32_t empty[2]{};
        _portalGiPlaceholder = create_image(empty, {1, 1, 2},
            VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
        _mainDeletionQueue.push_function([this]() { destroy_image(_portalGiPlaceholder); });
    }
    // Placeholders keep the set complete while emitter light is off; the
    // shader reads nothing through them then.
    if (_emitterBufferPlaceholder.buffer == VK_NULL_HANDLE) {
        _emitterBufferPlaceholder = create_buffer(sizeof(glm::vec4) * 16,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        _mainDeletionQueue.push_function([this]() {
            destroy_buffer(_emitterBufferPlaceholder);
        });
    }
    const bool cache = _surfaceCache.valid && _surfaceCache.lightingValid &&
        _surfaceCache.lookupValid && _surfaceCache.emitter.image != VK_NULL_HANDLE;
    const bool portalGi = cache && _ssgi.enabled && _ssgi.lumenEnabled &&
        _sceneSdf.fieldValid && _surfaceCache.albedoVolume.image != VK_NULL_HANDLE;
    const bool portalControl = SDL_getenv("MIRABILIS_TEST_FRAMES") &&
        SDL_getenv("MIRABILIS_R6_PORTAL_GI_CONTROL");
    const auto buffer = [&](const AllocatedBuffer& source, bool usable) {
        return usable && source.buffer != VK_NULL_HANDLE
            ? source.buffer : _emitterBufferPlaceholder.buffer;
    };
    const auto page = [&](const AllocatedImage& image) {
        return cache ? image.imageView : _whiteImage.imageView;
    };
    const auto write = [&](VkDescriptorSet set) {
        DescriptorWriter writer;
        writer.write_image(22,_localShadowAtlas.imageView,_shadow.sampler,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writer.write_image(23,_postProcess.sceneCopy.imageView,_postProcess.sampler,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        const std::array<std::pair<uint32_t, VkImageView>, 4> pages{{
            {6, page(_surfaceCache.albedo)}, {10, page(_surfaceCache.emissive)},
            {11, page(_surfaceCache.depth)}, {12, page(_surfaceCache.emitter)}}};
        for (const auto& [binding, view] : pages) {
            writer.write_image(binding, view, _prepass.sampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        }
        writer.write_buffer(7, buffer(_traceTriangleBuffer, _traceSupported), VK_WHOLE_SIZE, 0,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.write_buffer(8, buffer(_traceMaterialBuffer, _traceSupported), VK_WHOLE_SIZE, 0,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.write_buffer(9, buffer(_traceEmitterBuffer, _traceSupported), VK_WHOLE_SIZE, 0,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.write_buffer(13, buffer(_surfaceCache.cardBuffer, cache), VK_WHOLE_SIZE, 0,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.write_buffer(14, buffer(_surfaceCache.gridBuffer, cache), VK_WHOLE_SIZE, 0,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.write_buffer(15, buffer(_surfaceCache.indexBuffer, cache), VK_WHOLE_SIZE, 0,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        const std::array<VkImageView, 5> giPages{
            portalGi ? _sceneSdf.field.imageView : _portalGiPlaceholder.imageView,
            page(_surfaceCache.direct), page(_surfaceCache.indirect), page(_surfaceCache.sky),
            portalGi ? _surfaceCache.albedoVolume.imageView : _portalGiPlaceholder.imageView};
        for (uint32_t i = 0; i < giPages.size(); ++i) {
            writer.write_image(17 + i, giPages[i], i == 0 || i == 4 ? _sdf.sampler : _prepass.sampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        }
        if (_rayQueryShadows) {
            writer.write_acceleration_structure(16, _sunCasters.tlas);
        }
        writer.update_set(_device, set);
    };
    write(frame.sceneDescriptor);
    for (VkDescriptorSet portalSet : frame.portalSceneDescriptors) {
        write(portalSet);
    }

    // update_scene() counted the emitters before this frame's cache update,
    // which can rebuild the emitter list or invalidate the cache.  Count again
    // against the buffers just bound, so no camera indexes past them.
    const float emitterCount = static_cast<float>(sampled_emitter_count());
    sceneData.emitterSettings.x = emitterCount;
    std::memcpy(frame.sceneBuffer.info.pMappedData, &sceneData, sizeof(sceneData));
    for (size_t view = 0; view < frame.portalSceneBuffers.size(); ++view) {
        _portalRender.sceneData[view].portalIndirectSettings.y =
            _ssgi.enabled && _ssgi.lumenEnabled && !portalControl ? (portalGi ? 1.0f : 2.0f) : 0.0f;
        _portalRender.sceneData[view].portalIndirectSettings.z = _ssgi.intensity;
        _portalRender.sceneData[view].portalIndirectSettings.w = _surfaceCache.radiosityEnabled ? 1.0f : 0.0f;
        _portalRender.sceneData[view].emitterSettings.x = emitterCount;
        std::memcpy(frame.portalSceneBuffers[view].info.pMappedData,
            &_portalRender.sceneData[view], sizeof(GPUSceneData));
    }
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
    _gpuTiming.timestampPeriod = properties.limits.timestampPeriod;

    VkQueryPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    poolInfo.queryCount = FRAME_OVERLAP * TimestampsPerFrame;
    VK_CHECK(vkCreateQueryPool(_device, &poolInfo, nullptr, &_gpuTiming.timestampPool));
    poolInfo.queryCount = FRAME_OVERLAP * SSGITimestampsPerFrame;
    VK_CHECK(vkCreateQueryPool(
        _device, &poolInfo, nullptr, &_ssgi.timestampPool));
    _gpuTiming.supported = true;

    _mainDeletionQueue.push_function([this]() {
        vkDestroyQueryPool(_device, _gpuTiming.timestampPool, nullptr);
        vkDestroyQueryPool(_device, _ssgi.timestampPool, nullptr);
    });
}

void VulkanEngine::read_gpu_timestamps(uint32_t frameIndex)
{
    // Called only after this frame slot's fence has been waited on, so the
    // results being read belong to a submission that has certainly finished.
    if (!_gpuTiming.supported) {
        return;
    }
    if (_gpuTiming.timestampsPending[frameIndex]) {
        _gpuTiming.timestampsPending[frameIndex] = false;

    std::array<uint64_t, TimestampsPerFrame> ticks{};
    const VkResult result = vkGetQueryPoolResults(
        _device,
        _gpuTiming.timestampPool,
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
        return static_cast<float>(to - from) * _gpuTiming.timestampPeriod * 1e-6f;
    };
    stats.ssao_raw_time = toMilliseconds(ticks[0], ticks[1]);
    stats.ssao_blur_horizontal_time = toMilliseconds(ticks[1], ticks[2]);
    stats.ssao_blur_vertical_time = toMilliseconds(ticks[2], ticks[3]);
    stats.ssao_total_time = toMilliseconds(ticks[0], ticks[3]);
    stats.ssao_time_is_gpu = true;
    _profiler.counter(mirabilis::ProfileCounter::GpuAO, stats.ssao_total_time);
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
                    _gpuTiming.timestampPeriod * 1e-6f;
            };
            stats.ssgi_raw_time = milliseconds(ssgiTicks[0], ssgiTicks[1]);
            stats.ssgi_temporal_time = milliseconds(ssgiTicks[1], ssgiTicks[2]);
            stats.ssgi_filter_time = milliseconds(ssgiTicks[2], ssgiTicks[3]);
            stats.ssgi_composite_time = milliseconds(ssgiTicks[3], ssgiTicks[4]);
            stats.ssgi_total_time = milliseconds(ssgiTicks[0], ssgiTicks[4]);
            stats.ssgi_time_is_gpu = true;
            _profiler.counter(mirabilis::ProfileCounter::GpuGI, stats.ssgi_total_time);
        }
    }
}
