#include "vk_engine.h"
#include "vk_engine_render_helpers.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

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
    _shadow.formatName = string_VkFormat(shadowFormat);

    _shadow.mapImage = create_image(
        VkExtent3D{ShadowMapResolution, ShadowMapResolution, 1},
        shadowFormat,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    // A comparison sampler runs the depth test in hardware and filters the
    // results, which is what makes each PCF tap cheap.  The white border
    // leaves everything outside the shadowed box lit, instead of stamping a
    // dark square edge onto the world.
    VkSamplerCreateInfo samplerInfo = sampler_info(
        shadowFilter, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VK_CHECK(vkCreateSampler(_device, &samplerInfo, nullptr, &_shadow.sampler));

    _mainDeletionQueue.push_function([this]() {
        vkDestroySampler(_device, _shadow.sampler, nullptr);
        destroy_image(_shadow.mapImage);
    });
}

void VulkanEngine::init_shadow_pipeline()
{
    ScopedShaderModule shadowVertexShader(_device);
    if (!shadowVertexShader.load("../../shaders/shadow_depth.vert.spv")) {
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
        _device, &layoutInfo, nullptr, &_shadow.pipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _shadow.pipeline.layout;
    builder.set_vertex_only_shader(shadowVertexShader.get());
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
    builder.set_depth_format(_shadow.mapImage.imageFormat);
    _shadow.pipeline.pipeline = builder.build_pipeline(_device);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _shadow.pipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _shadow.pipeline.layout, nullptr);
    });
}

void VulkanEngine::init_shadow_mask_pipeline()
{
    ScopedShaderModule vertexShader(_device);
    ScopedShaderModule fragmentShader(_device);
    if (!vertexShader.load("../../shaders/shadow_depth_mask.vert.spv") ||
        !fragmentShader.load("../../shaders/shadow_depth_mask.frag.spv")) {
        fmt::print("Error loading alpha-tested shadow shaders\n");
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
        _device, &layoutInfo, nullptr, &_shadow.maskPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _shadow.maskPipeline.layout;
    builder.set_shaders(vertexShader.get(), fragmentShader.get());
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
    builder.set_depth_format(_shadow.mapImage.imageFormat);
    _shadow.maskPipeline.pipeline = builder.build_pipeline(_device);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _shadow.maskPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _shadow.maskPipeline.layout, nullptr);
    });
}

void VulkanEngine::draw_shadow_map(VkCommandBuffer cmd)
{
    const auto startTime = std::chrono::steady_clock::now();

    vkutil::transition_image(
        cmd,
        _shadow.mapImage.image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _shadow.mapImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
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

    if (_shadow.enabled && _shadow.pipeline.pipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(
            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _shadow.pipeline.pipeline);

        VkPipeline lastPipeline = _shadow.pipeline.pipeline;
        MaterialInstance* lastMaterial = nullptr;
        VkBuffer lastIndexBuffer = VK_NULL_HANDLE;
        // portalViewDrawContext is the world plus the player's body, which is
        // what actually exists in the level.  The main camera's context is
        // not usable here: it also carries the portal quads, and it omits the
        // body the first-person camera sits inside.
        // Transparent surfaces intentionally do not cast yet: a blended
        // surface has no single silhouette to cast.  Masked ones do, and they
        // go through _shadow.maskPipeline so foliage and grates cast their
        // cutout rather than the rectangle it is painted on.
        // IQ2: while the ray query owns the trace scene's casters, the map
        // keeps only what that scene leaves out: masked draws, and the body,
        // which update_scene() appends after the world's draws.
        const size_t worldDraws = worldDrawContext.OpaqueSurfaces.size();
        for (size_t drawIndex = 0;
                drawIndex < portalViewDrawContext.OpaqueSurfaces.size(); ++drawIndex) {
            const RenderObject& renderObject =
                portalViewDrawContext.OpaqueSurfaces[drawIndex];
            if (_rayQueryShadows && drawIndex < worldDraws &&
                    ray_query_caster(renderObject)) {
                continue;
            }
            // Cull against the light, never against the player's camera: an
            // object behind the camera can still drop a shadow into view.
            if (!is_visible(renderObject, _shadow.sunViewProjection)) {
                continue;
            }

            const bool masked = renderObject.material != nullptr &&
                renderObject.material->passType == MaterialPass::Mask &&
                _shadow.maskPipeline.pipeline != VK_NULL_HANDLE;
            const MaterialPipeline& active = masked
                ? _shadow.maskPipeline
                : _shadow.pipeline;
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
            pushConstants.lightMatrix = _shadow.sunViewProjection * renderObject.transform;
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
        _shadow.mapImage.image,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    stats.shadow_record_time = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - startTime).count();
}

bool VulkanEngine::ray_query_caster(const RenderObject& object) const
{
    // update_trace_scene()'s filter, so the BLAS holds exactly _traceTriangles.
    if (object.material == nullptr || object.material->passType != MaterialPass::MainColor) {
        return false;
    }
    const auto source = _traceMeshSources.find(object.vertexBufferAddress);
    return source != _traceMeshSources.end() && !source->second.expired() &&
        std::abs(glm::determinant(glm::mat3(object.transform))) >= 1e-12f;
}

void VulkanEngine::destroy_sun_casters()
{
    if (_sunCasters.tlas) _sunCasters.destroy(_device, _sunCasters.tlas, nullptr);
    if (_sunCasters.blas) _sunCasters.destroy(_device, _sunCasters.blas, nullptr);
    destroy_buffer(_sunCasters.tlasBuffer);
    destroy_buffer(_sunCasters.blasBuffer);
    _sunCasters.tlas = _sunCasters.blas = VK_NULL_HANDLE;
    _sunCasters.tlasBuffer = _sunCasters.blasBuffer = {};
}

void VulkanEngine::update_sun_casters()
{
    if (!_rayQueryShadows) {
        return;
    }
    struct Caster { const RenderObject* object; uint32_t vertexCount; };
    std::vector<Caster> casters;
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](const void* data, size_t bytes) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) { hash ^= p[i]; hash *= 1099511628211ull; }
    };
    for (const RenderObject& object : worldDrawContext.OpaqueSurfaces) {
        if (!ray_query_caster(object)) {
            continue;
        }
        casters.push_back({&object, static_cast<uint32_t>(
            _traceMeshSources.at(object.vertexBufferAddress).lock()->vertices.size())});
        mix(&object.transform, sizeof(object.transform));
        mix(&object.vertexBufferAddress, sizeof(object.vertexBufferAddress));
        mix(&object.indexBuffer, sizeof(object.indexBuffer));
        mix(&object.firstIndex, sizeof(object.firstIndex));
        mix(&object.indexCount, sizeof(object.indexCount));
    }
    if (_sunCasters.tlas != VK_NULL_HANDLE && hash == _sunCasters.hash) {
        return;
    }
    // ponytail: static-scene policy, as update_trace_scene(): any caster change
    // rebuilds everything behind a device wait.  Per-mesh BLASes and per-draw
    // TLAS instances, rebuilt in the frame, if moving casters ever matter.
    const auto start = std::chrono::steady_clock::now();
    VK_CHECK(vkDeviceWaitIdle(_device));
    destroy_sun_casters();
    _sunCasters.hash = hash;

    const auto bufferAddress = [&](VkBuffer buffer) {
        const VkBufferDeviceAddressInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer};
        return vkGetBufferDeviceAddress(_device, &info);
    };
    constexpr VkBufferUsageFlags buildInput =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    // One geometry per draw: its own vertex and index buffers, placed in the
    // world by a row-major 3x4 copy of its transform.
    AllocatedBuffer transforms = create_buffer(
        std::max<size_t>(casters.size(), 1) * sizeof(VkTransformMatrixKHR),
        buildInput, VMA_MEMORY_USAGE_CPU_TO_GPU);
    auto* matrices = static_cast<VkTransformMatrixKHR*>(transforms.info.pMappedData);
    std::vector<VkAccelerationStructureGeometryKHR> geometries(casters.size());
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(casters.size());
    std::vector<uint32_t> triangleCounts(casters.size());
    size_t triangles = 0;
    for (size_t i = 0; i < casters.size(); ++i) {
        const RenderObject& object = *casters[i].object;
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 4; ++column) {
                matrices[i].matrix[row][column] = object.transform[column][row];
            }
        }
        VkAccelerationStructureGeometryKHR& geometry = geometries[i];
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        VkAccelerationStructureGeometryTrianglesDataKHR& data = geometry.geometry.triangles;
        data.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        data.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        data.vertexData.deviceAddress = object.vertexBufferAddress; // Vertex::position leads
        data.vertexStride = sizeof(Vertex);
        data.maxVertex = std::max(casters[i].vertexCount, 1u) - 1;
        data.indexType = VK_INDEX_TYPE_UINT32;
        data.indexData.deviceAddress = bufferAddress(object.indexBuffer);
        data.transformData.deviceAddress = bufferAddress(transforms.buffer);
        triangleCounts[i] = object.indexCount / 3;
        ranges[i] = {triangleCounts[i], static_cast<uint32_t>(object.firstIndex * sizeof(uint32_t)),
            0, static_cast<uint32_t>(i * sizeof(VkTransformMatrixKHR))};
        triangles += triangleCounts[i];
    }
    vmaFlushAllocation(_allocator, transforms.allocation, 0, VK_WHOLE_SIZE);

    const auto create = [&](VkAccelerationStructureTypeKHR type, VkDeviceSize size,
            AllocatedBuffer& buffer, VkAccelerationStructureKHR& structure) {
        buffer = create_buffer(size,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY);
        VkAccelerationStructureCreateInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        info.buffer = buffer.buffer;
        info.size = size;
        info.type = type;
        VK_CHECK(_sunCasters.create(_device, &info, nullptr, &structure));
    };
    const auto sizes = [&](VkAccelerationStructureBuildGeometryInfoKHR& info, const uint32_t* counts) {
        VkAccelerationStructureBuildSizesInfoKHR result{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        _sunCasters.buildSizes(_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &info, counts, &result);
        return result;
    };

    VkAccelerationStructureBuildGeometryInfoKHR blasInfo{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    blasInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    // IQ11: compacted after the build, about half the memory for the same rays.
    blasInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
        VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    blasInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    blasInfo.geometryCount = static_cast<uint32_t>(geometries.size());
    blasInfo.pGeometries = geometries.data();
    VkAccelerationStructureBuildSizesInfoKHR blasSizes{};
    if (!casters.empty()) {
        blasSizes = sizes(blasInfo, triangleCounts.data());
        create(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            blasSizes.accelerationStructureSize, _sunCasters.blasBuffer, _sunCasters.blas);
    }
    AllocatedBuffer instances = create_buffer(
        sizeof(VkAccelerationStructureInstanceKHR), buildInput, VMA_MEMORY_USAGE_CPU_TO_GPU);
    VkAccelerationStructureGeometryKHR instanceGeometry{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    instanceGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    instanceGeometry.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instanceGeometry.geometry.instances.data.deviceAddress = bufferAddress(instances.buffer);
    VkAccelerationStructureBuildGeometryInfoKHR tlasInfo{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tlasInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlasInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasInfo.geometryCount = 1;
    tlasInfo.pGeometries = &instanceGeometry;
    // An empty scene still gets a TLAS, with no instance: the ray-query
    // shaders bind one whatever the scene holds.
    const uint32_t instanceCount = casters.empty() ? 0u : 1u;
    const VkAccelerationStructureBuildSizesInfoKHR tlasSizes = sizes(tlasInfo, &instanceCount);
    create(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        tlasSizes.accelerationStructureSize, _sunCasters.tlasBuffer, _sunCasters.tlas);

    // One scratch buffer serves both builds, one after the other.
    const VkDeviceSize alignment = _sunCasters.scratchAlignment;
    AllocatedBuffer scratch = create_buffer(
        std::max(blasSizes.buildScratchSize, tlasSizes.buildScratchSize) + alignment,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);
    const VkDeviceAddress scratchAddress =
        (bufferAddress(scratch.buffer) + alignment - 1) / alignment * alignment;
    // A build or copy finishes before the next reads its result or reuses scratch.
    // Without VK_KHR_ray_tracing_maintenance1, copies run in the BUILD stage.
    const auto structureBarrier = [](VkCommandBuffer cmd) {
        VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    };

    VkAccelerationStructureInstanceKHR instance{};
    VkDeviceSize compactedSize = 0;
    VkAccelerationStructureKHR compact = VK_NULL_HANDLE;
    AllocatedBuffer compactBuffer{};
    if (!casters.empty()) {
        VkQueryPoolCreateInfo queryInfo{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queryInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        queryInfo.queryCount = 1;
        VkQueryPool query = VK_NULL_HANDLE;
        VK_CHECK(vkCreateQueryPool(_device, &queryInfo, nullptr, &query));
        immediate_submit([&](VkCommandBuffer cmd) {
            vkCmdResetQueryPool(cmd, query, 0, 1);
            blasInfo.dstAccelerationStructure = _sunCasters.blas;
            blasInfo.scratchData.deviceAddress = scratchAddress;
            const VkAccelerationStructureBuildRangeInfoKHR* blasRanges = ranges.data();
            _sunCasters.build(cmd, 1, &blasInfo, &blasRanges);
            structureBarrier(cmd);
            _sunCasters.writeProperties(cmd, 1, &_sunCasters.blas,
                VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, query, 0);
        });
        VK_CHECK(vkGetQueryPoolResults(_device, query, 0, 1, sizeof(compactedSize),
            &compactedSize, sizeof(compactedSize),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        vkDestroyQueryPool(_device, query, nullptr);
        create(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, compactedSize,
            compactBuffer, compact);
        const VkAccelerationStructureDeviceAddressInfoKHR compactAddress{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
            .accelerationStructure = compact};
        instance.accelerationStructureReference = _sunCasters.address(_device, &compactAddress);
    }
    // The BLAS is already in world space.  Casting from either face is what
    // the shadow pass does too (it culls nothing).
    instance.transform.matrix[0][0] = instance.transform.matrix[1][1] =
        instance.transform.matrix[2][2] = 1.0f;
    instance.mask = 0xFF;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
        VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
    std::memcpy(instances.info.pMappedData, &instance, sizeof(instance));
    vmaFlushAllocation(_allocator, instances.allocation, 0, VK_WHOLE_SIZE);

    immediate_submit([&](VkCommandBuffer cmd) {
        if (compact != VK_NULL_HANDLE) {
            VkCopyAccelerationStructureInfoKHR copyInfo{
                .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
            copyInfo.src = _sunCasters.blas;
            copyInfo.dst = compact;
            copyInfo.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
            _sunCasters.copy(cmd, &copyInfo);
            structureBarrier(cmd);
        }
        tlasInfo.dstAccelerationStructure = _sunCasters.tlas;
        tlasInfo.scratchData.deviceAddress = scratchAddress;
        const VkAccelerationStructureBuildRangeInfoKHR tlasRange{instanceCount, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* tlasRanges = &tlasRange;
        _sunCasters.build(cmd, 1, &tlasInfo, &tlasRanges);
    });
    if (compact != VK_NULL_HANDLE) {
        _sunCasters.destroy(_device, _sunCasters.blas, nullptr);
        destroy_buffer(_sunCasters.blasBuffer);
        _sunCasters.blas = compact;
        _sunCasters.blasBuffer = compactBuffer;
    }
    destroy_buffer(scratch);
    destroy_buffer(instances);
    destroy_buffer(transforms);
    const double milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    constexpr double MiB = 1024.0 * 1024.0;
    fmt::print("IQ2 sun casters: {} draws, {} triangles; BLAS {:.2f} MiB (compacted from {:.2f}), "
        "TLAS {:.3f} MiB, scratch {:.2f} MiB; build {:.1f} ms (wall clock, device wait included)\n",
        casters.size(), triangles, compactedSize / MiB, blasSizes.accelerationStructureSize / MiB,
        tlasSizes.accelerationStructureSize / MiB,
        std::max(blasSizes.buildScratchSize, tlasSizes.buildScratchSize) / MiB, milliseconds);
}

glm::mat4 VulkanEngine::compute_sun_view_projection(const glm::vec3& focusPoint) const
{
    const glm::vec3 toLight = normalized_sun_direction(_shadow.sunlightDirection);
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
        (2.0f * _shadow.radius) / static_cast<float>(ShadowMapResolution);
    focus.x = std::floor(focus.x / texelWorldSize) * texelWorldSize;
    focus.y = std::floor(focus.y / texelWorldSize) * texelWorldSize;

    // The extra depth reaches casters standing outside the lit box; only the
    // x/y extent decides how much ground can receive a shadow.
    const float depthHalfRange = _shadow.radius + _shadow.depthMargin;
    glm::mat4 projection = glm::ortho(
        focus.x - _shadow.radius,
        focus.x + _shadow.radius,
        focus.y - _shadow.radius,
        focus.y + _shadow.radius,
        -focus.z - depthHalfRange,
        -focus.z + depthHalfRange);
    // Vulkan's framebuffer Y runs opposite glm's, exactly as the main camera
    // projection corrects it.
    projection[1][1] *= -1.0f;
    return projection * lightRotation;
}

