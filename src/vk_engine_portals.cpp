#include "vk_engine.h"

#include <algorithm>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>

#include <vk_images.h>
#include <vk_initializers.h>

RenderObject VulkanEngine::make_portal_render_object(
    const Portal& portal,
    MaterialInstance& material) const
{
    const glm::vec3 right = glm::normalize(glm::cross(portal.up, portal.normal));
    glm::mat4 transform(1.0f);
    transform[0] = glm::vec4(right * (portal.halfWidth * 2.0f), 0.0f);
    transform[1] = glm::vec4(portal.up * (portal.halfHeight * 2.0f), 0.0f);
    transform[2] = glm::vec4(portal.normal, 0.0f);
    transform[3] = glm::vec4(portal.position, 1.0f);

    return RenderObject{
        .indexCount = 6,
        .firstIndex = 0,
        .indexBuffer = _portalMesh.indexBuffer.buffer,
        .material = &material,
        .bounds = _portalBounds,
        .transform = transform,
        .vertexBufferAddress = _portalMesh.vertexBufferAddress,
    };
}

void VulkanEngine::draw_portal_masks(VkCommandBuffer cmd)
{
    if (!_bluePortal.placed || !_orangePortal.placed) {
        return;
    }

    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
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

    const VkDescriptorSet sceneDescriptor = get_current_frame().sceneDescriptor;
    const auto drawMask = [&](MaterialPipeline& pipeline,
                              const Portal& portal,
                              MaterialInstance& material,
                              uint32_t stencilReference,
                              uint32_t stencilWriteMask) {
        const RenderObject object = make_portal_render_object(portal, material);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout, 0, 1,
            &sceneDescriptor, 0, nullptr);
        vkCmdSetStencilReference(
            cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencilReference);
        vkCmdSetStencilCompareMask(
            cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
        vkCmdSetStencilWriteMask(
            cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencilWriteMask);
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout, 1, 1,
            &material.materialSet, 0, nullptr);
        vkCmdBindIndexBuffer(cmd, object.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        GPUDrawPushConstants pushConstants{};
        pushConstants.worldMatrix = object.transform;
        pushConstants.vertexBuffer = object.vertexBufferAddress;
        vkCmdPushConstants(
            cmd, pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
            sizeof(GPUDrawPushConstants), &pushConstants);
        vkCmdDrawIndexed(cmd, object.indexCount, 1, object.firstIndex, 0, 0);
    };

    // First mark only portal pixels that passed the main scene's depth test.
    // This prevents a portal hidden behind a nearer panel from drawing over it.
    drawMask(
        metalRoughMaterial.portalStencilPipeline,
        _bluePortal,
        _bluePortalMaterial,
        BluePortalView + 1,
        0xff);
    drawMask(
        metalRoughMaterial.portalStencilPipeline,
        _orangePortal,
        _orangePortalMaterial,
        OrangePortalView + 1,
        0xff);

    // Then set far depth only inside each already-visible stencil silhouette,
    // opening room for its virtual scene without touching foreground depth.
    drawMask(
        metalRoughMaterial.portalMaskPipeline,
        _bluePortal,
        _bluePortalMaterial,
        BluePortalView + 1,
        0x00);
    drawMask(
        metalRoughMaterial.portalMaskPipeline,
        _orangePortal,
        _orangePortalMaterial,
        OrangePortalView + 1,
        0x00);
    vkCmdEndRendering(cmd);
}

void VulkanEngine::draw_recursive_portal_mask(
    VkCommandBuffer cmd,
    const Portal& portal,
    MaterialInstance& material,
    VkDescriptorSet sceneDescriptor,
    uint32_t parentStencilReference,
    uint32_t recursiveStencilReference,
    uint32_t recursiveStencilBit)
{
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
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

    const RenderObject object = make_portal_render_object(portal, material);
    const auto drawMask = [&](MaterialPipeline& pipeline,
                              uint32_t compareMask,
                              uint32_t writeMask) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout, 0, 1,
            &sceneDescriptor, 0, nullptr);
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout, 1, 1,
            &material.materialSet, 0, nullptr);
        vkCmdSetStencilReference(
            cmd, VK_STENCIL_FACE_FRONT_AND_BACK, recursiveStencilReference);
        vkCmdSetStencilCompareMask(
            cmd, VK_STENCIL_FACE_FRONT_AND_BACK, compareMask);
        vkCmdSetStencilWriteMask(
            cmd, VK_STENCIL_FACE_FRONT_AND_BACK, writeMask);
        vkCmdBindIndexBuffer(cmd, object.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        GPUDrawPushConstants pushConstants{};
        pushConstants.worldMatrix = object.transform;
        pushConstants.vertexBuffer = object.vertexBufferAddress;
        vkCmdPushConstants(
            cmd, pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
            sizeof(GPUDrawPushConstants), &pushConstants);
        vkCmdDrawIndexed(cmd, object.indexCount, 1, object.firstIndex, 0, 0);
    };

    // The recursive mask can only be written inside its primary portal.
    drawMask(
        metalRoughMaterial.portalRecursiveStencilPipeline,
        parentStencilReference,
        recursiveStencilBit);
    // Clear depth only where both the parent and recursive bits are set.
    drawMask(
        metalRoughMaterial.portalMaskPipeline,
        recursiveStencilReference,
        0x00);
    vkCmdEndRendering(cmd);
}

void VulkanEngine::draw_portal_views(VkCommandBuffer cmd)
{
    if (!_bluePortal.placed || !_orangePortal.placed) {
        return;
    }

    FrameData& frame = get_current_frame();
    constexpr uint32_t BluePrimaryStencil = 0x01;
    constexpr uint32_t OrangePrimaryStencil = 0x02;
    constexpr uint32_t BlueRecursiveBit = 0x04;
    constexpr uint32_t OrangeRecursiveBit = 0x08;
    constexpr uint32_t BlueRecursiveStencil = BluePrimaryStencil | BlueRecursiveBit;
    constexpr uint32_t OrangeRecursiveStencil = OrangePrimaryStencil | OrangeRecursiveBit;

    draw_portal_sky(cmd, _portalSceneData[BluePortalView], BluePrimaryStencil);
    draw_geometry(
        cmd,
        portalViewDrawContext,
        _portalSceneData[BluePortalView].viewproj,
        frame.portalSceneDescriptors[BluePortalView],
        false,
        &metalRoughMaterial.portalViewPipeline,
        BluePrimaryStencil,
        false);
    if (_portalRecursionEnabled) {
        draw_recursive_portal_mask(
            cmd,
            _bluePortal,
            _bluePortalMaterial,
            frame.portalSceneDescriptors[BluePortalView],
            BluePrimaryStencil,
            BlueRecursiveStencil,
            BlueRecursiveBit);
        draw_portal_sky(
            cmd,
            _portalSceneData[BluePortalRecursiveView],
            BlueRecursiveStencil,
            BlueRecursiveStencil);
        draw_geometry(
            cmd,
            portalViewDrawContext,
            _portalSceneData[BluePortalRecursiveView].viewproj,
            frame.portalSceneDescriptors[BluePortalRecursiveView],
            false,
            &metalRoughMaterial.portalViewPipeline,
            BlueRecursiveStencil,
            false,
            BlueRecursiveStencil);
    }

    draw_portal_sky(cmd, _portalSceneData[OrangePortalView], OrangePrimaryStencil);
    draw_geometry(
        cmd,
        portalViewDrawContext,
        _portalSceneData[OrangePortalView].viewproj,
        frame.portalSceneDescriptors[OrangePortalView],
        false,
        &metalRoughMaterial.portalViewPipeline,
        OrangePrimaryStencil,
        false);
    if (_portalRecursionEnabled) {
        draw_recursive_portal_mask(
            cmd,
            _orangePortal,
            _orangePortalMaterial,
            frame.portalSceneDescriptors[OrangePortalView],
            OrangePrimaryStencil,
            OrangeRecursiveStencil,
            OrangeRecursiveBit);
        draw_portal_sky(
            cmd,
            _portalSceneData[OrangePortalRecursiveView],
            OrangeRecursiveStencil,
            OrangeRecursiveStencil);
        draw_geometry(
            cmd,
            portalViewDrawContext,
            _portalSceneData[OrangePortalRecursiveView].viewproj,
            frame.portalSceneDescriptors[OrangePortalRecursiveView],
            false,
            &metalRoughMaterial.portalViewPipeline,
            OrangeRecursiveStencil,
            false,
            OrangeRecursiveStencil);
    }
}

void VulkanEngine::draw_geometry_to_portal_camera(
    VkCommandBuffer cmd,
    const DrawContext& drawContext,
    const glm::mat4& viewProjection,
    VkDescriptorSet sceneDescriptor,
    const AllocatedImage& colorTarget)
{
    const auto startTime = std::chrono::steady_clock::now();
    VkClearValue clearColor{};
    clearColor.color = {{0.025f, 0.045f, 0.085f, 1.0f}};
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        colorTarget.imageView, &clearColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _portalCameraDepthImage.imageView,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo stencilAttachment = depthAttachment;
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _portalCameraExtent, &colorAttachment, &depthAttachment);
    renderInfo.pStencilAttachment = &stencilAttachment;

    vkCmdBeginRendering(cmd, &renderInfo);
    VkViewport viewport{};
    viewport.width = static_cast<float>(_portalCameraExtent.width);
    viewport.height = static_cast<float>(_portalCameraExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = _portalCameraExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    MaterialInstance* lastMaterial = nullptr;
    VkBuffer lastIndexBuffer = VK_NULL_HANDLE;
    for (const RenderObject& renderObject : drawContext.OpaqueSurfaces) {
        if (renderObject.material == nullptr) {
            continue;
        }

        vkCmdBindPipeline(
            cmd,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            metalRoughMaterial.portalOffscreenPipeline.pipeline);
        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            metalRoughMaterial.portalOffscreenPipeline.layout,
            0,
            1,
            &sceneDescriptor,
            0,
            nullptr);
        if (renderObject.material != lastMaterial) {
            lastMaterial = renderObject.material;
            vkCmdBindDescriptorSets(
                cmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                metalRoughMaterial.portalOffscreenPipeline.layout,
                1,
                1,
                &renderObject.material->materialSet,
                0,
                nullptr);
        }
        if (renderObject.indexBuffer != lastIndexBuffer) {
            lastIndexBuffer = renderObject.indexBuffer;
            vkCmdBindIndexBuffer(cmd, renderObject.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        }
        GPUDrawPushConstants pushConstants{};
        pushConstants.worldMatrix = renderObject.transform;
        pushConstants.vertexBuffer = renderObject.vertexBufferAddress;
        vkCmdPushConstants(
            cmd,
            metalRoughMaterial.portalOffscreenPipeline.layout,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(GPUDrawPushConstants),
            &pushConstants);
        vkCmdDrawIndexed(
            cmd, renderObject.indexCount, 1, renderObject.firstIndex, 0, 0);
        ++stats.drawcall_count;
        stats.triangle_count += static_cast<int>(renderObject.indexCount / 3);
    }
    vkCmdEndRendering(cmd);
    stats.mesh_draw_time += std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - startTime).count();
}

void VulkanEngine::draw_offscreen_portal_views(VkCommandBuffer cmd)
{
    if (!_bluePortal.placed || !_orangePortal.placed) {
        return;
    }

    FrameData& frame = get_current_frame();
    const auto renderCamera = [&](uint32_t targetIndex, uint32_t viewIndex) {
        AllocatedImage& target = _portalCameraImages[targetIndex];
        // Every target is completely cleared before use, so the old contents
        // are irrelevant. This is valid from either first-use or shader-read.
        vkutil::transition_image(
            cmd,
            target.image,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        vkutil::transition_image(
            cmd,
            _portalCameraDepthImage.image,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        draw_geometry_to_portal_camera(
            cmd,
            portalViewDrawContext,
            _portalSceneData[viewIndex].viewproj,
            frame.portalSceneDescriptors[viewIndex],
            target);
        vkutil::transition_image(
            cmd,
            target.image,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    };

    // This experiment deliberately renders only one virtual view per portal.
    // The normal stencil mode below remains the recursive reference path.
    renderCamera(0, BluePortalView);
    renderCamera(1, OrangePortalView);

    DrawContext blueComposite{};
    blueComposite.OpaqueSurfaces.push_back(make_portal_render_object(
        _bluePortal, _portalCameraMaterials[0]));
    draw_geometry(
        cmd,
        blueComposite,
        sceneData.viewproj,
        frame.sceneDescriptor,
        false,
        &metalRoughMaterial.portalCompositePipeline,
        BluePortalView + 1,
        false);

    DrawContext orangeComposite{};
    orangeComposite.OpaqueSurfaces.push_back(make_portal_render_object(
        _orangePortal, _portalCameraMaterials[1]));
    draw_geometry(
        cmd,
        orangeComposite,
        sceneData.viewproj,
        frame.sceneDescriptor,
        false,
        &metalRoughMaterial.portalCompositePipeline,
        OrangePortalView + 1,
        false);
}

void VulkanEngine::draw_portal_sky(
    VkCommandBuffer cmd,
    const GPUSceneData& skyCamera,
    uint32_t stencilReference,
    uint32_t stencilCompareMask)
{
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
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
    vkCmdSetStencilReference(
        cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencilReference);
    vkCmdSetStencilCompareMask(
        cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencilCompareMask);
    vkCmdSetStencilWriteMask(
        cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0x00);
    vkCmdBindPipeline(
        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _portalSkyPipeline.pipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        _portalSkyPipeline.layout,
        0,
        1,
        &_skyboxDescriptor,
        0,
        nullptr);
    const ComputePushConstants& backgroundData =
        backgroundEffects[currentBackgroundEffect].data;
    PortalSkyPushConstants pushConstants{};
    pushConstants.data1 = backgroundData.data1;
    pushConstants.data2 = backgroundData.data2;
    pushConstants.settings = glm::vec4(
        static_cast<float>(currentBackgroundEffect),
        static_cast<float>(_drawExtent.width),
        static_cast<float>(_drawExtent.height),
        0.0f);
    // The inverse view matrix is the camera's world transform.  Its first
    // two columns are right/up; local -Z is the camera's forward direction.
    const glm::mat4 cameraWorld = glm::inverse(skyCamera.view);
    pushConstants.cameraRight = glm::vec4(glm::normalize(glm::vec3(cameraWorld[0])), 0.0f);
    pushConstants.cameraUp = glm::vec4(glm::normalize(glm::vec3(cameraWorld[1])), 0.0f);
    pushConstants.cameraForward = glm::vec4(
        glm::normalize(-glm::vec3(cameraWorld[2])), 0.0f);
    vkCmdPushConstants(
        cmd,
        _portalSkyPipeline.layout,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(PortalSkyPushConstants),
        &pushConstants);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

GPUSceneData VulkanEngine::build_portal_scene_data(
    const glm::mat4& view,
    const Portal& destination) const
{
    GPUSceneData data = build_scene_data(view);

    // Keep the room-facing side of the destination portal and clip the
    // outside/behind-wall side. A tiny offset avoids a precision fight with
    // the wall face. This is performed in the portal fragment shader rather
    // than by mutating the reversed-Z projection matrix.
    constexpr float ClipEpsilon = 0.01f;
    const glm::vec3 clipPoint = destination.position +
        destination.normal * ClipEpsilon;
    data.portalClipPlane = glm::vec4(
        destination.normal,
        -glm::dot(destination.normal, clipPoint));
    data.portalClipEnabled = glm::vec4(1.0f);
    return data;
}

void VulkanEngine::init_portal_camera_targets()
{
    const VkExtent3D targetExtent{
        _portalCameraExtent.width,
        _portalCameraExtent.height,
        1};
    for (AllocatedImage& image : _portalCameraImages) {
        image = create_image(
            targetExtent,
            _drawImage.imageFormat,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }
    _portalCameraDepthImage = create_image(
        targetExtent,
        _depthImage.imageFormat,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);

    for (uint32_t index = 0; index < PortalCameraTargetCount; ++index) {
        _portalCameraMaterialBuffers[index] = create_buffer(
            sizeof(GLTFMetallic_Roughness::MaterialConstants),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU);
        auto* constants = static_cast<GLTFMetallic_Roughness::MaterialConstants*>(
            _portalCameraMaterialBuffers[index].info.pMappedData);
        *constants = {};
        constants->colorFactors = glm::vec4(1.0f);

        GLTFMetallic_Roughness::MaterialResources resources{};
        resources.colorImage = _portalCameraImages[index];
        resources.colorSampler = _defaultSamplerLinear;
        resources.metalRoughImage = _whiteImage;
        resources.metalRoughSampler = _defaultSamplerLinear;
        resources.dataBuffer = _portalCameraMaterialBuffers[index].buffer;
        _portalCameraMaterials[index] = metalRoughMaterial.write_material(
            _device,
            MaterialPass::MainColor,
            resources,
            globalDescriptorAllocator);
        _portalCameraMaterials[index].pipeline =
            &metalRoughMaterial.portalCompositePipeline;
    }

    _mainDeletionQueue.push_function([this]() {
        for (const AllocatedBuffer& buffer : _portalCameraMaterialBuffers) {
            destroy_buffer(buffer);
        }
        for (const AllocatedImage& image : _portalCameraImages) {
            destroy_image(image);
        }
        destroy_image(_portalCameraDepthImage);
    });
}

