#include "vk_engine.h"
#include "vk_engine_render_helpers.h"
#include "r4_contributions.h"

#include <algorithm>
#include <numeric>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <unordered_map>

#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/packing.hpp>

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

// R4 diagnostics (R4.11, R4.15, R4.16): CPU copies of what the GPU samples.
float half_at(const std::vector<uint8_t>& bytes, size_t offset)
{
    uint16_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return glm::unpackHalf1x16(value);
}

// The scene field read back, sampled as textureLod does with VK_FILTER_LINEAR
// and CLAMP_TO_EDGE.
struct FieldCopy {
    glm::uvec3 dim{0};
    glm::vec3 min{0.0f};
    glm::vec3 size{1.0f};
    std::vector<uint8_t> bytes;

    float voxel(int x, int y, int z) const
    {
        x = std::clamp(x, 0, int(dim.x) - 1);
        y = std::clamp(y, 0, int(dim.y) - 1);
        z = std::clamp(z, 0, int(dim.z) - 1);
        float value = 0.0f;
        std::memcpy(&value, bytes.data() + ((size_t(z) * dim.y + y) * dim.x + x) * 4, 4);
        return value;
    }

    float distance(glm::vec3 world) const
    {
        const glm::vec3 p = (world - min) / size * glm::vec3(dim) - 0.5f;
        const glm::vec3 base = glm::floor(p);
        const glm::vec3 f = p - base;
        const glm::ivec3 i(base);
        float result = 0.0f;
        for (int c = 0; c < 8; ++c) {
            const glm::ivec3 o(c & 1, (c >> 1) & 1, (c >> 2) & 1);
            const float w = (o.x ? f.x : 1.0f - f.x) * (o.y ? f.y : 1.0f - f.y) *
                (o.z ? f.z : 1.0f - f.z);
            result += w * voxel(i.x + o.x, i.y + o.y, i.z + o.z);
        }
        return result;
    }
};

// emitter_hash() and emitter_stratum()'s bit reversal, as the shaders do them.
uint32_t diagnostic_hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

uint32_t reverse_bits(uint32_t v)
{
    uint32_t r = 0;
    for (int b = 0; b < 32; ++b) { r = (r << 1) | (v & 1u); v >>= 1; }
    return r;
}

glm::vec3 diagnostic_rotation(uint32_t seed)
{
    const uint32_t h = diagnostic_hash(seed);
    return glm::vec3(float(h & 1023u), float((h >> 10) & 1023u), float((h >> 20) & 1023u)) / 1024.0f;
}

glm::vec3 diagnostic_stratum(uint32_t i, uint32_t n, glm::vec3 rotation)
{
    return glm::fract(glm::vec3(float(i) * 0.6180339887f, (float(i) + 0.5f) / float(n),
        float(reverse_bits(i)) * 2.3283064365386963e-10f) + rotation);
}

// Distance from p to a triangle (Ericson's closest point), for naming the
// surface that stopped a field march.
float triangle_distance(glm::vec3 p, glm::vec3 a, glm::vec3 b, glm::vec3 c)
{
    const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return glm::length(p - a);
    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return glm::length(p - b);
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return glm::length(p - (a + ab * (d1 / (d1 - d3))));
    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return glm::length(p - c);
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return glm::length(p - (a + ac * (d2 / (d2 - d6))));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
        return glm::length(p - (b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)))));
    const float denom = 1.0f / (va + vb + vc);
    return glm::length(p - (a + ab * (vb * denom) + ac * (vc * denom)));
}

int nearest_triangle(const std::vector<TraceTriangle>& triangles, glm::vec3 p, float& distance)
{
    int nearest = -1;
    distance = std::numeric_limits<float>::max();
    for (size_t k = 0; k < triangles.size(); ++k) {
        const TraceTriangle& t = triangles[k];
        const float d = triangle_distance(p, glm::vec3(t.p0), glm::vec3(t.p1), glm::vec3(t.p2));
        if (d < distance) { distance = d; nearest = int(k); }
    }
    return nearest;
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
        VkPipelineRasterizationConservativeStateCreateInfoEXT conservative{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT,
            .conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT};
        if (_conservativeRasterSupported &&
            SDL_getenv("MIRABILIS_R4_CONSERVATIVE_CARDS") != nullptr) {
            builder._rasterizer.pNext = &conservative;
            fmt::print("Surface cache: conservative card capture\n");
        }
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
        // The path tracer's triangles, materials and emitter list (R4.7).
        builder.add_binding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        builder.add_binding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        builder.add_binding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        // The emitter page (R4.12).
        builder.add_binding(8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        // The panorama, for the hemispherical sky (R4.14).
        builder.add_binding(9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        // Existing trace-scene BVH, queried only during cache relight (R4.21).
        builder.add_binding(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        builder.add_binding(11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        // IQ8: the sun casters' TLAS, which replaces that BVH on the ray-query device.
        if (_rayQueryShadows) {
            builder.add_binding(12, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);
        }
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
        if (directShader.load(ray_query_shader("surface_cache_direct.comp").c_str())) {
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
        builder.add_binding(10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
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
        VkQueryPoolCreateInfo query{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query.queryCount = 2 * FRAME_OVERLAP;
        VK_CHECK(vkCreateQueryPool(_device, &query, nullptr, &_surfaceCache.radiosityTimestampPool));
    }

    _mainDeletionQueue.push_function([this]() {
        vkDestroyQueryPool(_device, _surfaceCache.radiosityTimestampPool, nullptr);
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
                 &_surfaceCache.emitter, &_surfaceCache.sky,
                 &_surfaceCache.indirect, &_surfaceCache.indirectPrevious,
                 &_surfaceCache.albedoVolume}) {
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
        _surfaceCache.debugPage = std::clamp(std::atoi(page), 0, 13);
    }
    if (SDL_getenv("MIRABILIS_SURFACE_CACHE_COVERAGE")) {
        _surfaceCache.measureCoverage = true;
    }
    if (const char* radiosity = SDL_getenv("MIRABILIS_SURFACE_CACHE_RADIOSITY")) {
        _surfaceCache.radiosityEnabled = std::atoi(radiosity) != 0;
        _surfaceCache.singleBounce = std::atoi(radiosity) == 2;
    }
    if (SDL_getenv("MIRABILIS_SURFACE_CACHE_SHADOW_CHECK")) {
        _surfaceCache.measureShadows = true;
    }
    if (SDL_getenv("MIRABILIS_R4_EMITTER_VISIBILITY_CHECK")) {
        _surfaceCache.measureEmitterVisibility = true;
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
                 &_surfaceCache.emitter, &_surfaceCache.sky,
                 &_surfaceCache.indirect, &_surfaceCache.indirectPrevious}) {
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
        _surfaceCache.emitter = create_image(extent, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        _surfaceCache.sky = create_image(extent, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        // Transfer destination too: a recapture clears it to zero.
        _surfaceCache.indirect = create_image(extent, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        _surfaceCache.indirectPrevious = create_image(extent, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        immediate_submit([&](VkCommandBuffer cmd) {
            vkutil::transition_image(cmd, _surfaceCache.direct.image,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            vkutil::transition_image(cmd, _surfaceCache.emitter.image,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            vkutil::transition_image(cmd, _surfaceCache.sky.image,
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
    // A recapture moves cards, so bounce light starts again from zero, in
    // both pages: radiosity relies on them being equal between updates.
    immediate_submit([&](VkCommandBuffer cmd) {
        VkClearColorValue zero{};
        const VkImageSubresourceRange range =
            vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
        for (VkImage page : {_surfaceCache.indirect.image, _surfaceCache.indirectPrevious.image}) {
            vkutil::transition_image(cmd, page,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            vkCmdClearColorImage(cmd, page, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
            vkutil::transition_image(cmd, page,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });
    _surfaceCache.radiosityCursor = 0;
    _surfaceCache.radiosityUpdate = 0;
    _surfaceCache.cardUpdates.assign(_surfaceCache.cards.size(), 0);
    build_surface_cache_lookup();
    light_surface_cache();

    if (_surfaceCache.measureCoverage) {
        measure_surface_cache_coverage();
    }
    if (_surfaceCache.measureShadows) {
        measure_surface_cache_shadows();
    }
    if (const char* sunCheck = SDL_getenv("MIRABILIS_R4_CACHE_SUN_CHECK")) {
        measure_cache_sun_visibility(sunCheck);
    }
    if (_surfaceCache.measureEmitterVisibility) {
        measure_emitter_visibility();
    }
    if (const char* skyCheck = SDL_getenv("MIRABILIS_R4_SKY_VISIBILITY_CHECK")) {
        measure_sky_visibility(skyCheck);
    }
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
    // Emitters are direct light too (R4.7), sampled from the path tracer's own
    // scene buffers so the cache and the reference agree on geometry,
    // emission and sidedness.  Without the software tracer those buffers do
    // not exist, and that is said rather than quietly lit without emitters.
    if (_traceSupported) {
        update_trace_scene();
    }
    update_albedo_volume();
    const uint32_t emitterCount = _traceSupported
        ? static_cast<uint32_t>(_traceEmitters.size()) : 0u;
    // The sky source follows the same selection as every other path (R3.1).
    const uint32_t panoramaSky = _ssgi.traceEnvironmentMap ? 1u : 0u;
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
    mix(&panoramaSky, sizeof(panoramaSky));
    // Sky directions per texel: 32 through the software BVH; 256 by ray query
    // (IQ8), where they cost about what 32 did; MIRABILIS_IQ5_CACHE_SKY_SAMPLES
    // overrides either.  z = 0 keeps the shader's own 32 and the lighting hash.
    const char* skySamplesText = SDL_getenv("MIRABILIS_IQ5_CACHE_SKY_SAMPLES");
    const uint32_t skySamples = skySamplesText
        ? std::clamp(std::atoi(skySamplesText), 1, 1024) : (_rayQueryShadows ? 256 : 32);
    // IQ7 diagnostic: a second rotation seed for the sky directions, so two
    // relights differ only in their per-texel noise (0 = unchanged).
    const char* skySeedText = SDL_getenv("MIRABILIS_IQ7_SKY_SEED");
    const glm::vec4 skyEnvironment(_skyboxEnvironmentLod, _skyboxIndirectClamp,
        skySamplesText || _rayQueryShadows ? float(skySamples) : 0.0f,
        skySeedText ? float(std::clamp(std::atoi(skySeedText), 0, 65535)) : 0.0f);
    mix(&skyEnvironment, sizeof(skyEnvironment));
    mix(&_r4Contributions.cacheSources, sizeof(_r4Contributions.cacheSources));
    mix(&emitterCount, sizeof(emitterCount));
    mix(&_traceSceneHash, sizeof(_traceSceneHash));
    mix(&_shadow.enabled, sizeof(_shadow.enabled));
    mix(&_sceneSdf.drawHash, sizeof(_sceneSdf.drawHash));
    mix(&_sceneSdf.fieldMin, sizeof(_sceneSdf.fieldMin));
    mix(&_sceneSdf.voxelSize, sizeof(_sceneSdf.voxelSize));
    mix(&_surfaceCache.drawHash, sizeof(_surfaceCache.drawHash));
    if (_surfaceCache.lightingValid && hash == _surfaceCache.lightingHash) {
        return;
    }
    if (!_traceSupported) {
        fmt::print("Surface cache: emissive triangles unsampled (software trace unavailable)\n");
    }
    const auto started = std::chrono::steady_clock::now();

    // Bindings 5, 6, 7 and 10 are storage buffers; 12 is the TLAS (IQ8).
    std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> ratios{{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3.0f},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4.0f},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1.0f},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4.0f}}};
    if (_rayQueryShadows) {
        ratios.push_back({VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1.0f});
    }
    DescriptorAllocatorGrowable pool;
    pool.init(_device, 1, ratios);
    VkDescriptorSet set = pool.allocate(_device, _surfaceCache.directLayout);
    // The sky the cards are lit by, as the same band-2 irradiance the forward
    // pass reads.  Lighting is hash-gated, so this buffer is built only when
    // the cache is actually re-lit.
    AllocatedBuffer skyBuffer = create_buffer(
        sizeof(glm::vec4) * 11,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    std::memcpy(skyBuffer.info.pMappedData, _ibl.environmentSH.data(),
        sizeof(glm::vec4) * 9);
    const glm::uvec4 skySettings(panoramaSky, emitterCount, _r4Contributions.cacheSources,
        _traceSupported && !_traceNodes.empty() ? 1u : 0u);
    std::memcpy(static_cast<char*>(skyBuffer.info.pMappedData) + sizeof(glm::vec4) * 9,
        &skySettings, sizeof(skySettings));
    std::memcpy(static_cast<char*>(skyBuffer.info.pMappedData) + sizeof(glm::vec4) * 10,
        &skyEnvironment, sizeof(skyEnvironment));
    // Bound even when unused, so the descriptor set is always complete.
    AllocatedBuffer placeholder = create_buffer(sizeof(glm::vec4) * 16,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    const auto traceBuffer = [&](const AllocatedBuffer& buffer) {
        return _traceSupported && buffer.buffer != VK_NULL_HANDLE ? buffer.buffer : placeholder.buffer;
    };
    DescriptorWriter writer;
    writer.write_image(0, _surfaceCache.direct.imageView, VK_NULL_HANDLE,
        VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    writer.write_image(1, _surfaceCache.normal.imageView, _prepass.sampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.write_image(2, _surfaceCache.depth.imageView, _prepass.sampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.write_image(3, _sceneSdf.field.imageView, _sdf.sampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.write_buffer(4, skyBuffer.buffer, sizeof(glm::vec4) * 11, 0,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    writer.write_buffer(5, traceBuffer(_traceTriangleBuffer), VK_WHOLE_SIZE, 0,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.write_buffer(6, traceBuffer(_traceMaterialBuffer), VK_WHOLE_SIZE, 0,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.write_buffer(7, traceBuffer(_traceEmitterBuffer), VK_WHOLE_SIZE, 0,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.write_image(8, _surfaceCache.emitter.imageView, VK_NULL_HANDLE,
        VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    writer.write_image(9,
        _skyboxImage.imageView != VK_NULL_HANDLE ? _skyboxImage.imageView : _whiteImage.imageView,
        _skyboxImage.imageView != VK_NULL_HANDLE ? _skyboxSampler : _prepass.sampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.write_buffer(10, traceBuffer(_traceNodeBuffer), VK_WHOLE_SIZE, 0,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.write_image(11, _surfaceCache.sky.imageView, VK_NULL_HANDLE,
        VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    if (_rayQueryShadows) {
        writer.write_acceleration_structure(12, _sunCasters.tlas);
    }
    writer.update_set(_device, set);

    VK_CHECK(vkDeviceWaitIdle(_device));
    immediate_submit([&](VkCommandBuffer cmd) {
        vkutil::transition_image(cmd, _surfaceCache.direct.image,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(cmd, _surfaceCache.emitter.image,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(cmd, _surfaceCache.sky.image,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    });

    // Every texel marches shadow rays, so batches are bounded by texel count
    // to keep each submission well inside the driver's GPU timeout.  The
    // budget was sized for two marches; the hemispherical sky takes 32 and
    // emitter sampling 64 more, and it shrinks by the same factor (R4.14).
    const uint64_t TexelBudget = 1'500'000ull * 2 / (2 + skySamples + (emitterCount > 0 ? 64 : 0));
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
        vkutil::transition_image(cmd, _surfaceCache.emitter.image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkutil::transition_image(cmd, _surfaceCache.sky.image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    pool.destroy_pools(_device);
    destroy_buffer(skyBuffer);
    destroy_buffer(placeholder);

    _surfaceCache.lightingHash = hash;
    _surfaceCache.lightingValid = true;
    _surfaceCache.lightingMilliseconds = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    fmt::print("Surface cache lit: {} cards, {} emitter triangles, {} sky in {:.0f} ms\n",
        cards.size(), emitterCount, panoramaSky ? "panorama" : "gradient",
        _surfaceCache.lightingMilliseconds);
}

void VulkanEngine::measure_surface_cache_coverage()
{
    if (!_surfaceCache.valid) {
        return;
    }
    const glm::uvec2 size = _surfaceCache.atlasSize;
    const size_t texels = size_t(size.x) * size.y;

    VK_CHECK(vkDeviceWaitIdle(_device));
    AllocatedBuffer readback = create_buffer(texels * sizeof(uint16_t),
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
    immediate_submit([&](VkCommandBuffer cmd) {
        vkutil::transition_image(cmd, _surfaceCache.depth.image,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {size.x, size.y, 1};
        vkCmdCopyImageToBuffer(cmd, _surfaceCache.depth.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
        vkutil::transition_image(cmd, _surfaceCache.depth.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    void* mapped = nullptr;
    VK_CHECK(vmaMapMemory(_allocator, readback.allocation, &mapped));
    vmaInvalidateAllocation(_allocator, readback.allocation, 0, VK_WHOLE_SIZE);
    const auto* packed = static_cast<const uint16_t*>(mapped);
    const auto depthAt = [&](uint32_t x, uint32_t y) {
        return glm::unpackHalf1x16(packed[size_t(y) * size.x + x]);
    };

    // Sample points uniformly over each instance's opaque surface area and
    // ask whether any card facing that point recorded a surface at its depth.
    std::mt19937 generator(0xCA4D5EEDu);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    struct InstanceCoverage {
        size_t instance;
        double area;
        double covered;
    };
    std::vector<InstanceCoverage> results;
    double totalArea = 0.0;
    double totalCovered = 0.0;
    std::vector<std::vector<size_t>> cardsByInstance(_surfaceCache.instances.size());
    for (size_t i = 0; i < _surfaceCache.cards.size(); ++i) {
        cardsByInstance[_surfaceCache.cards[i].instance].push_back(i);
    }
    constexpr int SamplesPerTriangleCap = 64;
    for (size_t instanceIndex = 0; instanceIndex < _surfaceCache.instances.size(); ++instanceIndex) {
        const SceneOpaqueInstance& instance = _surfaceCache.instances[instanceIndex];
        auto sourceIt = _traceMeshSources.find(instance.address);
        auto source = sourceIt == _traceMeshSources.end() ? nullptr : sourceIt->second.lock();
        if (!source) {
            continue;
        }
        const glm::mat3 linear(instance.transform);
        double instanceArea = 0.0;
        double instanceCovered = 0.0;
        for (const RenderObject& draw : instance.draws) {
            const size_t end = std::min(
                source->indices.size(), size_t(draw.firstIndex) + draw.indexCount);
            for (size_t t = draw.firstIndex; t + 2 < end; t += 3) {
                const uint32_t ia = source->indices[t];
                const uint32_t ib = source->indices[t + 1];
                const uint32_t ic = source->indices[t + 2];
                if (std::max({ia, ib, ic}) >= source->vertices.size()) {
                    continue;
                }
                const glm::vec3 a = source->vertices[ia].position;
                const glm::vec3 b = source->vertices[ib].position;
                const glm::vec3 c = source->vertices[ic].position;
                const glm::vec3 localCross = glm::cross(b - a, c - a);
                const float worldArea = 0.5f * glm::length(
                    glm::cross(linear * (b - a), linear * (c - a)));
                if (worldArea <= 0.0f || glm::dot(localCross, localCross) <= 0.0f) {
                    continue;
                }
                // Vertex normals decide which cards may see the point, the
                // same test the capture shader applies.
                const glm::vec3 normal =
                    source->vertices[ia].normal + source->vertices[ib].normal +
                    source->vertices[ic].normal;
                // One sample per texel's worth of area, at least one.
                const float texelArea = _surfaceCache.texelSize * _surfaceCache.texelSize;
                const int samples = std::clamp(
                    int(std::ceil(worldArea / texelArea)), 1, SamplesPerTriangleCap);
                const double perSample = double(worldArea) / samples;
                for (int s = 0; s < samples; ++s) {
                    float r1 = unit(generator);
                    float r2 = unit(generator);
                    if (r1 + r2 > 1.0f) {
                        r1 = 1.0f - r1;
                        r2 = 1.0f - r2;
                    }
                    const glm::vec3 p = a + (b - a) * r1 + (c - a) * r2;
                    bool covered = false;
                    for (size_t cardIndex : cardsByInstance[instanceIndex]) {
                        const SurfaceCard& card = _surfaceCache.cards[cardIndex];
                        glm::vec3 facingAxis(0.0f);
                        facingAxis[card.axis] = card.sign;
                        const float facing = glm::dot(normal, facingAxis);
                        if (facing <= 0.0f &&
                            !(_surfaceCache.instanceTwoSided[instanceIndex] && facing < 0.0f)) {
                            continue;
                        }
                        const glm::vec3 clip = apply_rows(card, p);
                        const float fx = (clip.x * 0.5f + 0.5f) * card.rect.z;
                        const float fy = (clip.y * 0.5f + 0.5f) * card.rect.w;
                        const uint32_t tx = card.rect.x + std::min(
                            uint32_t(std::max(fx, 0.0f)), card.rect.z - 1);
                        const uint32_t ty = card.rect.y + std::min(
                            uint32_t(std::max(fy, 0.0f)), card.rect.w - 1);
                        const float stored = depthAt(tx, ty);
                        // Two texels of world depth, in the card's 0..1 units.
                        const float tolerance = std::max(
                            2.0f * _surfaceCache.texelSize / std::max(card.worldDepth, 1e-4f),
                            0.002f);
                        if (stored < 1.0f && std::abs(stored - clip.z) <= tolerance) {
                            covered = true;
                            break;
                        }
                    }
                    instanceArea += perSample;
                    if (covered) {
                        instanceCovered += perSample;
                    }
                }
            }
        }
        results.push_back({instanceIndex, instanceArea, instanceCovered});
        totalArea += instanceArea;
        totalCovered += instanceCovered;
    }
    vmaUnmapMemory(_allocator, readback.allocation);
    destroy_buffer(readback);

    std::sort(results.begin(), results.end(), [](const auto& l, const auto& r) {
        // Largest uncovered area first: that is where the lighting will be
        // missing the most.
        return (l.area - l.covered) > (r.area - r.covered);
    });
    fmt::print("Surface cache coverage: {:.1f}% of {:.1f} m^2 opaque surface\n",
        totalArea > 0.0 ? totalCovered / totalArea * 100.0 : 0.0, totalArea);
    for (size_t i = 0; i < std::min<size_t>(results.size(), 8); ++i) {
        const auto& r = results[i];
        const SceneOpaqueInstance& instance = _surfaceCache.instances[r.instance];
        size_t triangles = 0;
        for (const RenderObject& draw : instance.draws) {
            triangles += draw.indexCount / 3;
        }
        fmt::print("  instance {:3d}: {:6.1f}% of {:7.2f} m^2 covered, {:.2f} m^2 missing, {} triangles\n",
            r.instance, r.area > 0.0 ? r.covered / r.area * 100.0 : 0.0, r.area,
            r.area - r.covered, triangles);
    }
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
        _surfaceCache.valid && _surfaceCache.lightingValid && _surfaceCache.lookupValid &&
        _surfaceCache.albedoVolume.image != VK_NULL_HANDLE;
}

void VulkanEngine::update_surface_cache_radiosity(VkCommandBuffer cmd)
{
    // This slot's fence has passed, so the timing it last wrote is final.
    const uint32_t slot = _frameNumber % FRAME_OVERLAP;
    if (_surfaceCache.radiosityTimingWritten[slot]) {
        _surfaceCache.radiosityTimingWritten[slot] = false;
        std::array<uint64_t, 2> ticks{};
        if (vkGetQueryPoolResults(_device, _surfaceCache.radiosityTimestampPool, slot * 2, 2,
                sizeof(ticks), ticks.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            _surfaceCache.radiosityMilliseconds =
                float(ticks[1] - ticks[0]) * _gpuTiming.timestampPeriod * 1e-6f;
        }
    }
    if (!_surfaceCache.radiosityEnabled || !_surfaceCache.valid ||
        !_surfaceCache.lightingValid || !_surfaceCache.lookupValid ||
        !_sceneSdf.fieldValid || _surfaceCache.radiosityPipeline == VK_NULL_HANDLE ||
        _surfaceCache.cards.empty()) {
        _surfaceCache.radiosityBatchTexels = 0;
        return;
    }
    const auto& cards = _surfaceCache.cards;

    VkDescriptorSet set = get_current_frame()._frameDescriptors.allocate(
        _device, _surfaceCache.radiosityLayout);
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
    writer.write_image(10, _surfaceCache.indirectPrevious.imageView, _prepass.sampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
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
    _surfaceCache.radiosityBatchTexels = texels;

    const bool timed = _gpuTiming.supported;
    if (timed) {
        vkCmdResetQueryPool(cmd, _surfaceCache.radiosityTimestampPool, slot * 2, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            _surfaceCache.radiosityTimestampPool, slot * 2);
    }
    // Bounces read indirectPrevious, which equals indirect here; the batch is
    // written into indirect, then copied back so the two agree again.  The
    // barriers are all-commands, so the previous frame's reads of indirect
    // finish before this update writes it.
    vkutil::transition_image(cmd, _surfaceCache.indirect.image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _surfaceCache.radiosityPipeline);
    const std::array<VkDescriptorSet, 2> sets{get_current_frame().sceneDescriptor, set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        _surfaceCache.radiosityPipelineLayout, 0, 2, sets.data(), 0, nullptr);
    std::vector<VkImageCopy> regions;
    regions.reserve(batch.size());
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

        VkImageCopy& region = regions.emplace_back();
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.srcOffset = {int32_t(card.rect.x), int32_t(card.rect.y), 0};
        region.dstOffset = region.srcOffset;
        region.extent = {card.rect.z, card.rect.w, 1};
    }
    vkutil::transition_image(cmd, _surfaceCache.indirect.image,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkutil::transition_image(cmd, _surfaceCache.indirectPrevious.image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyImage(cmd, _surfaceCache.indirect.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        _surfaceCache.indirectPrevious.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<uint32_t>(regions.size()), regions.data());
    vkutil::transition_image(cmd, _surfaceCache.indirectPrevious.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkutil::transition_image(cmd, _surfaceCache.indirect.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (timed) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            _surfaceCache.radiosityTimestampPool, slot * 2 + 1);
        _surfaceCache.radiosityTimingWritten[slot] = true;
    }
}

void VulkanEngine::measure_surface_cache_shadows()
{
    if (!_surfaceCache.lightingValid) {
        fmt::print("Surface cache shadow check skipped: the cache is not lit\n");
        return;
    }
    update_trace_scene();
    if (_traceTriangles.empty() || _traceNodes.empty()) {
        fmt::print("Surface cache shadow check skipped: no CPU trace scene\n");
        return;
    }
    const glm::uvec2 size = _surfaceCache.atlasSize;
    const size_t texels = size_t(size.x) * size.y;
    VK_CHECK(vkDeviceWaitIdle(_device));
    const auto readPage = [&](const AllocatedImage& page, size_t bytesPerTexel) {
        AllocatedBuffer buffer = create_buffer(texels * bytesPerTexel,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
        immediate_submit([&](VkCommandBuffer cmd) {
            vkutil::transition_image(cmd, page.image,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {size.x, size.y, 1};
            vkCmdCopyImageToBuffer(cmd, page.image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1, &copy);
            vkutil::transition_image(cmd, page.image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        std::vector<uint8_t> bytes(texels * bytesPerTexel);
        void* mapped = nullptr;
        VK_CHECK(vmaMapMemory(_allocator, buffer.allocation, &mapped));
        vmaInvalidateAllocation(_allocator, buffer.allocation, 0, VK_WHOLE_SIZE);
        std::memcpy(bytes.data(), mapped, bytes.size());
        vmaUnmapMemory(_allocator, buffer.allocation);
        destroy_buffer(buffer);
        return bytes;
    };
    const std::vector<uint8_t> depthBytes = readPage(_surfaceCache.depth, 2);
    const std::vector<uint8_t> normalBytes = readPage(_surfaceCache.normal, 4);
    const std::vector<uint8_t> directBytes = readPage(_surfaceCache.direct, 8);
    const auto half = [](const std::vector<uint8_t>& bytes, size_t offset) {
        uint16_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return glm::unpackHalf1x16(value);
    };

    size_t validTexels = 0;
    for (size_t i = 0; i < texels; ++i) {
        if (half(depthBytes, i * 2) < 1.0f) {
            ++validTexels;
        }
    }
    constexpr size_t TargetSamples = 40000;
    const uint32_t stride = std::max<uint32_t>(1, uint32_t(std::sqrt(
        double(validTexels) / double(TargetSamples))));
    const glm::vec3 sun = normalized_sun_direction(_shadow.sunlightDirection);

    size_t facing = 0;
    size_t exactLit = 0;
    size_t cacheLit = 0;
    size_t bothLit = 0;
    size_t cacheOnly = 0;
    size_t exactOnly = 0;
    std::vector<glm::vec3> exactOnlyPositions;
    for (const SurfaceCard& card : _surfaceCache.cards) {
        for (uint32_t y = 0; y < card.rect.w; y += stride) {
            for (uint32_t x = 0; x < card.rect.z; x += stride) {
                const size_t index = size_t(card.rect.y + y) * size.x + (card.rect.x + x);
                const float depth = half(depthBytes, index * 2);
                if (depth >= 1.0f) {
                    continue;
                }
                glm::vec3 normal(
                    normalBytes[index * 4] / 255.0f, normalBytes[index * 4 + 1] / 255.0f,
                    normalBytes[index * 4 + 2] / 255.0f);
                normal = glm::normalize(normal * 2.0f - 1.0f);
                const float noL = glm::dot(normal, sun);
                if (noL <= 0.1f) {
                    continue;
                }
                const glm::vec4 cardPoint(
                    (float(x) + 0.5f) / float(card.rect.z),
                    (float(y) + 0.5f) / float(card.rect.w), depth, 1.0f);
                const glm::vec3 world = glm::vec3(card.cardToWorld * cardPoint);
                const TraceCPUHit hit = trace_cpu_intersect(
                    _traceTriangles, _traceNodes, world + normal * 0.02f, sun, false);
                const bool lit = hit.triangle < 0;
                const glm::vec3 direct(
                    half(directBytes, index * 8), half(directBytes, index * 8 + 2),
                    half(directBytes, index * 8 + 4));
                const float visibility =
                    glm::dot(direct, glm::vec3(0.2126f, 0.7152f, 0.0722f)) / noL;
                const bool cached = visibility > 0.5f;
                ++facing;
                exactLit += lit;
                cacheLit += cached;
                bothLit += lit && cached;
                cacheOnly += cached && !lit;
                exactOnly += lit && !cached;
                if (lit && !cached && exactOnlyPositions.size() < 20000) {
                    exactOnlyPositions.push_back(world);
                }
            }
        }
    }
    const auto percent = [&](size_t n) { return facing ? double(n) / double(facing) * 100.0 : 0.0; };
    fmt::print("Surface cache shadow check: {} sun-facing texels sampled (stride {})\n", facing, stride);
    fmt::print("  exact rays lit {:.1f}%, cache lit {:.1f}%, agree {:.1f}%\n",
        percent(exactLit), percent(cacheLit), percent(facing - cacheOnly - exactOnly));
    fmt::print("  cache shadowed but exactly lit {:.1f}%, cache lit but exactly shadowed {:.1f}%\n",
        percent(exactOnly), percent(cacheOnly));
    if (!exactOnlyPositions.empty()) {
        glm::vec3 lo(std::numeric_limits<float>::max());
        glm::vec3 hi(std::numeric_limits<float>::lowest());
        for (const glm::vec3& p : exactOnlyPositions) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        fmt::print("  wrongly shadowed texels span ({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f})\n",
            lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
    }
}

// R4.28 diagnostic: sample exactly the card texels the direct shader lit.
// The cache-source mask isolates sun so the page is N.L * sun * field visibility.
void VulkanEngine::measure_cache_sun_visibility(const char* csvPath)
{
    if (!_surfaceCache.lightingValid || _r4Contributions.cacheSources != r4::LightSun ||
        _traceNodes.empty()) {
        fmt::print("Cache sun check requires a lit sun-only cache and trace BVH\n");
        return;
    }
    const glm::uvec2 atlas = _surfaceCache.atlasSize;
    const auto depth = read_image_bytes(_surfaceCache.depth, {atlas.x, atlas.y, 1}, 2);
    const auto normals = read_image_bytes(_surfaceCache.normal, {atlas.x, atlas.y, 1}, 4);
    const auto direct = read_image_bytes(_surfaceCache.direct, {atlas.x, atlas.y, 1}, 8);
    const glm::vec3 sun = normalized_sun_direction(_shadow.sunlightDirection);
    const float sunY = glm::dot(glm::vec3(sceneData.sunlightColor),
        glm::vec3(0.2126f, 0.7152f, 0.0722f));
    if (sunY <= 0.0f) {
        float maximum = 0.0f;
        for (size_t i = 0; i < size_t(atlas.x) * atlas.y; ++i) {
            for (size_t channel = 0; channel < 3; ++channel) {
                maximum = std::max(maximum, std::abs(half_at(direct, i * 8 + channel * 2)));
            }
        }
        fmt::print("Cache sun check: zero-sun page maximum {}\n", maximum);
        return;
    }
    std::ofstream csv(csvPath);
    csv << "texel_x,texel_y,screen_u,screen_v,in_front,no_l,l_exact,l_gpu\n";
    constexpr uint32_t Stride = 4;
    size_t facing = 0;
    for (const SurfaceCard& card : _surfaceCache.cards) {
        for (uint32_t y = 0; y < card.rect.w; y += Stride) {
            for (uint32_t x = 0; x < card.rect.z; x += Stride) {
                const glm::uvec2 texel(card.rect.x + x, card.rect.y + y);
                const size_t index = size_t(texel.y) * atlas.x + texel.x;
                const float z = half_at(depth, index * 2);
                if (z >= 1.0f) continue;
                glm::vec3 normal(normals[index * 4] / 255.0f,
                    normals[index * 4 + 1] / 255.0f, normals[index * 4 + 2] / 255.0f);
                normal = glm::normalize(normal * 2.0f - 1.0f);
                const float noL = glm::dot(normal, sun);
                if (noL <= 0.0f) continue;
                const glm::vec3 world = glm::vec3(card.cardToWorld * glm::vec4(
                    (float(x) + 0.5f) / float(card.rect.z),
                    (float(y) + 0.5f) / float(card.rect.w), z, 1.0f));
                const float epsilon = std::max(0.0002f,
                    std::max({std::abs(world.x), std::abs(world.y), std::abs(world.z)}) * 0.000002f);
                const TraceCPUHit hit = trace_cpu_intersect(_traceTriangles, _traceNodes,
                    world + normal * epsilon, sun, false);
                const float exact = hit.triangle < 0 ? noL : 0.0f;
                const glm::vec3 gpu(half_at(direct, index * 8),
                    half_at(direct, index * 8 + 2), half_at(direct, index * 8 + 4));
                const float field = glm::dot(gpu, glm::vec3(0.2126f, 0.7152f, 0.0722f)) / sunY;
                const glm::vec4 clip = sceneData.viewproj * glm::vec4(world, 1.0f);
                const bool inFront = clip.w > 0.0f;
                const glm::vec2 screen = inFront
                    ? glm::vec2(clip.x / clip.w * 0.5f + 0.5f,
                        clip.y / clip.w * 0.5f + 0.5f) : glm::vec2(-1.0f);
                csv << texel.x << ',' << texel.y << ',' << screen.x << ',' << screen.y
                    << ',' << inFront << ',' << noL << ',' << exact << ',' << field << '\n';
                ++facing;
            }
        }
    }
    fmt::print("Cache sun check: {} facing texels (stride {}), written {}\n",
        facing, Stride, csvPath);
}

void VulkanEngine::apply_r4_light_sources()
{
    if ((_r4Contributions.lightSources & r4::LightSun) == 0u) {
        _traceSettings.lighting.sunRadiance = glm::vec4(0.0f);
    }
    if ((_r4Contributions.lightSources & r4::LightSky) == 0u) {
        _traceSettings.lighting.environment.y = 1.0f;
    }
}

// R4.11 diagnostic: the cache's emitter visibility (a scene-field march,
// emitter_sampling.glsl) against exact BVH visibility for the same receiver
// texels and the same 64 emitter samples each, with the surface that stops
// each disagreeing field march.  CPU only, once, after the cache is lit.
std::vector<uint8_t> VulkanEngine::read_image_bytes(const AllocatedImage& image,
    VkExtent3D extent, size_t bytesPerTexel, VkImageAspectFlags aspect)
{
    const size_t bytes = size_t(extent.width) * extent.height * extent.depth * bytesPerTexel;
    AllocatedBuffer buffer = create_buffer(bytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
    immediate_submit([&](VkCommandBuffer cmd) {
        vkutil::transition_image(cmd, image.image,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspect);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {aspect, 0, 0, 1};
        copy.imageExtent = extent;
        vkCmdCopyImageToBuffer(cmd, image.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1, &copy);
        vkutil::transition_image(cmd, image.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, aspect);
    });
    std::vector<uint8_t> out(bytes);
    void* mapped = nullptr;
    VK_CHECK(vmaMapMemory(_allocator, buffer.allocation, &mapped));
    vmaInvalidateAllocation(_allocator, buffer.allocation, 0, VK_WHOLE_SIZE);
    std::memcpy(out.data(), mapped, bytes);
    vmaUnmapMemory(_allocator, buffer.allocation);
    destroy_buffer(buffer);
    return out;
}

glm::vec3 VulkanEngine::trace_albedo(const TraceTriangle& t, glm::vec2 bary) const
{
    const auto srgb = [](float c) {
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    const TraceMaterial& m = _traceMaterials[t.meta.x];
    glm::vec3 color = glm::vec3(m.baseColor) *
        glm::vec3(t.c0 * (1.0f - bary.x - bary.y) + t.c1 * bary.x + t.c2 * bary.y);
    if (m.texture.y == 0 || m.texture.z == 0) return glm::clamp(color, 0.0f, 1.0f);
    const glm::vec2 uv = glm::fract((glm::vec2(t.n0.w, t.n1.w) *
        (1.0f - bary.x - bary.y) + glm::vec2(t.uv12.x, t.uv12.y) * bary.x +
        glm::vec2(t.uv12.z, t.uv12.w) * bary.y) * glm::vec2(m.uvScale));
    const glm::vec2 p = uv * glm::vec2(m.texture.y, m.texture.z) - 0.5f;
    const glm::ivec2 low = glm::ivec2(glm::floor(p));
    const glm::vec2 f = glm::fract(p);
    const auto texel = [&](glm::ivec2 xy) {
        const glm::ivec2 size(m.texture.y, m.texture.z);
        xy = (xy % size + size) % size;
        const uint32_t packed = _traceTexels[m.texture.x + xy.y * size.x + xy.x];
        return glm::vec3(srgb(float(packed & 255u) / 255.0f),
            srgb(float((packed >> 8u) & 255u) / 255.0f),
            srgb(float((packed >> 16u) & 255u) / 255.0f));
    };
    return glm::clamp(color * glm::mix(
        glm::mix(texel(low), texel(low + glm::ivec2(1, 0)), f.x),
        glm::mix(texel(low + glm::ivec2(0, 1)), texel(low + glm::ivec2(1, 1)), f.x),
        f.y), 0.0f, 1.0f);
}

std::vector<glm::vec4> VulkanEngine::build_albedo_volume(glm::vec3 lo, glm::vec3 voxel,
    glm::uvec3 dims) const
{
    std::vector<glm::vec4> cells(size_t(dims.x) * dims.y * dims.z, glm::vec4(0.0f));
    const glm::vec3 hi = lo + glm::vec3(dims) * voxel;
    const float largest = std::max({voxel.x, voxel.y, voxel.z});
    for (const TraceTriangle& t : _traceTriangles) {
        const TraceMaterial& m = _traceMaterials[t.meta.x];
        // Only what a bounce reflects: glass passes light on, emitters are
        // sampled as emitters.
        if (m.parameters.z > 0.0f || glm::dot(glm::vec3(m.emission), glm::vec3(1.0f)) > 0.0f)
            continue;
        const glm::vec3 p0(t.p0), e1 = glm::vec3(t.p1) - p0, e2 = glm::vec3(t.p2) - p0;
        // Split until no edge is wider than a voxel, and
        // drop any piece outside the box, so a ground plane far larger than
        // the scene costs only the part of it inside.  One sample per piece,
        // at its centroid, weighted by its area.
        struct Piece { glm::vec2 a, b, c; };
        std::vector<Piece> stack{{glm::vec2(0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f, 1.0f)}};
        while (!stack.empty()) {
            const Piece piece = stack.back();
            stack.pop_back();
            const glm::vec3 a = p0 + e1 * piece.a.x + e2 * piece.a.y;
            const glm::vec3 b = p0 + e1 * piece.b.x + e2 * piece.b.y;
            const glm::vec3 c = p0 + e1 * piece.c.x + e2 * piece.c.y;
            if (glm::any(glm::lessThan(glm::max(a, glm::max(b, c)), lo)) ||
                glm::any(glm::greaterThan(glm::min(a, glm::min(b, c)), hi))) continue;
            // Halve the longest edge only, so a long sliver splits along its
            // length instead of into pieces far smaller than a voxel.
            const float ab = glm::distance(a, b), bc = glm::distance(b, c),
                ca = glm::distance(c, a);
            if (std::max({ab, bc, ca}) > largest) {
                if (ab >= bc && ab >= ca) {
                    const glm::vec2 m = 0.5f * (piece.a + piece.b);
                    stack.push_back({piece.a, m, piece.c});
                    stack.push_back({m, piece.b, piece.c});
                } else if (bc >= ca) {
                    const glm::vec2 m = 0.5f * (piece.b + piece.c);
                    stack.push_back({piece.a, piece.b, m});
                    stack.push_back({piece.a, m, piece.c});
                } else {
                    const glm::vec2 m = 0.5f * (piece.c + piece.a);
                    stack.push_back({piece.a, piece.b, m});
                    stack.push_back({m, piece.b, piece.c});
                }
                continue;
            }
            const float area = 0.5f * glm::length(glm::cross(b - a, c - a));
            if (!(area > 0.0f)) continue;
            const glm::vec2 bary = (piece.a + piece.b + piece.c) / 3.0f;
            const glm::ivec3 cell = glm::ivec3(glm::floor(((a + b + c) / 3.0f - lo) / voxel));
            if (glm::any(glm::lessThan(cell, glm::ivec3(0))) ||
                glm::any(glm::greaterThanEqual(cell, glm::ivec3(dims)))) continue;
            cells[(size_t(cell.z) * dims.y + cell.y) * dims.x + cell.x] +=
                glm::vec4(trace_albedo(t, bary) * area, area);
        }
    }
    return cells;
}

void VulkanEngine::update_albedo_volume()
{
    uint64_t hash = _traceSceneHash ^ 0x9e3779b97f4a7c15ull;
    for (float value : {_sceneSdf.fieldMin.x, _sceneSdf.fieldMin.y, _sceneSdf.fieldMin.z,
             _sceneSdf.fieldMax.x, _sceneSdf.fieldMax.y, _sceneSdf.fieldMax.z,
             _sceneSdf.voxelSize}) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        hash = (hash ^ bits) * 1099511628211ull;
    }
    if (_surfaceCache.albedoVolume.image != VK_NULL_HANDLE &&
        hash == _surfaceCache.albedoVolumeHash) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    // About four field voxels, spanning exactly the field's box so a shader
    // maps a point into both with one transform.
    const glm::vec3 extent = glm::max(_sceneSdf.fieldMax - _sceneSdf.fieldMin, glm::vec3(1e-3f));
    const glm::uvec3 dims = glm::max(glm::uvec3(glm::ceil(
        extent / std::max(4.0f * _sceneSdf.voxelSize, 1e-3f))), glm::uvec3(1u));
    const std::vector<glm::vec4> cells = _traceTriangles.empty()
        ? std::vector<glm::vec4>(size_t(dims.x) * dims.y * dims.z, glm::vec4(0.0f))
        : build_albedo_volume(_sceneSdf.fieldMin, extent / glm::vec3(dims), dims);
    // Premultiplied by presence: rgb = mean albedo, a = 1 where a surface
    // is, so a filtered read divided by a averages only real surfaces.
    std::vector<uint32_t> texels(cells.size(), 0u);
    for (size_t i = 0; i < cells.size(); ++i) {
        if (!(cells[i].w > 0.0f)) continue;
        const glm::vec3 albedo = glm::clamp(glm::vec3(cells[i]) / cells[i].w, 0.0f, 1.0f);
        texels[i] = glm::packUnorm4x8(glm::vec4(albedo, 1.0f));
    }
    if (_surfaceCache.albedoVolume.image != VK_NULL_HANDLE) {
        VK_CHECK(vkDeviceWaitIdle(_device));
        destroy_image(_surfaceCache.albedoVolume);
    }
    _surfaceCache.albedoVolume = create_image(texels.data(), VkExtent3D{dims.x, dims.y, dims.z},
        VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
    _surfaceCache.albedoVolumeSize = dims;
    _surfaceCache.albedoVolumeVoxel = extent.x / float(dims.x);
    _surfaceCache.albedoVolumeHash = hash;
    fmt::print("Surface cache albedo volume: {}x{}x{} at {:.1f} cm, {} KiB, built in {:.0f} ms\n",
        dims.x, dims.y, dims.z, _surfaceCache.albedoVolumeVoxel * 100.0f,
        texels.size() * 4 / 1024, std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
}

void VulkanEngine::measure_cache_source_importance(const char* csvPath)
{
    const std::string outputPath(csvPath);
    if (!_surfaceCache.lightingValid || _traceNodes.empty() ||
        _surfaceCache.cards.empty()) std::abort();
    const auto started = std::chrono::steady_clock::now();
    VK_CHECK(vkDeviceWaitIdle(_device));
    const glm::uvec2 atlas = _surfaceCache.atlasSize;
    const auto depth = read_image_bytes(_surfaceCache.depth, {atlas.x, atlas.y, 1}, 2);
    const auto normal = read_image_bytes(_surfaceCache.normal, {atlas.x, atlas.y, 1}, 4);
    const auto albedo = read_image_bytes(_surfaceCache.albedo, {atlas.x, atlas.y, 1}, 4);
    const auto direct = read_image_bytes(_surfaceCache.direct, {atlas.x, atlas.y, 1}, 8);
    const auto half = [](const std::vector<uint8_t>& bytes, size_t offset) {
        uint16_t packed;
        std::memcpy(&packed, bytes.data() + offset, 2);
        return glm::unpackHalf1x16(packed);
    };
    const auto luminance = [](glm::vec3 c) {
        return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    };
    struct Source { glm::vec3 point, normal, radiance; float area; double mass; };
    std::vector<Source> sources;
    std::vector<double> cdf;
    std::vector<glm::vec3> cardDirections(_surfaceCache.cards.size());
    for (size_t i = 0; i < _surfaceCache.cards.size(); ++i) {
        const SurfaceCard& card = _surfaceCache.cards[i];
        const glm::mat3 matrix = glm::transpose(glm::inverse(
            glm::mat3(_surfaceCache.instances[card.instance].transform)));
        glm::vec3 axis(0.0f);
        axis[card.axis] = card.sign;
        cardDirections[i] = glm::normalize(matrix * axis);
    }
    double totalMass = 0.0;
    for (size_t cardIndex = 0; cardIndex < _surfaceCache.cards.size(); ++cardIndex) {
        const SurfaceCard& card = _surfaceCache.cards[cardIndex];
        const float projectedArea = glm::length(glm::cross(
            glm::vec3(card.cardToWorld[0]), glm::vec3(card.cardToWorld[1]))) /
            float(card.rect.z * card.rect.w);
        for (uint32_t y = 0; y < card.rect.w; ++y) {
            for (uint32_t x = 0; x < card.rect.z; ++x) {
                const size_t pixel = size_t(card.rect.y + y) * atlas.x + card.rect.x + x;
                const float z = half(depth, pixel * 2);
                if (z >= 1.0f) continue;
                glm::vec3 n(normal[pixel * 4] / 255.0f,
                    normal[pixel * 4 + 1] / 255.0f,
                    normal[pixel * 4 + 2] / 255.0f);
                n = glm::normalize(n * 2.0f - 1.0f);
                const float facing = glm::dot(n, cardDirections[cardIndex]);
                if (facing <= 0.2f) continue;
                // Six cards see an instance. Keep only the card most aligned
                // with this captured normal so one patch is one light source.
                bool bestCard = true;
                const size_t first = (cardIndex / 6) * 6;
                for (size_t j = first; j < std::min(first + 6, cardDirections.size()); ++j) {
                    const float other = glm::dot(n, cardDirections[j]);
                    if (other > facing + 1e-4f ||
                        (j < cardIndex && other >= facing - 1e-4f)) {
                        bestCard = false;
                        break;
                    }
                }
                if (!bestCard) continue;
                const glm::vec3 outgoing = glm::vec3(
                    albedo[pixel * 4] / 255.0f * half(direct, pixel * 8),
                    albedo[pixel * 4 + 1] / 255.0f * half(direct, pixel * 8 + 2),
                    albedo[pixel * 4 + 2] / 255.0f * half(direct, pixel * 8 + 4));
                const float area = projectedArea / facing;
                const double mass = double(luminance(outgoing)) * area;
                if (!(mass > 1e-9) || !std::isfinite(mass)) continue;
                const glm::vec3 point = glm::vec3(card.cardToWorld * glm::vec4(
                    (float(x) + 0.5f) / float(card.rect.z),
                    (float(y) + 0.5f) / float(card.rect.w), z, 1.0f));
                sources.push_back({point, n, outgoing, area, mass});
                totalMass += mass;
                cdf.push_back(totalMass);
            }
        }
    }
    if (sources.empty()) std::abort();

    const VkExtent2D extent = _drawExtent;
    const bool depth32 = _prepass.depthImage.imageFormat == VK_FORMAT_D32_SFLOAT;
    const auto screenDepth = read_image_bytes(_prepass.depthImage,
        {extent.width, extent.height, 1}, depth32 ? 4 : 2, VK_IMAGE_ASPECT_DEPTH_BIT);
    const auto screenNormals = read_ssgi_image(_prepass.normalImage, extent);
    const auto screenAlbedo = read_image_bytes(_sceneTargets.gbufferAlbedo,
        {extent.width, extent.height, 1}, 4);
    const auto screenZ = [&](size_t pixel) {
        if (!depth32) return half(screenDepth, pixel * 2);
        float z;
        std::memcpy(&z, screenDepth.data() + pixel * 4, 4);
        return z;
    };
    const glm::mat3 viewToWorld = glm::transpose(glm::mat3(sceneData.view));
    constexpr int Samples = 128;
    constexpr float Pi = 3.14159265359f;
    const bool exhaustive = SDL_getenv("MIRABILIS_R4_SOURCE_EXHAUSTIVE") != nullptr;
    const bool exactBounce = SDL_getenv("MIRABILIS_R4_EXACT_BOUNCE") != nullptr;
    const bool cardBlend = SDL_getenv("MIRABILIS_R4_CARD_BLEND") != nullptr;
    const bool finePlacement = SDL_getenv("MIRABILIS_R4_FINE_CARD_PLACEMENT") != nullptr;
    const bool cardPlacement = cardBlend || finePlacement ||
        SDL_getenv("MIRABILIS_R4_CARD_PLACEMENT") != nullptr;
    const glm::vec3 sun = normalized_sun_direction(_shadow.sunlightDirection);
    const glm::vec3 sunRadiance(_traceSettings.lighting.sunRadiance);
    const auto sourceAlbedo = [&](const TraceTriangle& t, glm::vec2 bary) {
        return trace_albedo(t, bary);
    };
    const bool hitLadder = SDL_getenv("MIRABILIS_R4_HIT_LADDER") != nullptr;
    // R4.61 rung V: the coarse albedo volume, read as the renderer would.
    const float volumeVoxel = 4.0f * _sceneSdf.voxelSize;
    const glm::vec3 volumeMin = _sceneSdf.fieldMin;
    const glm::uvec3 volumeDims = glm::uvec3(glm::ceil(
        (_sceneSdf.fieldMax - _sceneSdf.fieldMin) / std::max(volumeVoxel, 1e-4f)));
    std::vector<glm::vec4> volume;
    if (hitLadder) {
        const auto built = std::chrono::steady_clock::now();
        volume = build_albedo_volume(volumeMin, glm::vec3(volumeVoxel), volumeDims);
        fmt::print("R4 albedo volume: {}x{}x{} at {:.1f} cm, {:.1f} ms CPU\n",
            volumeDims.x, volumeDims.y, volumeDims.z, volumeVoxel * 100.0f,
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - built).count());
    }
    const auto volumeAlbedo = [&](glm::vec3 point) {
        const glm::vec3 f = (point - volumeMin) / volumeVoxel - 0.5f;
        const glm::ivec3 base = glm::ivec3(glm::floor(f));
        const glm::vec3 t = f - glm::vec3(base);
        glm::vec4 sum(0.0f);
        for (int corner = 0; corner < 8; ++corner) {
            const glm::ivec3 o(corner & 1, (corner >> 1) & 1, corner >> 2);
            const glm::ivec3 c = glm::clamp(base + o, glm::ivec3(0), glm::ivec3(volumeDims) - 1);
            const float w = (o.x ? t.x : 1.0f - t.x) * (o.y ? t.y : 1.0f - t.y) *
                (o.z ? t.z : 1.0f - t.z);
            sum += w * volume[(size_t(c.z) * volumeDims.y + c.y) * volumeDims.x + c.x];
        }
        return sum.w > 0.0f ? glm::vec3(sum) / sum.w : glm::vec3(0.0f);
    };
    std::vector<glm::mat4> worldToCards;
    if (cardPlacement || hitLadder) {
        worldToCards.reserve(_surfaceCache.cards.size());
        for (const SurfaceCard& card : _surfaceCache.cards)
            worldToCards.push_back(glm::inverse(card.cardToWorld));
    }
    // R4.60: the cache as surface_cache_lookup() reads it at any point, over
    // every card rather than one grid cell's first 64 (R4.34: negligible).
    struct CacheHit { bool covered; glm::vec3 albedo, direct, texelSun; };
    const auto cacheAt = [&](glm::vec3 point, glm::vec3 surfaceNormal) {
        const float front = _surfaceCache.texelSize;
        const float margin = 0.25f * _surfaceCache.texelSize;
        constexpr float DepthTolerance = 0.15f;
        const auto probe = [&](size_t i, size_t& index, float& behind, float& facing) {
            facing = glm::dot(surfaceNormal, cardDirections[i]);
            if (facing < 0.2f) return false;
            const SurfaceCard& card = _surfaceCache.cards[i];
            const glm::vec3 c = glm::vec3(worldToCards[i] * glm::vec4(point, 1.0f));
            if (c.x < 0.0f || c.y < 0.0f || c.x > 1.0f || c.y > 1.0f) return false;
            index = size_t(card.rect.y + std::min(uint32_t(c.y * card.rect.w), card.rect.w - 1u)) *
                atlas.x + card.rect.x + std::min(uint32_t(c.x * card.rect.z), card.rect.z - 1u);
            const float stored = half(depth, index * 2);
            if (stored >= 1.0f) return false;
            behind = (stored - c.z) * card.worldDepth;
            return true;
        };
        CacheHit out{false, glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f)};
        float nearest = 1e30f;
        for (size_t i = 0; i < _surfaceCache.cards.size(); ++i) {
            size_t index = 0;
            float behind = 0.0f, facing = 0.0f;
            if (probe(i, index, behind, facing) && behind >= -front && behind <= DepthTolerance)
                nearest = std::min(nearest, behind);
        }
        if (nearest > DepthTolerance) return out;
        float weightSum = 0.0f;
        for (size_t i = 0; i < _surfaceCache.cards.size(); ++i) {
            size_t index = 0;
            float behind = 0.0f, facing = 0.0f;
            if (!probe(i, index, behind, facing) || behind < -front) continue;
            const float further = std::max(behind - nearest, 0.0f) / margin;
            const float weight = facing * std::exp(-further * further);
            if (weight < 1e-3f) continue;
            out.albedo += weight * glm::vec3(albedo[index * 4], albedo[index * 4 + 1],
                albedo[index * 4 + 2]) / 255.0f;
            out.direct += weight * glm::vec3(half(direct, index * 8),
                half(direct, index * 8 + 2), half(direct, index * 8 + 4));
            // What an exact-visibility relight would store at this texel.
            const SurfaceCard& card = _surfaceCache.cards[i];
            const uint32_t tx = uint32_t(index % atlas.x), ty = uint32_t(index / atlas.x);
            const glm::vec3 texelPoint = glm::vec3(card.cardToWorld * glm::vec4(
                (float(tx - card.rect.x) + 0.5f) / card.rect.z,
                (float(ty - card.rect.y) + 0.5f) / card.rect.w, half(depth, index * 2), 1.0f));
            const glm::vec3 texelNormal = glm::normalize(glm::vec3(normal[index * 4],
                normal[index * 4 + 1], normal[index * 4 + 2]) / 255.0f * 2.0f - 1.0f);
            const float texelNoL = glm::dot(texelNormal, sun);
            if (texelNoL > 0.0f && trace_cpu_intersect(_traceTriangles, _traceNodes,
                    texelPoint + texelNormal * 0.002f, sun, false).triangle < 0)
                out.texelSun += weight * (texelNoL / Pi) * sunRadiance;
            weightSum += weight;
        }
        out.covered = weightSum > 0.0f;
        if (out.covered) {
            out.albedo /= weightSum;
            out.direct /= weightSum;
            out.texelSun /= weightSum;
        }
        return out;
    };
    // R4.60: why the lookup found nothing.  0 no card faces the point inside
    // its bounds, 1 only empty texels, 2 a card captured a nearer surface in
    // front of it (a hidden layer), 3 the captured surface is too far behind.
    std::ofstream hitCsv;
    if (hitLadder) {
        hitCsv.open(outputPath + ".uncovered.csv");
        hitCsv << "x,y,z,nx,ny,nz,material,energy,card,instance,axis,sign,behind,front\n";
    }
    // Every facing card whose bounds hold the point, with its depth gap.
    const auto logUncovered = [&](glm::vec3 point, glm::vec3 normal, uint32_t material,
                                  double energy, bool front) {
        for (size_t i = 0; i < _surfaceCache.cards.size(); ++i) {
            if (glm::dot(normal, cardDirections[i]) < 0.2f) continue;
            const SurfaceCard& card = _surfaceCache.cards[i];
            const glm::vec3 c = glm::vec3(worldToCards[i] * glm::vec4(point, 1.0f));
            if (c.x < 0.0f || c.y < 0.0f || c.x > 1.0f || c.y > 1.0f) continue;
            const size_t index = size_t(card.rect.y +
                std::min(uint32_t(c.y * card.rect.w), card.rect.w - 1u)) * atlas.x +
                card.rect.x + std::min(uint32_t(c.x * card.rect.z), card.rect.z - 1u);
            const float stored = half(depth, index * 2);
            hitCsv << point.x << ',' << point.y << ',' << point.z << ',' << normal.x << ','
                << normal.y << ',' << normal.z << ',' << material << ',' << energy << ','
                << i << ',' << card.instance << ',' << card.axis << ',' << card.sign << ','
                << (stored >= 1.0f ? 1e9f : (stored - c.z) * card.worldDepth) << ','
                << front << '\n';
        }
    };
    const auto whyUncovered = [&](glm::vec3 point, glm::vec3 normal) {
        int reason = 0;
        for (size_t i = 0; i < _surfaceCache.cards.size(); ++i) {
            if (glm::dot(normal, cardDirections[i]) < 0.2f) continue;
            const SurfaceCard& card = _surfaceCache.cards[i];
            const glm::vec3 c = glm::vec3(worldToCards[i] * glm::vec4(point, 1.0f));
            if (c.x < 0.0f || c.y < 0.0f || c.x > 1.0f || c.y > 1.0f) continue;
            const size_t index = size_t(card.rect.y +
                std::min(uint32_t(c.y * card.rect.w), card.rect.w - 1u)) * atlas.x +
                card.rect.x + std::min(uint32_t(c.x * card.rect.z), card.rect.z - 1u);
            const float stored = half(depth, index * 2);
            if (stored >= 1.0f) { reason = std::max(reason, 1); continue; }
            const float behind = (stored - c.z) * card.worldDepth;
            reason = std::max(reason, behind < 0.0f ? 2 : 3);
        }
        return reason;
    };
    double uncoveredByReason[4] = {0, 0, 0, 0};
    std::vector<double> uncoveredByMaterial(_traceMaterials.size(), 0.0);
    std::vector<double> exactByMaterial(_traceMaterials.size(), 0.0);
    constexpr uint32_t BounceSamples = 512;
    size_t bounceHits = 0;
    // ladder, when given, receives R4.60's C, A and U on the same rays.
    const auto bounceAt = [&](glm::vec3 point, glm::vec3 normal, uint32_t seed,
                              glm::vec3* ladder = nullptr) {
        glm::vec3 bounce(0.0f);
        const glm::vec3 helper = std::abs(normal.z) < 0.999f
            ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
        const glm::vec3 tangent = glm::normalize(glm::cross(helper, normal));
        const glm::vec3 bitangent = glm::cross(normal, tangent);
        const glm::vec3 rotation = diagnostic_rotation(seed);
        for (uint32_t i = 0; i < BounceSamples; ++i) {
            const glm::vec3 xi = diagnostic_stratum(i, BounceSamples, rotation);
            const float radius = std::sqrt(xi.y);
            const float angle = 2.0f * Pi * xi.z;
            const glm::vec3 direction = glm::normalize(
                tangent * (radius * std::cos(angle)) +
                bitangent * (radius * std::sin(angle)) +
                normal * std::sqrt(1.0f - xi.y));
            const glm::vec3 origin = point + normal * 0.0002f;
            const TraceCPUHit hit = trace_cpu_intersect(_traceTriangles, _traceNodes,
                origin, direction, false);
            if (hit.triangle < 0) continue;
            const TraceTriangle& t = _traceTriangles[hit.triangle];
            const TraceMaterial& m = _traceMaterials[t.meta.x];
            if (m.parameters.z > 0.0f) continue;
            const glm::vec3 geometric = glm::normalize(glm::cross(
                glm::vec3(t.p1 - t.p0), glm::vec3(t.p2 - t.p0)));
            const bool front = glm::dot(direction, geometric) < 0.0f;
            const glm::vec3 geo = front ? geometric : -geometric;
            glm::vec3 shade = glm::vec3(t.n0) * (1.0f - hit.bary.x - hit.bary.y) +
                glm::vec3(t.n1) * hit.bary.x + glm::vec3(t.n2) * hit.bary.y;
            shade = glm::dot(shade, shade) > 1e-16f
                ? glm::normalize(shade) : geometric;
            if (glm::dot(shade, geometric) < 0.0f) shade = -shade;
            if (!front) shade = -shade;
            const glm::vec3 hitPoint = origin + direction * hit.t;
            CacheHit cache{false, glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f)};
            if (ladder) {
                cache = cacheAt(hitPoint, shade);
                ladder[0] += cache.albedo * cache.direct;
                ladder[3] += cache.albedo * cache.texelSun;
            }
            const float noL = glm::dot(shade, sun);
            if (glm::dot(sun, geo) <= 0.0f || noL <= 0.0f) continue;
            const float epsilon = std::max(0.0002f,
                std::max({std::abs(hitPoint.x), std::abs(hitPoint.y),
                    std::abs(hitPoint.z)}) * 0.000002f);
            const TraceCPUHit shadow = trace_cpu_intersect(_traceTriangles, _traceNodes,
                hitPoint + geo * epsilon, sun, false);
            if (shadow.triangle >= 0) continue;
            const glm::vec3 halfVector = glm::normalize(sun - direction);
            const float voH = std::max(glm::dot(-direction, halfVector), 0.0f);
            const float metallic = glm::clamp(m.parameters.x, 0.0f, 1.0f);
            const auto sunBounce = [&](glm::vec3 albedoAtHit) {
                const glm::vec3 f0 = glm::mix(glm::vec3(0.04f), albedoAtHit, metallic);
                const glm::vec3 fresnel = f0 + (1.0f - f0) *
                    std::pow(glm::clamp(1.0f - voH, 0.0f, 1.0f), 5.0f);
                return (1.0f - fresnel) * (1.0f - metallic) * albedoAtHit *
                    (noL / Pi) * sunRadiance;
            };
            const glm::vec3 exact = sunBounce(sourceAlbedo(t, hit.bary));
            bounce += exact;
            if (ladder) {
                const double energy = luminance(exact);
                exactByMaterial[t.meta.x] += energy;
                if (cache.covered) {
                    ladder[1] += sunBounce(cache.albedo);
                } else {
                    ladder[2] += exact;
                    ladder[4] += sunBounce(volumeAlbedo(hitPoint));
                    uncoveredByReason[whyUncovered(hitPoint, shade)] += energy;
                    logUncovered(hitPoint, shade, t.meta.x, energy, front);
                    uncoveredByMaterial[t.meta.x] += energy;
                }
            }
            ++bounceHits;
        }
        if (ladder) {
            for (int i = 0; i < 5; ++i) ladder[i] /= float(BounceSamples);
        }
        return bounce / float(BounceSamples);
    };
    const glm::vec3 patchRay(0.0f, 0.0f, 1.0f);
    const float unitPatch = std::max(glm::dot(glm::vec3(0, 0, 1), patchRay), 0.0f) *
        std::max(glm::dot(glm::vec3(0, 0, -1), -patchRay), 0.0f) /
        (Pi * glm::dot(patchRay, patchRay));
    if (std::abs(unitPatch - 0.31830988618f) > 1e-6f) std::abort();
    std::ofstream csv(outputPath);
    csv << "x,y,unoccluded_r,unoccluded_g,unoccluded_b,visible_r,visible_g,visible_b";
    if (exhaustive) csv << ",exact_unoccluded_r,exact_unoccluded_g,exact_unoccluded_b,"
        "exact_visible_r,exact_visible_g,exact_visible_b";
    if (exactBounce) csv << ",bounce_r,bounce_g,bounce_b";
    if (hitLadder) csv << ",ladder_c_r,ladder_c_g,ladder_c_b,ladder_a_r,ladder_a_g,ladder_a_b,"
        "ladder_u_r,ladder_u_g,ladder_u_b,ladder_t_r,ladder_t_g,ladder_t_b,ladder_v_r,ladder_v_g,ladder_v_b";
    if (cardPlacement) csv << ",card_covered,card_count,card_bounce_r,card_bounce_g,card_bounce_b"
        ",world_x,world_y,world_z,card_index,card_instance,card_distance,normal_agreement,"
        "card_behind,primary_material";
    csv << '\n';
    size_t receivers = 0, visibleSamples = 0, exhaustiveVisible = 0, cardCovered = 0,
        cardSamples = 0;
    const uint32_t step = exhaustive ? 32u : 16u;
    for (uint32_t y = step / 2; y < extent.height; y += step) {
        for (uint32_t x = step / 2; x < extent.width; x += step) {
            const size_t pixel = size_t(y) * extent.width + x;
            const float z = screenZ(pixel);
            if (z <= 1e-6f) continue;
            const glm::vec4 clip(
                (float(x) + 0.5f) / float(extent.width) * 2.0f - 1.0f,
                (float(y) + 0.5f) / float(extent.height) * 2.0f - 1.0f,
                z, 1.0f);
            const glm::vec4 world4 = sceneData.inverseViewProjection * clip;
            const glm::vec3 world = glm::vec3(world4) / world4.w;
            const glm::vec3 n = glm::normalize(viewToWorld *
                glm::vec3(screenNormals[pixel]));
            const glm::vec3 receiverAlbedo(
                screenAlbedo[pixel * 4] / 255.0f,
                screenAlbedo[pixel * 4 + 1] / 255.0f,
                screenAlbedo[pixel * 4 + 2] / 255.0f);
            uint32_t state = (x * 1973u ^ y * 9277u ^ 0x68bc21ebu);
            state ^= state >> 16; state *= 0x7feb352du;
            state ^= state >> 15; state *= 0x846ca68bu;
            state ^= state >> 16;
            const double rotation = double(state) / 4294967296.0;
            glm::vec3 unoccluded(0.0f), visible(0.0f);
            for (int sample = 0; sample < Samples; ++sample) {
                const double u = (double(sample) + rotation) / Samples * totalMass;
                const size_t index = size_t(std::lower_bound(cdf.begin(), cdf.end(), u) - cdf.begin());
                const Source& source = sources[std::min(index, sources.size() - 1)];
                const glm::vec3 ray = source.point - world;
                const float distance2 = glm::dot(ray, ray);
                if (distance2 <= 1e-4f) continue;
                const float distance = std::sqrt(distance2);
                const glm::vec3 direction = ray / distance;
                const float cosR = std::max(glm::dot(n, direction), 0.0f);
                const float cosS = std::max(glm::dot(source.normal, -direction), 0.0f);
                if (cosR <= 0.0f || cosS <= 0.0f) continue;
                const double pdf = source.mass / totalMass;
                const glm::vec3 value = source.radiance *
                    float(cosR * cosS * source.area / (Pi * distance2 * pdf * Samples));
                unoccluded += value;
                const TraceCPUHit hit = trace_cpu_intersect(_traceTriangles, _traceNodes,
                    world + n * 0.0002f, direction, false);
                if (hit.triangle < 0 || hit.t >= distance - 0.002f) {
                    visible += value;
                    ++visibleSamples;
                }
            }
            unoccluded *= receiverAlbedo;
            visible *= receiverAlbedo;
            glm::vec3 exactUnoccluded(0.0f), exactVisible(0.0f);
            if (exhaustive) {
                for (const Source& source : sources) {
                    const glm::vec3 ray = source.point - world;
                    const float distance2 = glm::dot(ray, ray);
                    if (distance2 <= 1e-4f) continue;
                    const float distance = std::sqrt(distance2);
                    const glm::vec3 direction = ray / distance;
                    const float cosR = std::max(glm::dot(n, direction), 0.0f);
                    const float cosS = std::max(glm::dot(source.normal, -direction), 0.0f);
                    if (cosR <= 0.0f || cosS <= 0.0f) continue;
                    const glm::vec3 value = source.radiance *
                        (cosR * cosS * source.area / (Pi * distance2));
                    exactUnoccluded += value;
                    const TraceCPUHit hit = trace_cpu_intersect(_traceTriangles, _traceNodes,
                        world + n * 0.0002f, direction, false);
                    if (hit.triangle < 0 || hit.t >= distance - 0.002f) {
                        exactVisible += value;
                        ++exhaustiveVisible;
                    }
                }
                exactUnoccluded *= receiverAlbedo;
                exactVisible *= receiverAlbedo;
            }
            glm::vec3 ladder[5] = {glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f)};
            const glm::vec3 bounce = exactBounce
                ? bounceAt(world, n, x * 1973u ^ y * 9277u ^ 0x74bc21ebu,
                    hitLadder ? ladder : nullptr) * receiverAlbedo : glm::vec3(0.0f);
            for (glm::vec3& value : ladder) {
                value *= receiverAlbedo;
                if (!std::isfinite(value.x + value.y + value.z)) std::abort();
            }
            glm::vec3 cardBounce(0.0f);
            bool covered = false;
            uint32_t cardCount = 0;
            size_t selectedCard = 0;
            float cardDistance = 0.0f, normalAgreement = 0.0f,
                selectedBehind = 0.0f;
            if (cardPlacement) {
                float nearest = 0.15f;
                size_t selected = 0, selectedPixel = 0;
                const auto probe = [&](size_t cardIndex, size_t& index,
                                       float& behind, float& facing) {
                    facing = glm::dot(n, cardDirections[cardIndex]);
                    if (facing < 0.2f) return false;
                    const SurfaceCard& card = _surfaceCache.cards[cardIndex];
                    const glm::vec3 c = glm::vec3(worldToCards[cardIndex] *
                        glm::vec4(world, 1.0f));
                    if (c.x < 0.0f || c.y < 0.0f || c.x > 1.0f || c.y > 1.0f)
                        return false;
                    const uint32_t tx = card.rect.x + std::min(
                        uint32_t(c.x * card.rect.z), card.rect.z - 1u);
                    const uint32_t ty = card.rect.y + std::min(
                        uint32_t(c.y * card.rect.w), card.rect.w - 1u);
                    index = size_t(ty) * atlas.x + tx;
                    const float stored = half(depth, index * 2);
                    if (stored >= 1.0f) return false;
                    behind = (stored - c.z) * card.worldDepth;
                    return true;
                };
                const auto cardValue = [&](size_t cardIndex, size_t index) {
                    const SurfaceCard& card = _surfaceCache.cards[cardIndex];
                    const uint32_t tx = uint32_t(index % atlas.x);
                    const uint32_t ty = uint32_t(index / atlas.x);
                    glm::vec2 uv((float(tx - card.rect.x) + 0.5f) / card.rect.z,
                        (float(ty - card.rect.y) + 0.5f) / card.rect.w);
                    if (finePlacement && card.instance == 0) {
                        const glm::vec2 fineSize = glm::vec2(card.rect.z, card.rect.w) * 4.0f;
                        const glm::vec2 query = glm::vec2(worldToCards[cardIndex] *
                            glm::vec4(world, 1.0f));
                        uv = (glm::floor(query * fineSize) + 0.5f) / fineSize;
                    }
                    const glm::vec3 point = glm::vec3(card.cardToWorld * glm::vec4(
                        uv, half(depth, index * 2), 1.0f));
                    glm::vec3 cardNormal(normal[index * 4] / 255.0f,
                        normal[index * 4 + 1] / 255.0f,
                        normal[index * 4 + 2] / 255.0f);
                    cardNormal = glm::normalize(cardNormal * 2.0f - 1.0f);
                    return bounceAt(point, cardNormal,
                        tx * 1973u ^ ty * 9277u ^ 0x74bc21ebu);
                };
                for (size_t cardIndex = 0; cardIndex < _surfaceCache.cards.size(); ++cardIndex) {
                    size_t index = 0;
                    float behind = 0.0f, facing = 0.0f;
                    if (!probe(cardIndex, index, behind, facing)) continue;
                    if (behind < -_surfaceCache.texelSize || behind > nearest) continue;
                    nearest = behind;
                    selected = cardIndex;
                    selectedPixel = index;
                    selectedBehind = behind;
                    covered = true;
                }
                if (covered) {
                    selectedCard = selected;
                    const SurfaceCard& chosen = _surfaceCache.cards[selected];
                    const uint32_t tx = uint32_t(selectedPixel % atlas.x);
                    const uint32_t ty = uint32_t(selectedPixel / atlas.x);
                    const glm::vec3 cardPoint = glm::vec3(chosen.cardToWorld * glm::vec4(
                        (float(tx - chosen.rect.x) + 0.5f) / chosen.rect.z,
                        (float(ty - chosen.rect.y) + 0.5f) / chosen.rect.w,
                        half(depth, selectedPixel * 2), 1.0f));
                    glm::vec3 cardNormal(normal[selectedPixel * 4] / 255.0f,
                        normal[selectedPixel * 4 + 1] / 255.0f,
                        normal[selectedPixel * 4 + 2] / 255.0f);
                    cardNormal = glm::normalize(cardNormal * 2.0f - 1.0f);
                    cardDistance = glm::distance(world, cardPoint);
                    normalAgreement = glm::dot(n, cardNormal);
                    if (cardBlend) {
                        float weightSum = 0.0f;
                        for (size_t j = 0; j < _surfaceCache.cards.size(); ++j) {
                            size_t index = 0;
                            float behind = 0.0f, facing = 0.0f;
                            if (!probe(j, index, behind, facing) ||
                                behind < -_surfaceCache.texelSize) continue;
                            const float further = std::max(behind - nearest, 0.0f) /
                                (0.25f * _surfaceCache.texelSize);
                            const float weight = facing * std::exp(-further * further);
                            if (weight < 0.001f) continue;
                            cardBounce += weight * cardValue(j, index);
                            weightSum += weight;
                            ++cardCount;
                        }
                        if (weightSum > 0.0f) cardBounce /= weightSum;
                    } else {
                        cardBounce = cardValue(selected, selectedPixel);
                        cardCount = 1;
                    }
                    cardBounce *= receiverAlbedo;
                    ++cardCovered;
                    cardSamples += cardCount;
                }
            }
            if (!std::isfinite(unoccluded.x) || !std::isfinite(unoccluded.y) ||
                !std::isfinite(unoccluded.z) || !std::isfinite(visible.x) ||
                !std::isfinite(visible.y) || !std::isfinite(visible.z) ||
                !std::isfinite(exactUnoccluded.x) || !std::isfinite(exactUnoccluded.y) ||
                !std::isfinite(exactUnoccluded.z) || !std::isfinite(exactVisible.x) ||
                !std::isfinite(exactVisible.y) || !std::isfinite(exactVisible.z) ||
                !std::isfinite(bounce.x) || !std::isfinite(bounce.y) ||
                !std::isfinite(bounce.z) || !std::isfinite(cardBounce.x) ||
                !std::isfinite(cardBounce.y) || !std::isfinite(cardBounce.z)) std::abort();
            csv << x << ',' << y << ',' << unoccluded.x << ',' << unoccluded.y << ','
                << unoccluded.z << ',' << visible.x << ',' << visible.y << ','
                << visible.z;
            if (exhaustive) csv << ',' << exactUnoccluded.x << ',' << exactUnoccluded.y
                << ',' << exactUnoccluded.z << ',' << exactVisible.x << ','
                << exactVisible.y << ',' << exactVisible.z;
            if (exactBounce) csv << ',' << bounce.x << ',' << bounce.y << ',' << bounce.z;
            if (hitLadder) {
                for (const glm::vec3& value : ladder)
                    csv << ',' << value.x << ',' << value.y << ',' << value.z;
            }
            if (cardPlacement) {
                const glm::vec3 camera(sceneData.cameraPosition);
                const TraceCPUHit primary = trace_cpu_intersect(_traceTriangles, _traceNodes,
                    camera, glm::normalize(world - camera), false);
                const int material = primary.triangle < 0 ? -1 :
                    int(_traceTriangles[primary.triangle].meta.x);
                csv << ',' << covered << ',' << cardCount << ',' << cardBounce.x << ','
                    << cardBounce.y << ',' << cardBounce.z << ',' << world.x << ','
                    << world.y << ',' << world.z << ',' << selectedCard << ','
                    << (covered ? int(_surfaceCache.cards[selectedCard].instance) : -1) << ','
                    << cardDistance << ',' << normalAgreement << ',' << selectedBehind << ','
                    << material;
            }
            csv << '\n';
            ++receivers;
        }
    }
    if (!csv) std::abort();
    const double milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    fmt::print("R4 source-importance: {} sources, {:.6f} weighted area, {} receivers, "
        "{} visible samples ({} exhaustive), {:.1f} ms CPU, source bytes ~{}, written {}\n",
        sources.size(), totalMass, receivers, visibleSamples, exhaustiveVisible, milliseconds,
        sources.size() * sizeof(Source), outputPath);
    if (exactBounce) fmt::print("R4 exact bounce: {} exact sun-visible hits across {} receiver rays\n",
        bounceHits, (receivers + cardCovered) * BounceSamples);
    if (cardPlacement) fmt::print("R4 card placement: {} of {} receivers covered, {} card samples\n",
        cardCovered, receivers, cardSamples);
    if (hitLadder) {
        const double total = std::accumulate(exactByMaterial.begin(), exactByMaterial.end(), 0.0);
        fmt::print("R4 hit ladder: uncovered sun-hit energy share by reason: no facing card {:.3f}, "
            "empty texel {:.3f}, hidden behind nearer layer {:.3f}, too far behind {:.3f}\n",
            uncoveredByReason[0] / total, uncoveredByReason[1] / total,
            uncoveredByReason[2] / total, uncoveredByReason[3] / total);
        for (size_t i = 0; i < exactByMaterial.size(); ++i) {
            if (exactByMaterial[i] <= 0.0) continue;
            fmt::print("R4 hit ladder: material {} exact share {:.3f}, uncovered share {:.3f}\n",
                i, exactByMaterial[i] / total, uncoveredByMaterial[i] / total);
        }
    }
    if (finePlacement) {
        size_t texels = 0;
        for (const SurfaceCard& card : _surfaceCache.cards)
            if (card.instance == 0) texels += size_t(card.rect.z) * card.rect.w;
        fmt::print("R4 fine-card bound: instance 0 has {} card texels; 4x each axis needs {}\n",
            texels, texels * 16);
    }
}

void VulkanEngine::measure_emitter_visibility()
{
    update_trace_scene();
    if (!_surfaceCache.lightingValid || !_sceneSdf.fieldValid || _traceEmitters.empty() ||
        _traceNodes.empty()) {
        fmt::print("Emitter visibility check skipped: needs a lit cache, a scene field and emitters\n");
        return;
    }
    VK_CHECK(vkDeviceWaitIdle(_device));
    const glm::uvec2 atlas = _surfaceCache.atlasSize;
    const std::vector<uint8_t> depthBytes = read_image_bytes(_surfaceCache.depth, {atlas.x, atlas.y, 1}, 2);
    const std::vector<uint8_t> normalBytes = read_image_bytes(_surfaceCache.normal, {atlas.x, atlas.y, 1}, 4);
    const glm::uvec3 dim = _sceneSdf.fieldDimensions;
    const FieldCopy field{dim, _sceneSdf.fieldMin, _sceneSdf.fieldMax - _sceneSdf.fieldMin,
        read_image_bytes(_sceneSdf.field, {dim.x, dim.y, dim.z}, 4)};
    const auto half = half_at;
    const float voxel = _sceneSdf.voxelSize;
    // emitter_visibility() in emitter_sampling.glsl, step for step: the
    // target arrives already lifted off the emitter along its normal.
    const auto fieldVisible = [&](glm::vec3 origin, glm::vec3 target, glm::vec3& stop) {
        const glm::vec3 delta = target - origin;
        const float span = glm::length(delta);
        if (span <= 1e-6f) return true;
        const glm::vec3 direction = delta / span;
        float t = 0.0f;
        for (int step = 0; step < 96 && t < span; ++step) {
            const float d = field.distance(origin + direction * t);
            if (d < 0.25f * voxel) {
                stop = origin + direction * t;
                return false;
            }
            t += std::max(d, 0.5f * voxel);
        }
        return true;
    };

    struct ClassTotals {
        size_t samples = 0, bothVisible = 0, bothBlocked = 0, fieldOnlyBlocked = 0, exactOnlyBlocked = 0;
        double exactLight = 0.0, fieldLight = 0.0;
    };
    std::array<ClassTotals, 3> totals{};  // walls, floor, ceiling
    const std::array<const char*, 3> classNames{"walls", "floor", "ceiling"};
    std::unordered_map<int, size_t> falseBlockers;
    std::unordered_map<int, double> falseBlockerDistance;
    const uint32_t count = uint32_t(_traceEmitters.size());
    constexpr uint32_t Samples = 64;
    constexpr uint32_t Stride = 4;
    for (const SurfaceCard& card : _surfaceCache.cards) {
        for (uint32_t y = 0; y < card.rect.w; y += Stride) {
            for (uint32_t x = 0; x < card.rect.z; x += Stride) {
                const glm::uvec2 texel(card.rect.x + x, card.rect.y + y);
                const size_t index = size_t(texel.y) * atlas.x + texel.x;
                const float depth = half(depthBytes, index * 2);
                if (depth >= 1.0f) continue;
                glm::vec3 normal(normalBytes[index * 4] / 255.0f, normalBytes[index * 4 + 1] / 255.0f,
                    normalBytes[index * 4 + 2] / 255.0f);
                normal = glm::normalize(normal * 2.0f - 1.0f);
                const int cls = normal.y > 0.7f ? 1 : (normal.y < -0.7f ? 2 : (std::abs(normal.y) < 0.3f ? 0 : -1));
                if (cls < 0) continue;
                const glm::vec3 world = glm::vec3(card.cardToWorld * glm::vec4(
                    (float(x) + 0.5f) / float(card.rect.z), (float(y) + 0.5f) / float(card.rect.w), depth, 1.0f));
                const glm::vec3 rotation = diagnostic_rotation(texel.x * 1973u ^ texel.y * 9277u ^ 0x68bc21ebu);
                const glm::vec3 origin = world + normal * (2.5f * voxel);
                const float epsilon = std::max(0.0002f, std::max({std::abs(world.x), std::abs(world.y), std::abs(world.z)}) * 0.000002f);
                for (uint32_t i = 0; i < Samples; ++i) {
                    const glm::vec3 xi = diagnostic_stratum(i, Samples, rotation);
                    const TraceTriangle& e = _traceTriangles[_traceEmitters[std::min(uint32_t(xi.x * float(count)), count - 1u)]];
                    const float u = std::sqrt(xi.y), v = xi.z;
                    const glm::vec3 p0(e.p0), p1(e.p1), p2(e.p2);
                    const glm::vec3 lightPoint = p0 * (1.0f - u) + p1 * (u * (1.0f - v)) + p2 * (u * v);
                    const glm::vec3 delta = lightPoint - world;
                    const float d2 = glm::dot(delta, delta);
                    if (d2 <= 1e-10f) continue;
                    const glm::vec3 direction = delta / std::sqrt(d2);
                    const glm::vec3 crossEdges = glm::cross(p1 - p0, p2 - p0);
                    const float twiceArea = glm::length(crossEdges);
                    glm::vec3 emitterNormal = crossEdges / twiceArea;
                    const glm::vec3 authored = glm::vec3(e.n0) + glm::vec3(e.n1) + glm::vec3(e.n2);
                    if (glm::dot(authored, authored) > 1e-16f && glm::dot(authored, emitterNormal) < 0) emitterNormal = -emitterNormal;
                    const float cosineLight = std::max(glm::dot(emitterNormal, -direction), 0.0f);
                    const float cosineReceiver = glm::dot(normal, direction);
                    if (cosineReceiver <= 0.0f || cosineLight <= 1e-8f) continue;
                    const glm::vec3 emission(_traceMaterials[e.meta.x].emission);
                    const double light = double(glm::dot(emission, glm::vec3(0.2126f, 0.7152f, 0.0722f))) *
                        cosineReceiver * cosineLight * 0.5 * twiceArea * count / (glm::pi<float>() * d2);
                    glm::vec3 stop(0.0f);
                    const bool fieldVisibleSample =
                        fieldVisible(origin, lightPoint + emitterNormal * (2.5f * voxel), stop);
                    const glm::vec3 rayOrigin = world + normal * epsilon;
                    const float span = glm::length(lightPoint - rayOrigin);
                    const TraceCPUHit hit = trace_cpu_intersect(_traceTriangles, _traceNodes, rayOrigin,
                        (lightPoint - rayOrigin) / span, false);
                    const bool exactVisible = !(hit.triangle >= 0 && hit.t < span - 1e-3f);
                    ClassTotals& c = totals[cls];
                    ++c.samples;
                    c.bothVisible += fieldVisibleSample && exactVisible;
                    c.bothBlocked += !fieldVisibleSample && !exactVisible;
                    c.fieldOnlyBlocked += !fieldVisibleSample && exactVisible;
                    c.exactOnlyBlocked += fieldVisibleSample && !exactVisible;
                    c.exactLight += exactVisible ? light : 0.0;
                    c.fieldLight += fieldVisibleSample ? light : 0.0;
                    if (!fieldVisibleSample && exactVisible) {
                        float best = 0.0f;
                        const int nearest = nearest_triangle(_traceTriangles, stop, best);
                        ++falseBlockers[nearest];
                        falseBlockerDistance[nearest] += best;
                    }
                }
            }
        }
    }
    fmt::print("Emitter visibility check: voxel {:.1f} cm, {} emitter triangles, {} samples per texel, texel stride {}\n",
        voxel * 100.0f, count, Samples, Stride);
    for (int cls = 0; cls < 3; ++cls) {
        const ClassTotals& c = totals[cls];
        if (c.samples == 0) continue;
        const auto pct = [&](size_t n) { return 100.0 * double(n) / double(c.samples); };
        fmt::print("  {}: {} samples; both visible {:.1f}%, both blocked {:.1f}%, field-only blocked {:.1f}%, "
            "exact-only blocked {:.1f}%; field/exact emitter light {:.4f}\n",
            classNames[cls], c.samples, pct(c.bothVisible), pct(c.bothBlocked), pct(c.fieldOnlyBlocked),
            pct(c.exactOnlyBlocked), c.exactLight > 0.0 ? c.fieldLight / c.exactLight : 0.0);
    }
    std::vector<std::pair<size_t, int>> ranked;
    for (const auto& [triangle, n] : falseBlockers) ranked.push_back({n, triangle});
    std::sort(ranked.rbegin(), ranked.rend());
    for (size_t r = 0; r < std::min<size_t>(ranked.size(), 6); ++r) {
        const auto [n, k] = ranked[r];
        if (k < 0) continue;
        const TraceTriangle& t = _traceTriangles[k];
        const glm::vec3 centroid = (glm::vec3(t.p0) + glm::vec3(t.p1) + glm::vec3(t.p2)) / 3.0f;
        const glm::vec3 n3 = glm::normalize(glm::cross(glm::vec3(t.p1 - t.p0), glm::vec3(t.p2 - t.p0)));
        const bool emissive = glm::length(glm::vec3(_traceMaterials[t.meta.x].emission)) > 0.0f;
        fmt::print("  false blocker #{}: triangle {} material {}{} centroid ({:.2f},{:.2f},{:.2f}) normal ({:.2f},{:.2f},{:.2f}): "
            "{} samples, mean stop-to-surface {:.1f} cm\n",
            r + 1, k, t.meta.x, emissive ? " (emitter)" : "", centroid.x, centroid.y, centroid.z,
            n3.x, n3.y, n3.z, n, 100.0 * falseBlockerDistance[k] / double(n));
    }
}

// R4.16 diagnostic: the cache's 32 hemispherical sky directions
// (sky_irradiance() in surface_cache_direct.comp) through the scene field
// against the path tracer's BVH.  One CSV row per texel, grouped by
// scripts/r4_sky_visibility.py.
void VulkanEngine::measure_sky_visibility(const char* csvPath)
{
    update_trace_scene();
    if (!_surfaceCache.lightingValid || !_sceneSdf.fieldValid || _traceNodes.empty()) {
        fmt::print("Sky visibility check skipped: needs a lit cache, a scene field and a trace scene\n");
        return;
    }
    VK_CHECK(vkDeviceWaitIdle(_device));
    const glm::uvec2 atlas = _surfaceCache.atlasSize;
    const std::vector<uint8_t> depthBytes = read_image_bytes(_surfaceCache.depth, {atlas.x, atlas.y, 1}, 2);
    const std::vector<uint8_t> normalBytes = read_image_bytes(_surfaceCache.normal, {atlas.x, atlas.y, 1}, 4);
    const std::vector<uint8_t> directBytes = read_image_bytes(_surfaceCache.sky, {atlas.x, atlas.y, 1}, 8);
    const glm::uvec3 dim = _sceneSdf.fieldDimensions;
    const FieldCopy field{dim, _sceneSdf.fieldMin, _sceneSdf.fieldMax - _sceneSdf.fieldMin,
        read_image_bytes(_sceneSdf.field, {dim.x, dim.y, dim.z}, 4)};
    const float voxel = _sceneSdf.voxelSize;
    const float intensity = sceneData.ssgiFallbackSettings.y > 0.5f ? 0.0f : sceneData.ssgiFallbackSettings.x;
    // gradient_radiance() in environment_gradient.glsl.
    const auto skyLuminance = [&](glm::vec3 d) {
        const float s = std::clamp(d.y * 0.5f + 0.5f, 0.0f, 1.0f);
        const glm::vec3 c = glm::mix(glm::vec3(0.7f, 0.8f, 1.0f), glm::vec3(0.12f, 0.3f, 0.65f), s);
        return intensity * glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    };
    std::ofstream csv(csvPath);
    csv << "texel_x,texel_y,world_x,world_y,world_z,normal_x,normal_y,normal_z,screen_u,screen_v,in_front,"
           "n_agree_exit,n_agree_blocked,n_false_block,n_false_exit,n_unresolved_lost,n_unresolved_harmless,"
           "l_exact,l_field,l_false_block,l_false_exit,l_unresolved_lost,l_gpu\n";
    std::map<uint32_t, size_t> falseBlockers;
    constexpr uint32_t Samples = 32;
    constexpr uint32_t Stride = 4;
    const glm::mat4 viewProjection = sceneData.viewproj;
    for (const SurfaceCard& card : _surfaceCache.cards) {
        for (uint32_t y = 0; y < card.rect.w; y += Stride) {
            for (uint32_t x = 0; x < card.rect.z; x += Stride) {
                const glm::uvec2 texel(card.rect.x + x, card.rect.y + y);
                const size_t index = size_t(texel.y) * atlas.x + texel.x;
                const float depth = half_at(depthBytes, index * 2);
                if (depth >= 1.0f) continue;
                const glm::vec3 gpuLight(half_at(directBytes, index * 8),
                    half_at(directBytes, index * 8 + 2), half_at(directBytes, index * 8 + 4));
                const double lGpu = 32.0 * glm::dot(gpuLight, glm::vec3(0.2126f, 0.7152f, 0.0722f));
                glm::vec3 normal(normalBytes[index * 4] / 255.0f, normalBytes[index * 4 + 1] / 255.0f,
                    normalBytes[index * 4 + 2] / 255.0f);
                normal = glm::normalize(normal * 2.0f - 1.0f);
                const glm::vec3 world = glm::vec3(card.cardToWorld * glm::vec4(
                    (float(x) + 0.5f) / float(card.rect.z), (float(y) + 0.5f) / float(card.rect.w), depth, 1.0f));
                const glm::vec3 origin = world + normal * (2.5f * voxel);
                const glm::vec3 helper = std::abs(normal.z) < 0.999f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
                const glm::vec3 tangent = glm::normalize(glm::cross(helper, normal));
                const glm::vec3 bitangent = glm::cross(normal, tangent);
                const glm::vec3 rotation = diagnostic_rotation(texel.x * 1973u ^ texel.y * 9277u ^ 0x5bd1e995u);
                const float epsilon = std::max(0.0002f,
                    std::max({std::abs(world.x), std::abs(world.y), std::abs(world.z)}) * 0.000002f);
                std::array<size_t, 6> n{};
                double lExact = 0, lField = 0, lFalseBlock = 0, lFalseExit = 0, lLost = 0;
                for (uint32_t i = 0; i < Samples; ++i) {
                    const glm::vec3 xi = diagnostic_stratum(i, Samples, rotation);
                    const float radius = std::sqrt(xi.y);
                    const float angle = 2.0f * glm::pi<float>() * xi.z;
                    const glm::vec3 direction = glm::normalize(tangent * (radius * std::cos(angle)) +
                        bitangent * (radius * std::sin(angle)) + normal * std::sqrt(std::max(1.0f - xi.y, 0.0f)));
                    // The shader's march, step for step.
                    const glm::vec3 inverse = 1.0f / direction;
                    const glm::vec3 t0 = (_sceneSdf.fieldMin - origin) * inverse;
                    const glm::vec3 t1 = (_sceneSdf.fieldMax - origin) * inverse;
                    const float tExit = std::min({std::max(t0.x, t1.x), std::max(t0.y, t1.y), std::max(t0.z, t1.z)});
                    float t = 0.0f;
                    bool blocked = false;
                    for (int step = 0; step < 96 && t < tExit; ++step) {
                        const float d = field.distance(origin + direction * t);
                        if (d < 0.25f * voxel) { blocked = true; break; }
                        t += std::max(d, 0.5f * voxel);
                    }
                    const bool fieldExit = !blocked && t >= tExit;
                    const bool unresolved = !blocked && !fieldExit;
                    const TraceCPUHit hit = trace_cpu_intersect(_traceTriangles, _traceNodes,
                        world + normal * epsilon, direction, false);
                    const bool escapes = hit.triangle < 0;
                    const double L = skyLuminance(direction);
                    lExact += escapes ? L : 0.0;
                    lField += fieldExit ? L : 0.0;
                    if (fieldExit && escapes) ++n[0];
                    else if (blocked && !escapes) ++n[1];
                    else if (blocked && escapes) {
                        ++n[2]; lFalseBlock += L;
                        float distance = 0.0f;
                        const int k = nearest_triangle(_traceTriangles, origin + direction * t, distance);
                        if (k >= 0) ++falseBlockers[_traceTriangles[k].meta.x];
                    }
                    else if (fieldExit && !escapes) { ++n[3]; lFalseExit += L; }
                    else if (unresolved && escapes) { ++n[4]; lLost += L; }
                    else ++n[5];
                }
                const glm::vec4 clip = viewProjection * glm::vec4(world, 1.0f);
                const bool inFront = clip.w > 0.0f;
                const glm::vec2 screen = inFront
                    ? glm::vec2(clip.x / clip.w * 0.5f + 0.5f, clip.y / clip.w * 0.5f + 0.5f)
                    : glm::vec2(-1.0f);
                csv << texel.x << ',' << texel.y << ',' << world.x << ',' << world.y << ',' << world.z << ','
                    << normal.x << ',' << normal.y << ',' << normal.z << ',' << screen.x << ',' << screen.y << ','
                    << inFront << ',' << n[0] << ',' << n[1] << ',' << n[2] << ',' << n[3] << ',' << n[4] << ','
                    << n[5] << ',' << lExact << ',' << lField << ',' << lFalseBlock << ',' << lFalseExit << ','
                    << lLost << ',' << lGpu << '\n';
            }
        }
    }
    fmt::print("Sky visibility check written: {}\n", csvPath);
    for (const auto& [material, count] : falseBlockers) {
        const TraceMaterial& m = _traceMaterials[material];
        fmt::print("  sky false blocking near material {} base ({:.2f},{:.2f},{:.2f}) transmission {:.2f}: {} samples\n",
            material, m.baseColor.x, m.baseColor.y, m.baseColor.z, m.parameters.z, count);
    }
}

void VulkanEngine::draw_surface_cache_debug(VkCommandBuffer cmd)
{
    if (!_surfaceCache.valid || _surfaceCache.debugPipeline == VK_NULL_HANDLE) {
        return;
    }
    if ((_surfaceCache.debugPage >= 6 && _surfaceCache.debugPage <= 10) ||
        _surfaceCache.debugPage == 13) {
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
            float(_surfaceCache.debugPage == 13 ? 5 : _surfaceCache.debugPage - 6), 0.0f);
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
        "Indirect light", "Lit with indirect",
        "Screen: indirect bounce at G-buffer"};
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
    ImGui::SameLine();
    if (ImGui::Button("Measure Coverage")) {
        measure_surface_cache_coverage();
    }
    ImGui::SameLine();
    if (ImGui::Button("Check Shadows")) {
        measure_surface_cache_shadows();
    }
}
