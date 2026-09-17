#include "vk_engine.h"
#include "vk_engine_render_helpers.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

#include <glm/gtc/matrix_access.hpp>

#include "imgui.h"

#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_pipelines.h>

// Lumen-lite surface cache, checkpoint 4: cards and their capture.  Every
// opaque instance gets six cards, one per local axis direction, each an
// orthographic view of the instance's local bounds.  They are packed into one
// atlas and rasterised with the scene's own materials.  Lighting the atlas and
// reading it back at distance-field hits come later; see
// docs/lumen_lite_design.md.

namespace {

constexpr VkFormat AlbedoFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat NormalFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat EmissiveFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat DepthPageFormat = VK_FORMAT_R16_SFLOAT;
constexpr VkFormat CaptureDepthFormat = VK_FORMAT_D32_SFLOAT;

// Empty texels between cards, so linear filtering at a card's edge never
// reads its neighbour.
constexpr uint32_t Gutter = 1;
constexpr uint32_t MinCardTexels = 2;
constexpr uint32_t MaxCardTexels = 1024;

struct InstanceBounds {
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};
    bool valid() const { return min.x <= max.x; }
};

InstanceBounds local_bounds(
    const TraceMeshSource& source, const SceneOpaqueInstance& instance)
{
    InstanceBounds bounds;
    for (const RenderObject& draw : instance.draws) {
        const size_t end = std::min(
            source.indices.size(), size_t(draw.firstIndex) + draw.indexCount);
        for (size_t i = draw.firstIndex; i < end; ++i) {
            const uint32_t index = source.indices[i];
            if (index < source.vertices.size()) {
                bounds.min = glm::min(bounds.min, source.vertices[index].position);
                bounds.max = glm::max(bounds.max, source.vertices[index].position);
            }
        }
    }
    return bounds;
}

// Rows mapping a local position to card clip space: x and y across the card,
// z = 0 at the card's face of the box and 1 at the far face.
void set_card_projection(SurfaceCard& card)
{
    const int a = card.axis;
    const int u = (a + 1) % 3;
    const int v = (a + 2) % 3;
    const glm::vec3 lo = card.localMin;
    const glm::vec3 extent = card.localMax - card.localMin;
    card.row0 = glm::vec4(0.0f);
    card.row1 = glm::vec4(0.0f);
    card.row2 = glm::vec4(0.0f);
    card.row0[u] = 2.0f / extent[u];
    card.row0.w = -2.0f * lo[u] / extent[u] - 1.0f;
    card.row1[v] = 2.0f / extent[v];
    card.row1.w = -2.0f * lo[v] / extent[v] - 1.0f;
    if (card.sign > 0.0f) {
        card.row2[a] = -1.0f / extent[a];
        card.row2.w = card.localMax[a] / extent[a];
    } else {
        card.row2[a] = 1.0f / extent[a];
        card.row2.w = -lo[a] / extent[a];
    }
}

glm::vec3 apply_rows(const SurfaceCard& card, glm::vec3 local)
{
    const glm::vec4 p(local, 1.0f);
    return {glm::dot(card.row0, p), glm::dot(card.row1, p), glm::dot(card.row2, p)};
}

// Shelf packing, tallest first.  Returns false when the cards do not fit.
bool pack_cards(std::vector<SurfaceCard>& cards, uint32_t side)
{
    std::vector<size_t> order(cards.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](size_t l, size_t r) {
        return cards[l].rect.w > cards[r].rect.w;
    });
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t shelfHeight = 0;
    for (size_t index : order) {
        SurfaceCard& card = cards[index];
        const uint32_t w = card.rect.z + Gutter;
        const uint32_t h = card.rect.w + Gutter;
        if (w > side) {
            return false;
        }
        if (x + w > side) {
            x = 0;
            y += shelfHeight;
            shelfHeight = 0;
        }
        if (y + h > side) {
            return false;
        }
        card.rect.x = x;
        card.rect.y = y;
        x += w;
        shelfHeight = std::max(shelfHeight, h);
    }
    return true;
}

} // namespace

void VulkanEngine::init_surface_cache_resources()
{
    ScopedShaderModule vertexShader(_device);
    ScopedShaderModule fragmentShader(_device);
    if (vertexShader.load("../../shaders/surface_card_capture.vert.spv") &&
        fragmentShader.load("../../shaders/surface_card_capture.frag.spv") &&
        metalRoughMaterial.opaquePipeline.layout != VK_NULL_HANDLE) {
        // The forward pass's layout: scene set, material set, and the same
        // push-constant block, whose previous-transform rows carry the card
        // projection here.
        PipelineBuilder builder;
        builder._pipelineLayout = metalRoughMaterial.opaquePipeline.layout;
        builder.set_shaders(vertexShader.get(), fragmentShader.get());
        builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
        // The fragment shader's facing test does the culling, from normals
        // rather than from winding, which glTF does not keep consistent.
        builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
        builder.set_multisampling_none();
        const std::array<VkFormat, 4> formats{
            AlbedoFormat, NormalFormat, EmissiveFormat, DepthPageFormat};
        builder.set_color_attachment_formats(formats);
        builder.disable_blending();
        // Conventional depth: 0 at the card, the nearest surface wins.
        builder.enable_depthtest(true, VK_COMPARE_OP_LESS);
        builder.set_depth_format(CaptureDepthFormat);
        _surfaceCache.capturePipeline = builder.build_pipeline(_device);
    } else {
        _surfaceCache.status = "surface card capture shaders failed to load";
        fmt::print("Error loading surface card capture shaders\n");
    }

    DescriptorLayoutBuilder layoutBuilder;
    layoutBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    for (uint32_t binding = 1; binding <= 6; ++binding) {
        layoutBuilder.add_binding(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    }
    _surfaceCache.debugLayout =
        layoutBuilder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
    VkPushConstantRange pushRange{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(glm::vec4)};
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &_surfaceCache.debugLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &layoutInfo, nullptr, &_surfaceCache.debugPipelineLayout));
    ScopedShaderModule debugShader(_device);
    if (debugShader.load("../../shaders/surface_cache_debug.comp.spv")) {
        VkPipelineShaderStageCreateInfo stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = debugShader.get();
        stage.pName = "main";
        VkComputePipelineCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        createInfo.layout = _surfaceCache.debugPipelineLayout;
        createInfo.stage = stage;
        VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
            &createInfo, nullptr, &_surfaceCache.debugPipeline));
    }

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        _surfaceCache.directLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
        VkPushConstantRange directRange{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(SurfaceCacheDirectPushConstants)};
        VkPipelineLayoutCreateInfo directLayoutInfo = vkinit::pipeline_layout_create_info();
        directLayoutInfo.setLayoutCount = 1;
        directLayoutInfo.pSetLayouts = &_surfaceCache.directLayout;
        directLayoutInfo.pushConstantRangeCount = 1;
        directLayoutInfo.pPushConstantRanges = &directRange;
        VK_CHECK(vkCreatePipelineLayout(
            _device, &directLayoutInfo, nullptr, &_surfaceCache.directPipelineLayout));
        ScopedShaderModule directShader(_device);
        if (directShader.load("../../shaders/surface_cache_direct.comp.spv")) {
            VkPipelineShaderStageCreateInfo stage{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = directShader.get();
            stage.pName = "main";
            VkComputePipelineCreateInfo createInfo{
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            createInfo.layout = _surfaceCache.directPipelineLayout;
            createInfo.stage = stage;
            VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
                &createInfo, nullptr, &_surfaceCache.directPipeline));
        }
    }

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        for (uint32_t binding = 1; binding <= 8; ++binding) {
            builder.add_binding(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        }
        for (uint32_t binding = 9; binding <= 11; ++binding) {
            builder.add_binding(binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        }
        builder.add_binding(12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _surfaceCache.compareLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
        VkPushConstantRange compareRange{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(SurfaceCacheComparePushConstants)};
        VkPipelineLayoutCreateInfo compareLayoutInfo = vkinit::pipeline_layout_create_info();
        compareLayoutInfo.setLayoutCount = 1;
        compareLayoutInfo.pSetLayouts = &_surfaceCache.compareLayout;
        compareLayoutInfo.pushConstantRangeCount = 1;
        compareLayoutInfo.pPushConstantRanges = &compareRange;
        VK_CHECK(vkCreatePipelineLayout(
            _device, &compareLayoutInfo, nullptr, &_surfaceCache.comparePipelineLayout));
        ScopedShaderModule compareShader(_device);
        if (compareShader.load("../../shaders/surface_cache_compare.comp.spv")) {
            VkPipelineShaderStageCreateInfo stage{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = compareShader.get();
            stage.pName = "main";
            VkComputePipelineCreateInfo createInfo{
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            createInfo.layout = _surfaceCache.comparePipelineLayout;
            createInfo.stage = stage;
            VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
                &createInfo, nullptr, &_surfaceCache.comparePipeline));
        }
    }

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        for (uint32_t binding = 1; binding <= 6; ++binding) {
            builder.add_binding(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        }
        for (uint32_t binding = 7; binding <= 9; ++binding) {
            builder.add_binding(binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        }
        _surfaceCache.radiosityLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
        VkPushConstantRange range{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(SurfaceCacheRadiosityPushConstants)};
        // Set 0 is the frame's scene set, for the sky the misses read.
        const std::array<VkDescriptorSetLayout, 2> layouts{
            _gpuSceneDataDescriptorLayout, _surfaceCache.radiosityLayout};
        VkPipelineLayoutCreateInfo info = vkinit::pipeline_layout_create_info();
        info.setLayoutCount = static_cast<uint32_t>(layouts.size());
        info.pSetLayouts = layouts.data();
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;
        VK_CHECK(vkCreatePipelineLayout(
            _device, &info, nullptr, &_surfaceCache.radiosityPipelineLayout));
        ScopedShaderModule shader(_device);
        if (shader.load("../../shaders/surface_cache_radiosity.comp.spv")) {
            VkPipelineShaderStageCreateInfo stage{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = shader.get();
            stage.pName = "main";
            VkComputePipelineCreateInfo createInfo{
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            createInfo.layout = _surfaceCache.radiosityPipelineLayout;
            createInfo.stage = stage;
            VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
                &createInfo, nullptr, &_surfaceCache.radiosityPipeline));
        }
    }

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _surfaceCache.radiosityPipeline, nullptr);
        vkDestroyPipelineLayout(_device, _surfaceCache.radiosityPipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _surfaceCache.radiosityLayout, nullptr);
    });

    _mainDeletionQueue.push_function([this]() {
        for (AllocatedBuffer* buffer : {&_surfaceCache.cardBuffer,
                 &_surfaceCache.gridBuffer, &_surfaceCache.indexBuffer}) {
            if (buffer->buffer != VK_NULL_HANDLE) {
                destroy_buffer(*buffer);
            }
        }
        vkDestroyPipeline(_device, _surfaceCache.comparePipeline, nullptr);
        vkDestroyPipelineLayout(_device, _surfaceCache.comparePipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _surfaceCache.compareLayout, nullptr);
    });

    _mainDeletionQueue.push_function([this]() {
        for (AllocatedImage* image : {
                 &_surfaceCache.albedo, &_surfaceCache.normal,
                 &_surfaceCache.emissive, &_surfaceCache.depth,
                 &_surfaceCache.captureDepth, &_surfaceCache.direct,
                 &_surfaceCache.indirect}) {
            if (image->image != VK_NULL_HANDLE) {
                destroy_image(*image);
            }
        }
        vkDestroyPipeline(_device, _surfaceCache.directPipeline, nullptr);
        vkDestroyPipelineLayout(_device, _surfaceCache.directPipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _surfaceCache.directLayout, nullptr);
        vkDestroyPipeline(_device, _surfaceCache.capturePipeline, nullptr);
        vkDestroyPipeline(_device, _surfaceCache.debugPipeline, nullptr);
        vkDestroyPipelineLayout(_device, _surfaceCache.debugPipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _surfaceCache.debugLayout, nullptr);
    });

    if (const char* page = SDL_getenv("MIRABILIS_SURFACE_CACHE_PAGE")) {
        _surfaceCache.debugPage = std::clamp(std::atoi(page), 0, 12);
    }
    if (const char* radiosity = SDL_getenv("MIRABILIS_SURFACE_CACHE_RADIOSITY")) {
        _surfaceCache.radiosityEnabled = std::atoi(radiosity) != 0;
        _surfaceCache.singleBounce = std::atoi(radiosity) == 2;
    }
}

void VulkanEngine::update_surface_cache()
{
    if (_surfaceCache.capturePipeline == VK_NULL_HANDLE) {
        return;
    }
    std::vector<SceneOpaqueInstance> instances;
    const uint64_t drawHash = collect_opaque_instances(instances);
    if (drawHash == _surfaceCache.drawHash && !_surfaceCache.rebuildRequested) {
        if (_surfaceCache.valid) {
            light_surface_cache();
        }
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    _surfaceCache.drawHash = drawHash;
    _surfaceCache.rebuildRequested = false;
    _surfaceCache.valid = false;

    // Six cards per instance, sized in world texels.
    std::vector<SurfaceCard> cards;
    std::vector<SceneOpaqueInstance> kept;
    std::vector<bool> twoSided;
    for (SceneOpaqueInstance& instance : instances) {
        auto sourceIt = _traceMeshSources.find(instance.address);
        auto source = sourceIt == _traceMeshSources.end()
            ? nullptr : sourceIt->second.lock();
        if (!source) {
            continue;
        }
        InstanceBounds bounds = local_bounds(*source, instance);
        if (!bounds.valid()) {
            continue;
        }
        // A flat mesh has no depth range along its thin axis; pad every axis
        // a little so each card has a finite depth to map.
        const glm::vec3 extent = bounds.max - bounds.min;
        const float pad = std::max(0.01f * std::max({extent.x, extent.y, extent.z}), 1e-3f);
        bounds.min -= glm::vec3(pad);
        bounds.max += glm::vec3(pad);
        const glm::mat3 linear(instance.transform);
        const glm::vec3 axisScale(
            glm::length(linear[0]), glm::length(linear[1]), glm::length(linear[2]));
        const glm::vec3 worldThickness = extent * axisScale;
        twoSided.push_back(std::min({worldThickness.x, worldThickness.y, worldThickness.z}) <
            2.0f * _surfaceCache.targetTexelSize);
        const uint32_t instanceIndex = static_cast<uint32_t>(kept.size());
        for (int axis = 0; axis < 3; ++axis) {
            for (float sign : {1.0f, -1.0f}) {
                SurfaceCard card;
                card.instance = instanceIndex;
                card.axis = axis;
                card.sign = sign;
                card.localMin = bounds.min;
                card.localMax = bounds.max;
                set_card_projection(card);
                {
                    const int u = (axis + 1) % 3;
                    const int v = (axis + 2) % 3;
                    const glm::vec3 e = bounds.max - bounds.min;
                    glm::mat4 cardToLocal(0.0f);
                    glm::vec3 origin = bounds.min;
                    cardToLocal[0][u] = e[u];
                    cardToLocal[1][v] = e[v];
                    if (sign > 0.0f) {
                        cardToLocal[2][axis] = -e[axis];
                        origin[axis] = bounds.max[axis];
                    } else {
                        cardToLocal[2][axis] = e[axis];
                    }
                    cardToLocal[3] = glm::vec4(origin, 1.0f);
                    card.cardToWorld = instance.transform * cardToLocal;
                }
                const glm::vec3 worldExtent = (bounds.max - bounds.min) * axisScale;
                card.worldDepth = worldExtent[axis];
                // Width and height in world units for now; converted to
                // texels once the texel size is known.
                card.rect = glm::uvec4(0);
                cards.push_back(card);
            }
        }
        kept.push_back(std::move(instance));
    }

    // Pick the finest texel size that packs into the largest atlas.
    float texel = std::max(_surfaceCache.targetTexelSize, 1e-3f);
    uint32_t side = 0;
    for (int attempt = 0; attempt < 24; ++attempt) {
        double area = 0.0;
        for (SurfaceCard& card : cards) {
            const glm::mat3 linear(kept[card.instance].transform);
            const glm::vec3 axisScale(glm::length(linear[0]),
                glm::length(linear[1]), glm::length(linear[2]));
            const glm::vec3 worldExtent =
                (card.localMax - card.localMin) * axisScale;
            const int u = (card.axis + 1) % 3;
            const int v = (card.axis + 2) % 3;
            card.rect.z = std::clamp(uint32_t(std::ceil(worldExtent[u] / texel)),
                MinCardTexels, MaxCardTexels);
            card.rect.w = std::clamp(uint32_t(std::ceil(worldExtent[v] / texel)),
                MinCardTexels, MaxCardTexels);
            area += double(card.rect.z + Gutter) * double(card.rect.w + Gutter);
        }
        uint32_t candidate = 256;
        while (candidate < uint32_t(_surfaceCache.maxAtlasSize) &&
               double(candidate) * candidate < area * 1.15) {
            candidate *= 2;
        }
        if (pack_cards(cards, candidate)) {
            side = candidate;
            break;
        }
        texel *= 1.25f;
    }
    _surfaceCache.instances = std::move(kept);
    _surfaceCache.instanceTwoSided = std::move(twoSided);
    _surfaceCache.cards = std::move(cards);
    if (side == 0 || _surfaceCache.cards.empty()) {
        _surfaceCache.status = _surfaceCache.cards.empty()
            ? "No opaque instances to capture."
            : "Cards did not fit the largest atlas.";
        fmt::print("Surface cache: {}\n", _surfaceCache.status);
        return;
    }
    _surfaceCache.texelSize = texel;

    // Anything submitted earlier may still sample the old pages.
    VK_CHECK(vkDeviceWaitIdle(_device));
    if (_surfaceCache.atlasSize != glm::uvec2(side)) {
        for (AllocatedImage* image : {
                 &_surfaceCache.albedo, &_surfaceCache.normal,
                 &_surfaceCache.emissive, &_surfaceCache.depth,
                 &_surfaceCache.captureDepth, &_surfaceCache.direct,
                 &_surfaceCache.indirect}) {
            if (image->image != VK_NULL_HANDLE) {
                destroy_image(*image);
                *image = AllocatedImage{};
            }
        }
        const VkExtent3D extent{side, side, 1};
        constexpr VkImageUsageFlags pageUsage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        _surfaceCache.albedo = create_image(extent, AlbedoFormat, pageUsage);
        _surfaceCache.normal = create_image(extent, NormalFormat, pageUsage);
        _surfaceCache.emissive = create_image(extent, EmissiveFormat, pageUsage);
        _surfaceCache.depth = create_image(extent, DepthPageFormat, pageUsage);
        _surfaceCache.captureDepth = create_image(
            extent, CaptureDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        _surfaceCache.direct = create_image(extent, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        // Transfer destination too: a recapture clears it to zero.
        _surfaceCache.indirect = create_image(extent, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        immediate_submit([&](VkCommandBuffer cmd) {
            vkutil::transition_image(cmd, _surfaceCache.direct.image,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        _surfaceCache.atlasSize = glm::uvec2(side);
    }

    const std::array<AllocatedImage*, 4> pages{
        &_surfaceCache.albedo, &_surfaceCache.normal,
        &_surfaceCache.emissive, &_surfaceCache.depth};
    const VkExtent2D atlasExtent{side, side};

    // One pass per batch of cards, each loading what the last one wrote, so a
    // large scene's capture is split across submissions.
    constexpr size_t CardsPerBatch = 96;
    VkDescriptorSet sceneDescriptor = get_current_frame().sceneDescriptor;
    for (size_t first = 0; first < _surfaceCache.cards.size(); first += CardsPerBatch) {
        const size_t last = std::min(first + CardsPerBatch, _surfaceCache.cards.size());
        const bool clear = first == 0;
        immediate_submit([&](VkCommandBuffer cmd) {
            for (AllocatedImage* page : pages) {
                vkutil::transition_image(cmd, page->image,
                    clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            }
            if (clear) {
                vkutil::transition_image(cmd, _surfaceCache.captureDepth.image,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_ASPECT_DEPTH_BIT);
            }

            std::array<VkRenderingAttachmentInfo, 4> colors{};
            for (size_t i = 0; i < pages.size(); ++i) {
                colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                colors[i].imageView = pages[i]->imageView;
                colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colors[i].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            }
            // Depth page clears to 1: nothing captured.
            colors[3].clearValue.color.float32[0] = 1.0f;
            VkRenderingAttachmentInfo depth{
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            depth.imageView = _surfaceCache.captureDepth.imageView;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth.clearValue.depthStencil.depth = 1.0f;
            VkRenderingInfo renderInfo{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
            renderInfo.renderArea = VkRect2D{{0, 0}, atlasExtent};
            renderInfo.layerCount = 1;
            renderInfo.colorAttachmentCount = static_cast<uint32_t>(colors.size());
            renderInfo.pColorAttachments = colors.data();
            renderInfo.pDepthAttachment = &depth;
            vkCmdBeginRendering(cmd, &renderInfo);

            const VkPipelineLayout layout = metalRoughMaterial.opaquePipeline.layout;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                _surfaceCache.capturePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                0, 1, &sceneDescriptor, 0, nullptr);
            vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
            vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
            vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);

            for (size_t index = first; index < last; ++index) {
                const SurfaceCard& card = _surfaceCache.cards[index];
                const SceneOpaqueInstance& instance =
                    _surfaceCache.instances[card.instance];
                VkViewport viewport{
                    float(card.rect.x), float(card.rect.y),
                    float(card.rect.z), float(card.rect.w), 0.0f, 1.0f};
                VkRect2D scissor{
                    {int32_t(card.rect.x), int32_t(card.rect.y)},
                    {card.rect.z, card.rect.w}};
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                for (const RenderObject& draw : instance.draws) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        layout, 1, 1, &draw.material->materialSet, 0, nullptr);
                    vkCmdBindIndexBuffer(cmd, draw.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                    GPUDrawPushConstants push{};
                    push.worldMatrix = draw.transform;
                    push.vertexBuffer = draw.vertexBufferAddress;
                    push.alignmentPadding =
                        _surfaceCache.instanceTwoSided[card.instance] ? 1u : 0u;
                    push.previousWorldRow0 = card.row0;
                    push.previousWorldRow1 = card.row1;
                    push.previousWorldRow2 = card.row2;
                    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                        sizeof(push), &push);
                    vkCmdDrawIndexed(cmd, draw.indexCount, 1, draw.firstIndex, 0, 0);
                }
            }
            vkCmdEndRendering(cmd);
        });
    }
    immediate_submit([&](VkCommandBuffer cmd) {
        for (AllocatedImage* page : pages) {
            vkutil::transition_image(cmd, page->image,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });

    _surfaceCache.valid = true;
    _surfaceCache.captureMilliseconds = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    _surfaceCache.status = fmt::format(
        "{} instances, {} cards in a {}x{} atlas at {:.1f} cm texels, captured in {:.0f} ms",
        _surfaceCache.instances.size(), _surfaceCache.cards.size(), side, side,
        texel * 100.0f, _surfaceCache.captureMilliseconds);
    fmt::print("Surface cache: {}\n", _surfaceCache.status);

    _surfaceCache.lightingValid = false;
    _surfaceCache.lightingHash = 0;
    // A recapture moves cards, so bounce light starts again from zero.
    immediate_submit([&](VkCommandBuffer cmd) {
        vkutil::transition_image(cmd, _surfaceCache.indirect.image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        VkClearColorValue zero{};
        const VkImageSubresourceRange range =
            vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
        vkCmdClearColorImage(cmd, _surfaceCache.indirect.image,
            VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        vkutil::transition_image(cmd, _surfaceCache.indirect.image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    _surfaceCache.radiosityCursor = 0;
    _surfaceCache.radiosityUpdate = 0;
    _surfaceCache.cardUpdates.assign(_surfaceCache.cards.size(), 0);
    build_surface_cache_lookup();
    light_surface_cache();
}

void VulkanEngine::light_surface_cache()
{
    if (!_surfaceCache.valid || _surfaceCache.directPipeline == VK_NULL_HANDLE) {
        return;
    }
    // Without a scene field there is nothing to cast shadows with, and no
    // volume to bind; the cache stays unlit until the field exists.
    if (!_sceneSdf.fieldValid) {
        return;
    }
    const glm::vec3 sunDirection = normalized_sun_direction(_shadow.sunlightDirection);
    const glm::vec3 sunColor = glm::vec3(sceneData.sunlightColor);
    // The forward pass's own ambient scale: zero when the environment is
    // being suppressed, so the cache goes dark with it rather than lighting
    // a room the raster pass is deliberately keeping black.
    const float skyIntensity = sceneData.ssgiFallbackSettings.y > 0.5f
        ? 0.0f
        : sceneData.ssgiFallbackSettings.x;
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](const void* data, size_t bytes) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < bytes; ++i) {
            hash ^= p[i];
            hash *= 1099511628211ull;
        }
    };
    mix(&sunDirection, sizeof(sunDirection));
    mix(&sunColor, sizeof(sunColor));
    // A changed sky relights the cache: swapping the skybox otherwise leaves
    // every card carrying the previous one's light.
    mix(&skyIntensity, sizeof(skyIntensity));
    mix(_ibl.environmentSH.data(), sizeof(glm::vec4) * 9);
    mix(&_shadow.enabled, sizeof(_shadow.enabled));
    mix(&_sceneSdf.drawHash, sizeof(_sceneSdf.drawHash));
    mix(&_sceneSdf.fieldMin, sizeof(_sceneSdf.fieldMin));
    mix(&_sceneSdf.voxelSize, sizeof(_sceneSdf.voxelSize));
    mix(&_surfaceCache.drawHash, sizeof(_surfaceCache.drawHash));
    if (_surfaceCache.lightingValid && hash == _surfaceCache.lightingHash) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();

    std::array<DescriptorAllocatorGrowable::PoolSizeRatio, 3> ratios{{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1.0f},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3.0f},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1.0f}}};
    DescriptorAllocatorGrowable pool;
    pool.init(_device, 1, ratios);
    VkDescriptorSet set = pool.allocate(_device, _surfaceCache.directLayout);
    // The sky the cards are lit by, as the same band-2 irradiance the forward
    // pass reads.  Lighting is hash-gated, so this buffer is built only when
    // the cache is actually re-lit.
    AllocatedBuffer skyBuffer = create_buffer(
        sizeof(glm::vec4) * 9,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    std::memcpy(skyBuffer.info.pMappedData, _ibl.environmentSH.data(),
        sizeof(glm::vec4) * 9);
    DescriptorWriter writer;
    writer.write_image(0, _surfaceCache.direct.imageView, VK_NULL_HANDLE,
        VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    writer.write_image(1, _surfaceCache.normal.imageView, _prepass.sampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.write_image(2, _surfaceCache.depth.imageView, _prepass.sampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.write_image(3, _sceneSdf.field.imageView, _sdf.sampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.write_buffer(4, skyBuffer.buffer, sizeof(glm::vec4) * 9, 0,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    writer.update_set(_device, set);

    VK_CHECK(vkDeviceWaitIdle(_device));
    immediate_submit([&](VkCommandBuffer cmd) {
        vkutil::transition_image(cmd, _surfaceCache.direct.image,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    });

    // Every texel marches a shadow ray, so batches are bounded by texel count
    // to keep each submission well inside the driver's GPU timeout.
    constexpr uint64_t TexelBudget = 1'500'000;
    size_t next = 0;
    const auto& cards = _surfaceCache.cards;
    while (next < cards.size()) {
        const size_t first = next;
        uint64_t texels = 0;
        while (next < cards.size() && (next == first || texels < TexelBudget)) {
            texels += uint64_t(cards[next].rect.z) * cards[next].rect.w;
            ++next;
        }
        immediate_submit([&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _surfaceCache.directPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                _surfaceCache.directPipelineLayout, 0, 1, &set, 0, nullptr);
            for (size_t i = first; i < next; ++i) {
                const SurfaceCard& card = cards[i];
                SurfaceCacheDirectPushConstants push{};
                push.cardToWorld0 = glm::row(card.cardToWorld, 0);
                push.cardToWorld1 = glm::row(card.cardToWorld, 1);
                push.cardToWorld2 = glm::row(card.cardToWorld, 2);
                push.rect = card.rect;
                push.sunDirection = glm::vec4(sunDirection, _shadow.enabled ? 1.0f : 0.0f);
                push.sunColor = glm::vec4(sunColor, skyIntensity);
                push.fieldMin = glm::vec4(_sceneSdf.fieldMin, _sceneSdf.voxelSize);
                push.fieldMax = glm::vec4(_sceneSdf.fieldMax, 0.0f);
                vkCmdPushConstants(cmd, _surfaceCache.directPipelineLayout,
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
                vkCmdDispatch(cmd, (card.rect.z + 7) / 8, (card.rect.w + 7) / 8, 1);
            }
        });
    }
    immediate_submit([&](VkCommandBuffer cmd) {
        vkutil::transition_image(cmd, _surfaceCache.direct.image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    pool.destroy_pools(_device);
    destroy_buffer(skyBuffer);

    _surfaceCache.lightingHash = hash;
    _surfaceCache.lightingValid = true;
    _surfaceCache.lightingMilliseconds = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    fmt::print("Surface cache lit: {} cards in {:.0f} ms\n",
        cards.size(), _surfaceCache.lightingMilliseconds);
}

void VulkanEngine::build_surface_cache_lookup()
{
    _surfaceCache.lookupValid = false;
    for (AllocatedBuffer* buffer : {&_surfaceCache.cardBuffer,
             &_surfaceCache.gridBuffer, &_surfaceCache.indexBuffer}) {
        if (buffer->buffer != VK_NULL_HANDLE) {
            destroy_buffer(*buffer);
            *buffer = AllocatedBuffer{};
        }
    }
    const auto& cards = _surfaceCache.cards;
    if (cards.empty()) {
        return;
    }

    // A lookup point sits on a captured surface give or take the texel size
    // (G-buffer positions) or the field voxel (distance-field hits, whose
    // shells put the surface slightly in front of the real one).
    const float depthTolerance = std::max(
        3.0f * _surfaceCache.texelSize,
        _sceneSdf.fieldValid ? 1.5f * _sceneSdf.voxelSize : 0.0f);

    std::vector<SurfaceCacheGpuCard> gpuCards(cards.size());
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    std::vector<std::pair<glm::vec3, glm::vec3>> boxes(cards.size());
    for (size_t i = 0; i < cards.size(); ++i) {
        const SurfaceCard& card = cards[i];
        const glm::mat4 worldToCard = glm::inverse(card.cardToWorld);
        const glm::mat4& transform = _surfaceCache.instances[card.instance].transform;
        glm::vec3 facing(0.0f);
        facing[card.axis] = card.sign;
        const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));
        SurfaceCacheGpuCard& gpu = gpuCards[i];
        gpu.worldToCard0 = glm::row(worldToCard, 0);
        gpu.worldToCard1 = glm::row(worldToCard, 1);
        gpu.worldToCard2 = glm::row(worldToCard, 2);
        gpu.direction = glm::vec4(glm::normalize(normalMatrix * facing), card.worldDepth);
        gpu.rect = card.rect;

        glm::vec3 boxLo(std::numeric_limits<float>::max());
        glm::vec3 boxHi(std::numeric_limits<float>::lowest());
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 local(
                corner & 1 ? card.localMax.x : card.localMin.x,
                corner & 2 ? card.localMax.y : card.localMin.y,
                corner & 4 ? card.localMax.z : card.localMin.z);
            const glm::vec3 world = glm::vec3(transform * glm::vec4(local, 1.0f));
            boxLo = glm::min(boxLo, world);
            boxHi = glm::max(boxHi, world);
        }
        boxes[i] = {boxLo - glm::vec3(depthTolerance), boxHi + glm::vec3(depthTolerance)};
        lo = glm::min(lo, boxes[i].first);
        hi = glm::max(hi, boxes[i].second);
    }

    constexpr int MaxCells = 64;
    const glm::vec3 extent = hi - lo;
    const float cellSize = std::max(std::max({extent.x, extent.y, extent.z}) / 48.0f, 0.05f);
    const glm::ivec3 dims = glm::clamp(
        glm::ivec3(glm::ceil(extent / cellSize)), glm::ivec3(1), glm::ivec3(MaxCells));
    std::vector<std::vector<uint32_t>> cellCards(size_t(dims.x) * dims.y * dims.z);
    for (size_t i = 0; i < cards.size(); ++i) {
        const glm::ivec3 first = glm::clamp(
            glm::ivec3(glm::floor((boxes[i].first - lo) / cellSize)), glm::ivec3(0), dims - 1);
        const glm::ivec3 last = glm::clamp(
            glm::ivec3(glm::floor((boxes[i].second - lo) / cellSize)), glm::ivec3(0), dims - 1);
        for (int z = first.z; z <= last.z; ++z) {
            for (int y = first.y; y <= last.y; ++y) {
                for (int x = first.x; x <= last.x; ++x) {
                    cellCards[(size_t(z) * dims.y + y) * dims.x + x].push_back(uint32_t(i));
                }
            }
        }
    }
    std::vector<glm::uvec4> cellRanges(cellCards.size());
    std::vector<uint32_t> indices;
    size_t fullestCell = 0;
    for (size_t c = 0; c < cellCards.size(); ++c) {
        cellRanges[c] = glm::uvec4(uint32_t(indices.size()), uint32_t(cellCards[c].size()), 0, 0);
        indices.insert(indices.end(), cellCards[c].begin(), cellCards[c].end());
        fullestCell = std::max(fullestCell, cellCards[c].size());
    }
    if (indices.empty()) {
        indices.push_back(0);
    }

    SurfaceCacheGridHeader header;
    header.gridMin = glm::vec4(lo, cellSize);
    header.gridDimensions = glm::uvec4(glm::uvec3(dims), uint32_t(cards.size()));
    header.lookupParameters = glm::vec4(
        depthTolerance, 0.2f,
        // A query on the surface itself (a G-buffer position) differs from
        // the captured depth by the card's sampling: about a texel.
        _surfaceCache.texelSize,
        // The depth scale over which a card counts as a different surface
        // from the nearest: overlapping cards that captured one surface agree
        // to well within this.
        0.25f * _surfaceCache.texelSize);
    header.fieldMin = glm::vec4(_sceneSdf.fieldMin, _sceneSdf.voxelSize);
    header.fieldMax = glm::vec4(_sceneSdf.fieldMax, 0.0f);
    header.sunDirection = glm::vec4(normalized_sun_direction(_shadow.sunlightDirection), 0.0f);

    const auto upload = [&](const void* data, size_t bytes, AllocatedBuffer& buffer,
                            const void* prefix = nullptr, size_t prefixBytes = 0) {
        buffer = create_buffer(prefixBytes + bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU);
        if (prefix) {
            std::memcpy(buffer.info.pMappedData, prefix, prefixBytes);
        }
        std::memcpy(static_cast<char*>(buffer.info.pMappedData) + prefixBytes, data, bytes);
    };
    upload(gpuCards.data(), gpuCards.size() * sizeof(SurfaceCacheGpuCard), _surfaceCache.cardBuffer);
    upload(cellRanges.data(), cellRanges.size() * sizeof(glm::uvec4), _surfaceCache.gridBuffer,
        &header, sizeof(header));
    upload(indices.data(), indices.size() * sizeof(uint32_t), _surfaceCache.indexBuffer);
    _surfaceCache.lookupValid = true;
    fmt::print("Surface cache lookup: {}x{}x{} cells of {:.2f} m, {} entries, fullest cell {} cards, depth tolerance {:.1f} cm\n",
        dims.x, dims.y, dims.z, cellSize, indices.size(), fullestCell, depthTolerance * 100.0f);
}

bool VulkanEngine::lumen_lite_ready() const
{
    return _ssgi.lumenPipeline != VK_NULL_HANDLE && _sceneSdf.fieldValid &&
        _surfaceCache.valid && _surfaceCache.lightingValid && _surfaceCache.lookupValid;
}

void VulkanEngine::update_surface_cache_radiosity()
{
    if (!_surfaceCache.radiosityEnabled || !_surfaceCache.valid ||
        !_surfaceCache.lightingValid || !_surfaceCache.lookupValid ||
        !_sceneSdf.fieldValid || _surfaceCache.radiosityPipeline == VK_NULL_HANDLE ||
        _surfaceCache.cards.empty()) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto& cards = _surfaceCache.cards;

    std::array<DescriptorAllocatorGrowable::PoolSizeRatio, 3> ratios{{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1.0f},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6.0f},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3.0f}}};
    DescriptorAllocatorGrowable pool;
    pool.init(_device, 1, ratios);
    VkDescriptorSet set = pool.allocate(_device, _surfaceCache.radiosityLayout);
    DescriptorWriter writer;
    writer.write_image(0, _surfaceCache.indirect.imageView, VK_NULL_HANDLE,
        VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    const std::array<VkImageView, 5> pages{
        _surfaceCache.normal.imageView, _surfaceCache.depth.imageView,
        _surfaceCache.albedo.imageView, _surfaceCache.emissive.imageView,
        _surfaceCache.direct.imageView};
    for (uint32_t i = 0; i < pages.size(); ++i) {
        writer.write_image(i + 1, pages[i], _prepass.sampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    }
    writer.write_image(6, _sceneSdf.field.imageView, _sdf.sampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.write_buffer(7, _surfaceCache.cardBuffer.buffer, VK_WHOLE_SIZE, 0,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.write_buffer(8, _surfaceCache.gridBuffer.buffer, VK_WHOLE_SIZE, 0,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.write_buffer(9, _surfaceCache.indexBuffer.buffer, VK_WHOLE_SIZE, 0,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.update_set(_device, set);

    // The next cards in round-robin order, up to the texel budget, so a large
    // atlas converges over several updates instead of stalling one frame.
    std::vector<uint32_t> batch;
    uint64_t texels = 0;
    const uint32_t cardCount = static_cast<uint32_t>(cards.size());
    for (uint32_t n = 0; n < cardCount; ++n) {
        const uint32_t index = (_surfaceCache.radiosityCursor + n) % cardCount;
        const uint64_t cardTexels = uint64_t(cards[index].rect.z) * cards[index].rect.w;
        if (!batch.empty() && texels + cardTexels > _surfaceCache.radiosityTexelBudget) {
            break;
        }
        batch.push_back(index);
        texels += cardTexels;
    }
    _surfaceCache.radiosityCursor =
        (_surfaceCache.radiosityCursor + static_cast<uint32_t>(batch.size())) % cardCount;
    const uint32_t update = _surfaceCache.radiosityUpdate++;

    const VkDescriptorSet sceneSet = get_current_frame().sceneDescriptor;
    immediate_submit([&](VkCommandBuffer cmd) {
        vkutil::transition_image(cmd, _surfaceCache.indirect.image,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _surfaceCache.radiosityPipeline);
        const std::array<VkDescriptorSet, 2> sets{sceneSet, set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            _surfaceCache.radiosityPipelineLayout, 0, 2, sets.data(), 0, nullptr);
        for (uint32_t index : batch) {
            const SurfaceCard& card = cards[index];
            SurfaceCacheRadiosityPushConstants push{};
            push.cardToWorld0 = glm::row(card.cardToWorld, 0);
            push.cardToWorld1 = glm::row(card.cardToWorld, 1);
            push.cardToWorld2 = glm::row(card.cardToWorld, 2);
            push.rect = card.rect;
            push.control = glm::uvec4(update, _surfaceCache.raysPerTexel,
                _surfaceCache.singleBounce ? 1u : 0u, 0);
            uint32_t& updates = _surfaceCache.cardUpdates[index];
            const float blend = std::max(
                1.0f / float(updates + 1), _surfaceCache.radiosityBlend);
            ++updates;
            push.settings = glm::vec4(
                blend, _surfaceCache.radiosityMaxDistance, 0.0f, 0.0f);
            vkCmdPushConstants(cmd, _surfaceCache.radiosityPipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, (card.rect.z + 7) / 8, (card.rect.w + 7) / 8, 1);
        }
        vkutil::transition_image(cmd, _surfaceCache.indirect.image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    pool.destroy_pools(_device);
    _surfaceCache.radiosityMilliseconds = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - started).count();
}

void VulkanEngine::draw_surface_cache_debug(VkCommandBuffer cmd)
{
    if (!_surfaceCache.valid || _surfaceCache.debugPipeline == VK_NULL_HANDLE) {
        return;
    }
    if (_surfaceCache.debugPage >= 6 && _surfaceCache.debugPage <= 10) {
        if (!_surfaceCache.lookupValid || !_surfaceCache.lightingValid ||
            !_sceneSdf.fieldValid || _surfaceCache.comparePipeline == VK_NULL_HANDLE) {
            return;
        }
        VkDescriptorSet set = get_current_frame()._frameDescriptors.allocate(
            _device, _surfaceCache.compareLayout);
        DescriptorWriter writer;
        writer.write_image(0, _drawImage.imageView, VK_NULL_HANDLE,
            VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        const std::array<VkImageView, 8> views{
            _prepass.depthImage.imageView, _prepass.normalImage.imageView,
            _sceneTargets.gbufferAlbedo.imageView, _sceneTargets.directLighting.imageView,
            _surfaceCache.albedo.imageView, _surfaceCache.emissive.imageView,
            _surfaceCache.depth.imageView, _surfaceCache.direct.imageView};
        for (uint32_t i = 0; i < views.size(); ++i) {
            writer.write_image(i + 1, views[i], _prepass.sampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        }
        writer.write_buffer(9, _surfaceCache.cardBuffer.buffer, VK_WHOLE_SIZE, 0,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.write_buffer(10, _surfaceCache.gridBuffer.buffer, VK_WHOLE_SIZE, 0,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.write_buffer(11, _surfaceCache.indexBuffer.buffer, VK_WHOLE_SIZE, 0,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.write_image(12, _sceneSdf.field.imageView, _sdf.sampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writer.write_image(13, _surfaceCache.indirect.imageView, _prepass.sampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writer.update_set(_device, set);

        SurfaceCacheComparePushConstants push{};
        push.inverseViewProjection = glm::inverse(sceneData.viewproj);
        const glm::mat3 viewToWorld = glm::inverse(glm::mat3(sceneData.view));
        push.viewToWorld0 = glm::vec4(glm::row(viewToWorld, 0), 0.0f);
        push.viewToWorld1 = glm::vec4(glm::row(viewToWorld, 1), 0.0f);
        push.viewToWorld2 = glm::vec4(glm::row(viewToWorld, 2), 0.0f);
        push.settings = glm::vec4(float(_drawExtent.width), float(_drawExtent.height),
            float(_surfaceCache.debugPage - 6), 0.0f);
        vkutil::transition_image(cmd, _drawImage.image,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _surfaceCache.comparePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            _surfaceCache.comparePipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, _surfaceCache.comparePipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (_drawExtent.width + 15) / 16, (_drawExtent.height + 15) / 16, 1);
        vkutil::transition_image(cmd, _drawImage.image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        return;
    }
    VkDescriptorSet set = get_current_frame()._frameDescriptors.allocate(
        _device, _surfaceCache.debugLayout);
    DescriptorWriter writer;
    writer.write_image(0, _drawImage.imageView, VK_NULL_HANDLE,
        VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    const std::array<const AllocatedImage*, 6> pages{
        &_surfaceCache.albedo, &_surfaceCache.normal,
        &_surfaceCache.emissive, &_surfaceCache.depth, &_surfaceCache.direct,
        &_surfaceCache.indirect};
    for (uint32_t i = 0; i < pages.size(); ++i) {
        writer.write_image(i + 1, pages[i]->imageView, _prepass.sampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    }
    writer.update_set(_device, set);

    const glm::vec4 settings(float(_drawExtent.width), float(_drawExtent.height),
        float(_surfaceCache.debugPage), 0.0f);
    vkutil::transition_image(cmd, _drawImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _surfaceCache.debugPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        _surfaceCache.debugPipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, _surfaceCache.debugPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    vkCmdDispatch(cmd, (_drawExtent.width + 15) / 16, (_drawExtent.height + 15) / 16, 1);
    vkutil::transition_image(cmd, _drawImage.image,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

void VulkanEngine::draw_surface_cache_settings()
{
    ImGui::SeparatorText("Lumen-lite surface cache");
    ImGui::TextWrapped("%s", _surfaceCache.status.c_str());
    const char* pages[] = {"Albedo", "Normal", "Emissive", "Depth",
        "Direct light", "Lit (albedo x direct + emissive)",
        "Screen: cache lit at G-buffer", "Screen: lit/shadow vs raster",
        "Screen: comparison data", "Screen: albedo data",
        "Screen: blocked sun ray hit positions",
        "Indirect light", "Lit with indirect"};
    if (!_sceneSdf.fieldValid) {
        ImGui::TextDisabled("Lighting needs the scene distance field (bake it first).");
    }
    ImGui::Combo("Atlas Page", &_surfaceCache.debugPage, pages, IM_ARRAYSIZE(pages));
    ImGui::SliderFloat("Target Texel (m)", &_surfaceCache.targetTexelSize,
        0.01f, 0.5f, "%.3f");
    ImGui::Checkbox("Radiosity", &_surfaceCache.radiosityEnabled);
    int budget = static_cast<int>(_surfaceCache.radiosityTexelBudget);
    if (ImGui::SliderInt("Texels Per Update", &budget, 10000, 2000000)) {
        _surfaceCache.radiosityTexelBudget = static_cast<uint32_t>(budget);
    }
    int rays = static_cast<int>(_surfaceCache.raysPerTexel);
    if (ImGui::SliderInt("Rays Per Texel", &rays, 1, 8)) {
        _surfaceCache.raysPerTexel = static_cast<uint32_t>(rays);
    }
    ImGui::SliderFloat("Radiosity Blend Floor", &_surfaceCache.radiosityBlend, 0.005f, 1.0f, "%.3f");
    ImGui::TextDisabled("Radiosity update %u: %.1f ms", _surfaceCache.radiosityUpdate,
        _surfaceCache.radiosityMilliseconds);
    if (ImGui::Button("Recapture")) {
        _surfaceCache.rebuildRequested = true;
    }
}
