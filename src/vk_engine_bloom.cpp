#include "vk_engine.h"
#include "vk_engine_render_helpers.h"
#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_pipelines.h>

void VulkanEngine::init_bloom() {
    DescriptorLayoutBuilder builder;
    builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    _postProcess.bloomLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    auto layout = vkinit::pipeline_layout_create_info();
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &_postProcess.bloomLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    VK_CHECK(vkCreatePipelineLayout(_device, &layout, nullptr, &_postProcess.bloomPipeline.layout));
    ScopedShaderModule shader(_device);
    if (!shader.load("../../shaders/bloom.comp.spv"))
        throw std::runtime_error("Bloom shader missing");
    VkComputePipelineCreateInfo pipeline{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.layout = _postProcess.bloomPipeline.layout;
    pipeline.stage =
        vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_COMPUTE_BIT, shader.get());
    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &pipeline, nullptr,
                                      &_postProcess.bloomPipeline.pipeline));
    for (int level = 0; level < PostProcessState::BloomLevels; ++level) {
        for (int up = 0; up < 2; ++up) {
            auto &image = up ? _postProcess.bloomUp[level] : _postProcess.bloomDown[level];
            image = create_image({std::max(1u, _drawImage.imageExtent.width >> (level + 1)),
                                  std::max(1u, _drawImage.imageExtent.height >> (level + 1)), 1},
                                 VK_FORMAT_R16G16B16A16_SFLOAT,
                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        }
    }
    immediate_submit([this](VkCommandBuffer cmd) {
        VkClearColorValue black{};
        auto range = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
        for (int level = 0; level < PostProcessState::BloomLevels; ++level)
            for (auto *image : {&_postProcess.bloomDown[level], &_postProcess.bloomUp[level]}) {
                vkutil::transition_image(cmd, image->image, VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                vkCmdClearColorImage(cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     &black, 1, &range);
                vkutil::transition_image(cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
    });
    for (int level = 0; level < PostProcessState::BloomLevels; ++level)
        for (int up = 0; up < 2; ++up) {
            auto &set = up ? _postProcess.bloomUpSets[level] : _postProcess.bloomDownSets[level];
            set = globalDescriptorAllocator.allocate(_device, _postProcess.bloomLayout);
            const int last = PostProcessState::BloomLevels - 1;
            const auto &source = up ? (level == last ? _postProcess.bloomDown[level]
                                                     : _postProcess.bloomUp[level + 1])
                                    : (level == 0 ? _drawImage : _postProcess.bloomDown[level - 1]);
            const auto &target = up ? _postProcess.bloomUp[level] : _postProcess.bloomDown[level];
            DescriptorWriter writer;
            writer.write_image(0, source.imageView, _postProcess.sampler,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            writer.write_image(1, (up ? _postProcess.bloomDown[level] : source).imageView,
                               _postProcess.sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            writer.write_image(2, target.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                               VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            writer.update_set(_device, set);
        }
    _mainDeletionQueue.push_function([this] {
        for (auto image : _postProcess.bloomDown)
            destroy_image(image);
        for (auto image : _postProcess.bloomUp)
            destroy_image(image);
        vkDestroyPipeline(_device, _postProcess.bloomPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _postProcess.bloomPipeline.layout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _postProcess.bloomLayout, nullptr);
    });
}

void VulkanEngine::draw_bloom(VkCommandBuffer cmd) {
    GpuPassScope gpu(*this,cmd,GpuPass::Bloom);
    const auto extent = [&](int level) {
        return glm::ivec2(std::max(1u, _drawExtent.width >> (level + 1)),
                          std::max(1u, _drawExtent.height >> (level + 1)));
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _postProcess.bloomPipeline.pipeline);
    for (int up = 0; up < 2; ++up)
        for (int step = 0; step < PostProcessState::BloomLevels; ++step) {
            const int level = up ? PostProcessState::BloomLevels - 1 - step : step;
            auto &image = up ? _postProcess.bloomUp[level] : _postProcess.bloomDown[level];
            const auto output = extent(level);
            const auto input = up ? extent(std::min(level + 1, PostProcessState::BloomLevels - 1))
                                  : (level == 0 ? glm::ivec2(_drawExtent.width, _drawExtent.height)
                                                : extent(level - 1));
            vkutil::transition_image(cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                     VK_IMAGE_LAYOUT_GENERAL);
            struct {
                glm::ivec4 extents;
                glm::vec4 settings;
            } push{{output, input},
                   {_postProcess.bloomThreshold, _postProcess.bloomScatter,
                    level == 0 && !up ? 1.f : 0.f, up ? 1.f : 0.f}};
            const auto set =
                up ? _postProcess.bloomUpSets[level] : _postProcess.bloomDownSets[level];
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    _postProcess.bloomPipeline.layout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(cmd, _postProcess.bloomPipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(push), &push);
            vkCmdDispatch(cmd, (output.x + 7) / 8, (output.y + 7) / 8, 1);
            vkutil::transition_image(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
}
