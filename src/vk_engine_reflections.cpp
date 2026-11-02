#include "vk_engine.h"
#include "vk_engine_render_helpers.h"
#include <vk_images.h>
#include <vk_initializers.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <cstring>

void VulkanEngine::init_reflections()
{
    if (!_rayQueryShadows) return;
    auto& state=_reflections;
    if (!state.layout) {
        DescriptorLayoutBuilder builder;
        for(uint32_t i=0;i<=16;++i)
            builder.add_binding(i,(i<=1 || (i>=12 && i<=14) || i==16)?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        state.layout=builder.build(_device,VK_SHADER_STAGE_COMPUTE_BIT);
        const VkDescriptorSetLayout sets[]{_gpuSceneDataDescriptorLayout,state.layout};
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,112};
        auto layout=vkinit::pipeline_layout_create_info(); layout.setLayoutCount=2; layout.pSetLayouts=sets;
        layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&range;
        VK_CHECK(vkCreatePipelineLayout(_device,&layout,nullptr,&state.pipeline.layout));
        ScopedShaderModule shader(_device);
        if(!shader.load("../../shaders/scene_reflections.comp.rt.spv")) throw std::runtime_error("Scene reflection shader missing");
        VkComputePipelineCreateInfo pipeline{.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.layout=state.pipeline.layout;
        pipeline.stage=vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_COMPUTE_BIT,shader.get());
        VK_CHECK(vkCreateComputePipelines(_device,VK_NULL_HANDLE,1,&pipeline,nullptr,&state.pipeline.pipeline));
        _mainDeletionQueue.push_function([this] {
            for(auto* image:{&_reflections.raw,&_reflections.rawHit,&_reflections.radiance[0],&_reflections.radiance[1],
                    &_reflections.hit[0],&_reflections.hit[1],&_reflections.metadata[0],&_reflections.metadata[1]})
                if(image->image) destroy_image(*image);
            vkDestroyPipeline(_device,_reflections.pipeline.pipeline,nullptr);
            vkDestroyPipelineLayout(_device,_reflections.pipeline.layout,nullptr);
            vkDestroyDescriptorSetLayout(_device,_reflections.layout,nullptr);
        });
    }
    if(state.raw.image && state.raw.imageExtent.width==_drawImage.imageExtent.width && state.raw.imageExtent.height==_drawImage.imageExtent.height) return;
    if(state.raw.image) {
        // Extent changes are rare; ordinary motion and lighting never drain the device.
        VK_CHECK(vkDeviceWaitIdle(_device));
        for(auto* image:{&state.raw,&state.rawHit,&state.radiance[0],&state.radiance[1],&state.hit[0],&state.hit[1],&state.metadata[0],&state.metadata[1]}) destroy_image(*image);
    }
    constexpr auto usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    state.raw=create_image(_drawImage.imageExtent,VK_FORMAT_R16G16B16A16_SFLOAT,usage);
    state.rawHit=create_image(_drawImage.imageExtent,VK_FORMAT_R32G32B32A32_SFLOAT,usage);
    for(int i=0;i<2;++i) {
        state.radiance[i]=create_image(_drawImage.imageExtent,VK_FORMAT_R16G16B16A16_SFLOAT,usage);
        state.hit[i]=create_image(_drawImage.imageExtent,VK_FORMAT_R32G32B32A32_SFLOAT,usage);
        state.metadata[i]=create_image(_drawImage.imageExtent,VK_FORMAT_R16G16B16A16_SFLOAT,usage);
    }
    immediate_submit([&](VkCommandBuffer cmd) {
        const auto range=vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
        VkClearColorValue zero{};
        for(auto* image:{&state.raw,&state.rawHit,&state.radiance[0],&state.radiance[1],&state.hit[0],&state.hit[1],&state.metadata[0],&state.metadata[1]}) {
            vkutil::transition_image(cmd,image->image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cmd,image->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&range);
            vkutil::transition_image(cmd,image->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
        }
    });
    state.historyValid=false;
}

void VulkanEngine::draw_reflections(VkCommandBuffer cmd)
{
    auto& state=_reflections;
    if(sceneData.reflectionSettings.x<0.5 || !state.pipeline.pipeline) {state.historyValid=false;return;}
    // Reject all moving-scene/camera history initially. This conservative path
    // avoids reusing a stationary mirror's obsolete reflected objects.
    state.historyValid=state.historyValid && state.lastSceneHash==_sunCasters.hash &&
        state.lastLightingHash==_surfaceCache.lightingHash && !_surfaceCache.lightingPending &&
        state.liveExtent.width==_drawExtent.width && state.liveExtent.height==_drawExtent.height &&
        std::memcmp(&state.lastViewProjection,&sceneData.viewproj,sizeof(glm::mat4))==0;
    const uint32_t write=state.writeIndex,read=1-write;
    VkDescriptorSet set=get_current_frame()._frameDescriptors.allocate(_device,state.layout);
    DescriptorWriter writer;
    const std::pair<uint32_t,VkImageView> storage[]{{0,state.raw.imageView},{1,state.rawHit.imageView},
        {12,state.radiance[write].imageView},{13,state.hit[write].imageView},{14,state.metadata[write].imageView},{16,_drawImage.imageView}};
    for(auto [binding,view]:storage) writer.write_image(binding,view,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_GENERAL,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    const std::pair<uint32_t,VkImageView> sampled[]{{2,state.raw.imageView},{3,state.rawHit.imageView},
        {4,_prepass.depthImage.imageView},{5,_prepass.normalImage.imageView},{6,_sceneTargets.gbufferVelocity.imageView},
        {7,_sceneTargets.gbufferSpecular.imageView},{8,_sceneTargets.portalMask.imageView},{9,state.radiance[read].imageView},
        {10,state.hit[read].imageView},{11,state.metadata[read].imageView},{15,state.radiance[write].imageView}};
    for(auto [binding,view]:sampled) writer.write_image(binding,view,_prepass.sampler,(binding==2 || binding==3 || binding>=9)?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.update_set(_device,set);
    const VkDescriptorSet sets[]{get_current_frame().sceneDescriptor,set};
    struct Push { glm::mat4 inverseViewProjection; glm::vec4 camera; glm::uvec4 control; glm::vec4 settings; };
    static_assert(sizeof(Push)==112);
    Push push{glm::inverse(sceneData.viewproj),glm::inverse(sceneData.view)[3],glm::uvec4(_drawExtent.width,_drawExtent.height,uint32_t(_frameNumber),0),glm::vec4(state.historyValid?1.f:0.f,0,0,0)};
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,state.pipeline.pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,state.pipeline.layout,0,2,sets,0,nullptr);
    const auto dispatch=[&](uint32_t pass) {
        push.control.w=pass;
        vkCmdPushConstants(cmd,state.pipeline.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
        vkCmdDispatch(cmd,(_drawExtent.width+7)/8,(_drawExtent.height+7)/8,1);
    };
    vkutil::transition_image(cmd,_drawImage.image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    vkutil::memory_barrier(cmd); dispatch(0);
    vkutil::memory_barrier(cmd); dispatch(1);
    vkutil::memory_barrier(cmd); dispatch(2);
    vkutil::transition_image(cmd,_drawImage.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    state.historyValid=!_surfaceCache.lightingPending; state.lastSceneHash=_sunCasters.hash; state.lastLightingHash=_surfaceCache.lightingHash;
    state.lastViewProjection=sceneData.viewproj; state.liveExtent=_drawExtent; state.writeIndex=read;
}
