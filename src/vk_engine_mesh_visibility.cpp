#include "vk_engine.h"
#include "vk_engine_render_helpers.h"
#include <bit>
#include <cstring>
#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_pipelines.h>

namespace {
struct ClusterInput {
    glm::vec4 minimum, maximum;
    glm::uvec4 range;
};
static_assert(sizeof(ClusterInput) == 48);
struct CullPush {
    glm::mat4 viewProjection;
    glm::uvec4 extent;
};
} // namespace

void VulkanEngine::init_mesh_visibility() {
    if (!_meshVisibility.supported)
        return;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(_chosenGPU, &properties);
    _meshVisibility.maxIndirectCount = properties.limits.maxDrawIndirectCount;
    _meshVisibility.tiles = create_image(
        {(_drawImage.imageExtent.width + 7) / 8, (_drawImage.imageExtent.height + 7) / 8, 1},
        VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
    DescriptorLayoutBuilder builder;
    for (uint32_t binding = 0; binding < 3; ++binding)
        builder.add_binding(binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    builder.add_binding(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    _meshVisibility.layout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
    for (auto pair :
         {std::pair{&_meshVisibility.tilePipeline, "../../shaders/visibility_tiles.comp.spv"},
          std::pair{&_meshVisibility.cullPipeline, "../../shaders/mesh_cluster_cull.comp.spv"}}) {
        auto &pipeline = *pair.first;
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CullPush)};
        auto layout = vkinit::pipeline_layout_create_info();
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &_meshVisibility.layout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &range;
        VK_CHECK(vkCreatePipelineLayout(_device, &layout, nullptr, &pipeline.layout));
        ScopedShaderModule shader(_device);
        if (!shader.load(pair.second))
            throw std::runtime_error("Mesh visibility shader missing");
        VkComputePipelineCreateInfo create{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        create.layout = pipeline.layout;
        create.stage =
            vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_COMPUTE_BIT, shader.get());
        VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &create, nullptr,
                                          &pipeline.pipeline));
    }
    _mainDeletionQueue.push_function([this] {
        for (auto pipeline : {_meshVisibility.tilePipeline, _meshVisibility.cullPipeline}) {
            vkDestroyPipeline(_device, pipeline.pipeline, nullptr);
            vkDestroyPipelineLayout(_device, pipeline.layout, nullptr);
        }
        for (auto buffer : _meshVisibility.readback)
            if (buffer.buffer)
                destroy_buffer(buffer);
        vkDestroyDescriptorSetLayout(_device, _meshVisibility.layout, nullptr);
        destroy_image(_meshVisibility.tiles);
    });
}

void VulkanEngine::prepare_mesh_visibility(VkCommandBuffer cmd) {
    auto &frame = get_current_frame();
    frame.clusterRanges.clear();
    const auto slot = _frameNumber % FRAME_OVERLAP;
    auto &readback = _meshVisibility.readback[slot];
    if (_meshVisibility.readbackCounts[slot]) {
        vmaInvalidateAllocation(_allocator, readback.allocation, 0, VK_WHOLE_SIZE);
        const auto *commands =
            static_cast<const VkDrawIndexedIndirectCommand *>(readback.info.pMappedData);
        uint32_t hidden = 0;
        for (uint32_t i = 0; i < _meshVisibility.readbackCounts[slot]; ++i)
            hidden += commands[i].indexCount == 0;
        if (_frameNumber == 4)
            fmt::print("GPU cluster culling: {} / {} clusters rejected\n", hidden,
                       _meshVisibility.readbackCounts[slot]);
        _meshVisibility.readbackCounts[slot] = 0;
    }
    if (!_meshVisibility.enabled || !_meshVisibility.supported)
        return;
    const auto &objects = mainDrawContext.OpaqueSurfaces;
    std::vector<ClusterInput> inputs;
    std::vector<GPUDrawInstance> instances;
    instances.reserve(objects.size());
    frame.clusterRanges.resize(objects.size());
    for (uint32_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        const auto &object = objects[objectIndex];
        instances.push_back(make_draw_instance(object.transform, object.previousTransform,
                                               object.material && object.material->doubleSided));
        if (!object.contributesToStaticCaches || !object.clusters || !object.material ||
            object.material->passType != MaterialPass::MainColor)
            continue;
        // Build once on first use from the retained mesh copy. Disabled culling
        // adds no clustering work to ordinary imports, and instances share the result.
        if (object.clusters->empty()) {
            const auto found = _traceMeshSources.find(object.vertexBufferAddress);
            if (found == _traceMeshSources.end())
                continue;
            const auto source = found->second.lock();
            if (!source)
                continue;
            *object.clusters = build_mesh_clusters<Vertex>(source->vertices, source->indices,
                                                           object.firstIndex, object.indexCount);
        }
        if (!is_visible(object, sceneData.viewproj))
            continue;
        const uint32_t first = uint32_t(inputs.size());
        for (const auto &cluster : *object.clusters)
            inputs.push_back({cluster.minimum,
                              cluster.maximum,
                              {cluster.firstIndex, cluster.indexCount, objectIndex, 0}});
        frame.clusterRanges[objectIndex] = {first, uint32_t(inputs.size()) - first};
    }
    if (inputs.empty())
        return;
    const auto allocate = [&](AllocatedBuffer &buffer, size_t bytes, VkBufferUsageFlags usage,
                              VmaMemoryUsage memory) {
        if (buffer.buffer)
            destroy_buffer(buffer);
        buffer = create_buffer(bytes, usage, memory);
    };
    if (frame.clusterCapacity < inputs.size()) {
        frame.clusterCapacity = std::bit_ceil(inputs.size());
        allocate(frame.clusterInput, frame.clusterCapacity * sizeof(ClusterInput),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        allocate(frame.clusterCommands,
                 frame.clusterCapacity * sizeof(VkDrawIndexedIndirectCommand),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VMA_MEMORY_USAGE_GPU_ONLY);
    }
    if (frame.clusterInstanceCapacity < instances.size()) {
        frame.clusterInstanceCapacity = std::bit_ceil(instances.size());
        allocate(frame.clusterInstances, frame.clusterInstanceCapacity * sizeof(GPUDrawInstance),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    }
    std::memcpy(frame.clusterInput.info.pMappedData, inputs.data(),
                inputs.size() * sizeof(ClusterInput));
    std::memcpy(frame.clusterInstances.info.pMappedData, instances.data(),
                instances.size() * sizeof(GPUDrawInstance));
    vmaFlushAllocation(_allocator, frame.clusterInput.allocation, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(_allocator, frame.clusterInstances.allocation, 0, VK_WHOLE_SIZE);
    const auto set = frame._frameDescriptors.allocate(_device, _meshVisibility.layout);
    DescriptorWriter writer;
    writer.write_buffer(0, frame.clusterInput.buffer, VK_WHOLE_SIZE, 0,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.write_buffer(1, frame.clusterInstances.buffer, VK_WHOLE_SIZE, 0,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.write_buffer(2, frame.clusterCommands.buffer, VK_WHOLE_SIZE, 0,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.write_image(3, _prepass.depthImage.imageView, _prepass.sampler,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.write_image(4, _meshVisibility.tiles.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    writer.update_set(_device, set);
    CullPush push{sceneData.viewproj,
                  {_drawExtent.width, _drawExtent.height, uint32_t(inputs.size()), 0}};
    vkutil::transition_image(cmd, _meshVisibility.tiles.image, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_GENERAL);
    const auto dispatch = [&](const MaterialPipeline &pipeline, uint32_t x, uint32_t y) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(cmd, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatch(cmd, x, y, 1);
    };
    dispatch(_meshVisibility.tilePipeline, (_drawExtent.width + 7) / 8,
             (_drawExtent.height + 7) / 8);
    vkutil::memory_barrier(cmd);
    dispatch(_meshVisibility.cullPipeline, (uint32_t(inputs.size()) + 63) / 64, 1);
    vkutil::memory_barrier(cmd);
    if (SDL_getenv("MIRABILIS_TEST_CLUSTER_COUNTS")) {
        const size_t bytes = inputs.size() * sizeof(VkDrawIndexedIndirectCommand);
        if (!readback.buffer || readback.info.size < bytes)
            allocate(readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
        const VkBufferCopy copy{0, 0, bytes};
        vkCmdCopyBuffer(cmd, frame.clusterCommands.buffer, readback.buffer, 1, &copy);
        vkutil::memory_barrier(cmd);
        _meshVisibility.readbackCounts[slot] = uint32_t(inputs.size());
    }
    // The current-frame main depth is valid only for the main camera. Portal,
    // alpha-cutout and dynamic draws deliberately retain their ordinary paths.
}
