#include "vk_engine.h"
#include "vk_engine_render_helpers.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_pipelines.h>

void VulkanEngine::init_shadow_resources()
{
    // Comparison sampling is a format capability, not a guarantee of every
    // depth format. Prefer D32 with bilinear comparison filtering, then fall
    // back through other depth-only formats and nearest filtering if needed.
    constexpr VkFormatFeatureFlags2 requiredFeatures =
        VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT;
    const PickedFormat shadowPick = pick_format(
        _chosenGPU,
        {VK_FORMAT_D32_SFLOAT,
         VK_FORMAT_D16_UNORM,
         VK_FORMAT_X8_D24_UNORM_PACK32},
        requiredFeatures,
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    const VkFormat shadowFormat = shadowPick.format;
    const VkFilter shadowFilter =
        (shadowPick.features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
            ? VK_FILTER_LINEAR
            : VK_FILTER_NEAREST;
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

