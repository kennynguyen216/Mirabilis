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

    _localShadowAtlas=create_image({2048,1024,1},shadowFormat,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
    _mainDeletionQueue.push_function([this]{destroy_image(_localShadowAtlas);});
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
    for (auto& frame:_frames) {
        if (frame.rayTlas) _sunCasters.destroy(_device,frame.rayTlas,nullptr);
        destroy_buffer(frame.rayTlasBuffer); destroy_buffer(frame.rayInstances); destroy_buffer(frame.rayScratch); destroy_buffer(frame.rayHitInstances);
        frame.rayHitInstances={}; frame.rayHitCapacity=0;
        frame.rayTlas={}; frame.rayTlasBuffer={}; frame.rayInstances={}; frame.rayScratch={};
        frame.rayTlasCapacity=frame.rayInstanceCapacity=frame.rayScratchCapacity=frame.rayHash=0;
    }
    for (auto& mesh:_sunCasters.meshes) {
        _sunCasters.destroy(_device,mesh.structure,nullptr); destroy_buffer(mesh.buffer);
    }
    _sunCasters.meshes.clear(); _sunCasters.tlas={}; _sunCasters.hash=0;
}

void VulkanEngine::update_sun_casters()
{
    if (!_rayQueryShadows) return;
    auto& scene=_sunCasters;
    auto& frame=get_current_frame();
    if (!scene.hash) for (auto& slot:_frames) slot.rayHash=0;
    const auto address=[&](VkBuffer buffer) {
        VkBufferDeviceAddressInfo info{.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer=buffer};
        return vkGetBufferDeviceAddress(_device,&info);
    };
    const auto create=[&](VkAccelerationStructureTypeKHR type,VkDeviceSize bytes,AllocatedBuffer& buffer,VkAccelerationStructureKHR& structure) {
        buffer=create_buffer(bytes,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VMA_MEMORY_USAGE_GPU_ONLY);
        VkAccelerationStructureCreateInfoKHR info{.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        info.buffer=buffer.buffer; info.size=bytes; info.type=type;
        VK_CHECK(scene.create(_device,&info,nullptr,&structure));
    };
    // Old meshes are retired after this frame's fence, which also follows all
    // earlier graphics-queue readers. No other frame borrows this slot's TLAS.
    std::erase_if(scene.meshes,[&](const SunCasterScene::Mesh& mesh) {
        if (!mesh.source.expired()) return false;
        get_current_frame()._deletionQueue.push_function([this,mesh]() {
            _sunCasters.destroy(_device,mesh.structure,nullptr); destroy_buffer(mesh.buffer);
        });
        return true;
    });
    std::vector<const RenderObject*> casters;
    uint64_t hash=1469598103934665603ull;
    const auto mix=[&](const void* value,size_t size) { auto bytes=static_cast<const uint8_t*>(value); for(size_t i=0;i<size;++i){hash^=bytes[i];hash*=1099511628211ull;} };
    for (const auto& draw:worldDrawContext.OpaqueSurfaces) {
        if (!ray_query_caster(draw)) continue;
        casters.push_back(&draw);
        mix(&draw.transform,sizeof(draw.transform)); mix(&draw.vertexBufferAddress,sizeof(draw.vertexBufferAddress));
        mix(&draw.indexBuffer,sizeof(draw.indexBuffer)); mix(&draw.firstIndex,sizeof(draw.firstIndex)); mix(&draw.indexCount,sizeof(draw.indexCount));
    }
    size_t hitBytes=std::max<size_t>(casters.size(),1)*sizeof(GPURayInstance);
    if (hitBytes>frame.rayHitCapacity) {
        destroy_buffer(frame.rayHitInstances);
        frame.rayHitInstances=create_buffer(hitBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_CPU_TO_GPU);
        frame.rayHitCapacity=hitBytes;
    }
    auto* hits=static_cast<GPURayInstance*>(frame.rayHitInstances.info.pMappedData);
    for (size_t i=0;i<casters.size();++i) {
        const auto& draw=*casters[i];
        auto normals=make_draw_instance(draw.transform,draw.previousTransform,false);
        GPURayInstance hit;
        hit.vertices=draw.vertexBufferAddress; hit.indices=address(draw.indexBuffer);
        std::copy(std::begin(normals.normalRows),std::end(normals.normalRows),std::begin(hit.normalRows));
        hit.baseColor=draw.material->traceBaseColor; hit.baseColor.w=draw.material->traceParameters.x; hit.emission=draw.material->traceEmission;
        const auto& texture=draw.material->traceTexture;
        bool textured=texture && (texture->width!=1 || texture->height!=1 || texture->rgba.size()!=1);
        if (texture && !textured) for (int channel=0;channel<3;++channel) {
            float color=float((texture->rgba[0]>>(8*channel))&255u)/255.f;
            hit.baseColor[channel]*=color<=0.04045f?color/12.92f:std::pow((color+0.055f)/1.055f,2.4f);
        }
        hit.geometry=glm::uvec4(draw.firstIndex,draw.indexCount,draw.contributesToStaticCaches?1:0,textured?1:0);
        hits[i]=hit;
    }
    vmaFlushAllocation(_allocator,frame.rayHitInstances.allocation,0,VK_WHOLE_SIZE);
    scene.hash=hash;
    if (frame.rayTlas && frame.rayHash==hash) { scene.tlas=frame.rayTlas; return; }
    const auto started=std::chrono::steady_clock::now();
    constexpr auto inputUsage=VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    std::vector<size_t> meshIndices;
    std::vector<size_t> added;
    for (const auto* draw:casters) {
        const auto source=_traceMeshSources.at(draw->vertexBufferAddress).lock();
        auto it=std::find_if(scene.meshes.begin(),scene.meshes.end(),[&](const auto& mesh) {
            return mesh.vertices==draw->vertexBufferAddress && mesh.indices==draw->indexBuffer && mesh.first==draw->firstIndex && mesh.count==draw->indexCount && mesh.source.lock()==source;
        });
        size_t index=it-scene.meshes.begin();
        if (it==scene.meshes.end()) {
            scene.meshes.push_back({draw->vertexBufferAddress,draw->indexBuffer,draw->firstIndex,draw->indexCount,source});
            added.push_back(index);
        }
        meshIndices.push_back(index);
    }
    if (!added.empty()) {
        std::vector<VkAccelerationStructureGeometryKHR> geometries(added.size());
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> builds(added.size());
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(added.size());
        std::vector<VkDeviceSize> originalSizes(added.size());
        VkDeviceSize scratchBytes=0;
        for (size_t i=0;i<added.size();++i) {
            auto& mesh=scene.meshes[added[i]];
            auto& geometry=geometries[i]; geometry.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
            geometry.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR; geometry.flags=VK_GEOMETRY_OPAQUE_BIT_KHR;
            auto& triangles=geometry.geometry.triangles;
            triangles.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            triangles.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT; triangles.vertexData.deviceAddress=mesh.vertices;
            triangles.vertexStride=sizeof(Vertex); triangles.maxVertex=uint32_t(mesh.source.lock()->vertices.size()-1);
            triangles.indexType=VK_INDEX_TYPE_UINT32; triangles.indexData.deviceAddress=address(mesh.indices);
            uint32_t count=mesh.count/3;
            ranges[i]={count,mesh.first*uint32_t(sizeof(uint32_t)),0,0};
            auto& build=builds[i]; build.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            build.type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            build.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR|VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
            build.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; build.geometryCount=1; build.pGeometries=&geometry;
            VkAccelerationStructureBuildSizesInfoKHR sizes{.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            scene.buildSizes(_device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&build,&count,&sizes);
            create(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,sizes.accelerationStructureSize,mesh.buffer,mesh.structure);
            build.dstAccelerationStructure=mesh.structure;
            originalSizes[i]=sizes.accelerationStructureSize;
            scratchBytes=std::max(scratchBytes,sizes.buildScratchSize);
        }
        AllocatedBuffer scratch=create_buffer(scratchBytes+scene.scratchAlignment,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VMA_MEMORY_USAGE_GPU_ONLY);
        VkDeviceAddress scratchAddress=(address(scratch.buffer)+scene.scratchAlignment-1)/scene.scratchAlignment*scene.scratchAlignment;
        VkQueryPoolCreateInfo queryInfo{.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; queryInfo.queryType=VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR; queryInfo.queryCount=uint32_t(added.size());
        VkQueryPool query{}; VK_CHECK(vkCreateQueryPool(_device,&queryInfo,nullptr,&query));
        immediate_submit([&](VkCommandBuffer cmd) {
            vkCmdResetQueryPool(cmd,query,0,uint32_t(added.size()));
            for (size_t i=0;i<added.size();++i) {
                builds[i].scratchData.deviceAddress=scratchAddress;
                const auto* range=&ranges[i]; scene.build(cmd,1,&builds[i],&range);
                vkutil::memory_barrier(cmd);
                scene.writeProperties(cmd,1,&builds[i].dstAccelerationStructure,VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,query,uint32_t(i));
            }
        });
        std::vector<uint64_t> compactSizes(added.size());
        VK_CHECK(vkGetQueryPoolResults(_device,query,0,uint32_t(added.size()),compactSizes.size()*sizeof(uint64_t),compactSizes.data(),sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
        vkDestroyQueryPool(_device,query,nullptr);
        std::vector<AllocatedBuffer> compactBuffers(added.size());
        std::vector<VkAccelerationStructureKHR> compact(added.size());
        for (size_t i=0;i<added.size();++i) create(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,compactSizes[i],compactBuffers[i],compact[i]);
        immediate_submit([&](VkCommandBuffer cmd) {
            for (size_t i=0;i<added.size();++i) {
                VkCopyAccelerationStructureInfoKHR copy{.sType=VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
                copy.src=scene.meshes[added[i]].structure; copy.dst=compact[i]; copy.mode=VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
                scene.copy(cmd,&copy);
            }
            vkutil::memory_barrier(cmd);
        });
        for (size_t i=0;i<added.size();++i) {
            auto& mesh=scene.meshes[added[i]];
            scene.destroy(_device,mesh.structure,nullptr); destroy_buffer(mesh.buffer);
            mesh.structure=compact[i]; mesh.buffer=compactBuffers[i];
        }
        destroy_buffer(scratch); scene.blasBuilds+=added.size();
    }
    const VkDeviceSize instanceBytes=std::max<size_t>(1,casters.size())*sizeof(VkAccelerationStructureInstanceKHR);
    if (instanceBytes>frame.rayInstanceCapacity) {
        destroy_buffer(frame.rayInstances);
        frame.rayInstances=create_buffer(instanceBytes,inputUsage,VMA_MEMORY_USAGE_CPU_TO_GPU); frame.rayInstanceCapacity=instanceBytes;
    }
    auto* instances=static_cast<VkAccelerationStructureInstanceKHR*>(frame.rayInstances.info.pMappedData);
    for (size_t i=0;i<casters.size();++i) {
        auto& instance=instances[i]; instance={};
        for (int row=0;row<3;++row) for(int column=0;column<4;++column) instance.transform.matrix[row][column]=casters[i]->transform[column][row];
        instance.instanceCustomIndex=uint32_t(i); instance.mask=0xff;
        instance.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR|VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
        VkAccelerationStructureDeviceAddressInfoKHR info{.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,.accelerationStructure=scene.meshes[meshIndices[i]].structure};
        instance.accelerationStructureReference=scene.address(_device,&info);
    }
    vmaFlushAllocation(_allocator,frame.rayInstances.allocation,0,VK_WHOLE_SIZE);
    VkAccelerationStructureGeometryKHR geometry{.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR}; geometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR; geometry.geometry.instances.data.deviceAddress=address(frame.rayInstances.buffer);
    VkAccelerationStructureBuildGeometryInfoKHR build{.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type=VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; build.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; build.geometryCount=1; build.pGeometries=&geometry;
    frame.rayCount=uint32_t(casters.size());
    VkAccelerationStructureBuildSizesInfoKHR sizes{.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    scene.buildSizes(_device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&build,&frame.rayCount,&sizes);
    if (sizes.accelerationStructureSize>frame.rayTlasCapacity) {
        if (frame.rayTlas) scene.destroy(_device,frame.rayTlas,nullptr);
        destroy_buffer(frame.rayTlasBuffer); create(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,sizes.accelerationStructureSize,frame.rayTlasBuffer,frame.rayTlas);
        frame.rayTlasCapacity=sizes.accelerationStructureSize;
    }
    if (sizes.buildScratchSize+scene.scratchAlignment>frame.rayScratchCapacity) {
        destroy_buffer(frame.rayScratch); frame.rayScratchCapacity=sizes.buildScratchSize+scene.scratchAlignment;
        frame.rayScratch=create_buffer(frame.rayScratchCapacity,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VMA_MEMORY_USAGE_GPU_ONLY);
    }
    frame.rayHash=hash; frame.rayBuildPending=true; scene.tlas=frame.rayTlas;
    if (!added.empty()) fmt::print("Ray scene: {} instances, {} new mesh BLASes ({} total), prepared in {:.1f} ms\n",casters.size(),added.size(),scene.blasBuilds,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());
}

void VulkanEngine::record_sun_casters(VkCommandBuffer cmd)
{
    if (!_rayQueryShadows) return;
    auto& frame=get_current_frame();
    if (!frame.rayBuildPending) return;
    const auto address=[&](VkBuffer buffer) { VkBufferDeviceAddressInfo info{.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer=buffer}; return vkGetBufferDeviceAddress(_device,&info); };
    VkAccelerationStructureGeometryKHR geometry{.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR}; geometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR; geometry.geometry.instances.data.deviceAddress=address(frame.rayInstances.buffer);
    VkAccelerationStructureBuildGeometryInfoKHR build{.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type=VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; build.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; build.geometryCount=1; build.pGeometries=&geometry; build.dstAccelerationStructure=frame.rayTlas;
    build.scratchData.deviceAddress=(address(frame.rayScratch.buffer)+_sunCasters.scratchAlignment-1)/_sunCasters.scratchAlignment*_sunCasters.scratchAlignment;
    const VkAccelerationStructureBuildRangeInfoKHR range{frame.rayCount,0,0,0}; const auto* ranges=&range;
    _sunCasters.build(cmd,1,&build,&ranges); vkutil::memory_barrier(cmd);
    frame.rayBuildPending=false; ++_sunCasters.tlasBuilds; ++stats.sun_shadow_rebuild_count;
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

