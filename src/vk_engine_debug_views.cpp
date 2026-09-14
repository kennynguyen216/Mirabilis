#include "vk_engine.h"
#include "vk_engine_render_helpers.h"

#include <glm/gtc/matrix_transform.hpp>

#include <vk_initializers.h>
#include <vk_pipelines.h>

void VulkanEngine::init_render_debug_pipeline()
{
    ScopedShaderModule vertexShader(_device);
    ScopedShaderModule fragmentShader(_device);
    if (!vertexShader.load("../../shaders/render_debug.vert.spv") ||
        !fragmentShader.load("../../shaders/render_debug.frag.spv")) {
        fmt::print("Error loading render debug shaders\n");
        return;
    }

    VkDescriptorSetLayout layouts[] = {
        _gpuSceneDataDescriptorLayout,
        _prepass.imageDescriptorLayout,
        _ssao.debugDescriptorLayout,
        _ssgi.debugDescriptorLayout};
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
    builder.set_shaders(vertexShader.get(), fragmentShader.get());
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

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _renderDebugPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _renderDebugPipeline.layout, nullptr);
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

    if (_shadow.showBounds) {
        // Convert the unit debug cube into the exact orthographic volume used
        // by the shadow map: clip x/y are [-1, 1], while Vulkan depth is [0, 1].
        const glm::mat4 clipBox = glm::translate(
            glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.5f)) * glm::scale(
            glm::mat4(1.0f), glm::vec3(2.0f, 2.0f, 1.0f));
        ColliderDebugPushConstants pushConstants{};
        pushConstants.viewProjection = sceneData.viewproj;
        pushConstants.model = glm::inverse(_shadow.sunViewProjection) * clipBox;
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

    set_fullscreen_dynamic_state(cmd, _drawExtent);

    vkCmdBindPipeline(
        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _renderDebugPipeline.pipeline);
    const std::array<VkDescriptorSet, 4> sets{
        get_current_frame().sceneDescriptor,
        _prepass.imageDescriptor,
        _ssao.debugDescriptor,
        _ssgi.debugDescriptors[1u - _ssgi.historyWriteIndex]};
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

