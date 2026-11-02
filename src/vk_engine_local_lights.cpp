#include "vk_engine.h"
#include "vk_engine_render_helpers.h"
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <vk_images.h>
#include <vk_initializers.h>

void VulkanEngine::build_local_lights(GPUSceneData &data) const {
    bool pointShadow = false, spotShadow = false;
    uint32_t count = 0;
    std::vector<std::pair<Light, glm::mat4>> lights;
    for (const auto ref : _scene.creation_order()) {
        const auto *visible = _scene.read<Visibility>(ref);
        if (!visible || !visible->visible || _scene.pending(ref))
            continue;
        const auto world = _scene.world_matrix(ref);
        if (const auto *light = _scene.read<Light>(ref); light && light->emits_light())
            lights.emplace_back(*light, world);
        if (const auto runtime = _sceneRenderRuntimes.find(ref.persistentId);
            runtime != _sceneRenderRuntimes.end() && runtime->second.model)
            for (const auto &imported : runtime->second.model->lights)
                if (imported.light.emits_light())
                    lights.emplace_back(imported.light, world * imported.node->worldTransform);
    }
    for (const auto &[value, world] : lights) {
        if (count == 16)
            break;
        const auto *light = &value;
        const glm::vec3 position(world[3]);
        glm::vec3 direction = -glm::vec3(world[2]);
        direction = glm::dot(direction, direction) > 1e-12f ? glm::normalize(direction)
                                                            : glm::vec3(0, 0, -1);
        const bool spot = light->type == LightType::Spot;
        data.localLightPosition[count] = glm::vec4(position, spot ? 1.f : 0.f);
        data.localLightColor[count] =
            glm::vec4(light->color * (light->intensity / 3.14159265359f), light->range);
        data.localLightDirection[count] = glm::vec4(direction, std::cos(light->innerCone));
        int slot = -1;
        if (light->castShadows && spot && !spotShadow) {
            slot = 6;
            spotShadow = true;
        }
        if (light->castShadows && !spot && !pointShadow) {
            slot = 0;
            pointShadow = true;
        }
        data.localLightCone[count] = glm::vec4(std::cos(light->outerCone), float(slot), 0, 0);
        if (slot >= 0) {
            auto projection = glm::perspective(spot ? 2.f * light->outerCone : glm::radians(90.f),
                                               1.f, 0.025f, light->range);
            projection[1][1] *= -1;
            if (spot) {
                const glm::vec3 up =
                    std::abs(direction.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
                data.localShadowMatrix[6] =
                    projection * glm::lookAt(position, position + direction, up);
            } else {
                const glm::vec3 axes[] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                          {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
                const glm::vec3 up[] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1},
                                        {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
                for (int face = 0; face < 6; ++face)
                    data.localShadowMatrix[face] =
                        projection * glm::lookAt(position, position + axes[face], up[face]);
            }
        }
        ++count;
    }
    data.graphicsSettings = glm::vec4(float(count), _postProcess.celShading ? 1.f : 0.f,
                                      float(_drawExtent.width), float(_drawExtent.height));
}

void VulkanEngine::draw_local_shadows(VkCommandBuffer cmd) {
    // One point light (six faces) and one spot light, as in FirstPrinciples.
    // The other lights remain unshadowed; selection is stable in creation order.
    vkutil::transition_image(cmd, _localShadowAtlas.image, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    auto depth = vkinit::depth_attachment_info(_localShadowAtlas.imageView,
                                               VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    depth.clearValue.depthStencil.depth = 1.f;
    auto rendering = vkinit::rendering_info({2048, 1024}, nullptr, &depth);
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    for (int light = 0; light < int(sceneData.graphicsSettings.x); ++light) {
        const int base = int(sceneData.localLightCone[light].y);
        if (base < 0)
            continue;
        const int faces = base == 6 ? 1 : 6;
        for (int face = 0; face < faces; ++face) {
            const int slot = base + face;
            VkViewport viewport{float((slot % 4) * 512), float((slot / 4) * 512), 512, 512, 0, 1};
            VkRect2D scissor{{(slot % 4) * 512, (slot / 4) * 512}, {512, 512}};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            MaterialPipeline *last = nullptr;
            for (const auto &object : portalViewDrawContext.OpaqueSurfaces) {
                if (!object.material || !is_visible(object, sceneData.localShadowMatrix[slot]))
                    continue;
                auto &pipeline = object.material->passType == MaterialPass::Mask
                                     ? _shadow.maskPipeline
                                     : _shadow.pipeline;
                if (last != &pipeline) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
                    last = &pipeline;
                }
                if (object.material->passType == MaterialPass::Mask)
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout,
                                            0, 1, &object.material->materialSet, 0, nullptr);
                vkCmdBindIndexBuffer(cmd, object.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                ShadowPushConstants push{sceneData.localShadowMatrix[slot] * object.transform,
                                         object.vertexBufferAddress};
                vkCmdPushConstants(cmd, pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(push), &push);
                vkCmdDrawIndexed(cmd, object.indexCount, 1, object.firstIndex, 0, 0);
                ++stats.shadow_drawcall_count;
                stats.shadow_triangle_count += object.indexCount / 3;
            }
        }
    }
    vkCmdEndRendering(cmd);
    vkutil::transition_image(cmd, _localShadowAtlas.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
}
