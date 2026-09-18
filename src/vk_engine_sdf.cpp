#include "vk_engine.h"
#include "vk_engine_render_helpers.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/packing.hpp>

#include "imgui.h"

#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_pipelines.h>

// Lumen-lite's 3D backup tracing tier.  Mesh volumes are baked offline by
// scripts/bake_sdf.py (the format is documented there); this file owns
// reading them, getting them onto the GPU, merging every drawn instance into
// the scene distance field, and the debug view that sphere-traces the result.

namespace {

struct SdfFile {
    glm::uvec3 dimensions{0};
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    // Signed distance in world units, texel (x, y, z) at
    // x + y * dimX + z * dimX * dimY.
    std::vector<float> voxels;
};

bool read_sdf_file(const std::string& path, SdfFile& out, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "could not open " + path;
        return false;
    }
    char magic[4];
    file.read(magic, 4);
    if (!file || std::memcmp(magic, "MSDF", 4) != 0) {
        error = "not an MSDF volume: " + path;
        return false;
    }
    uint32_t version = 0;
    uint32_t dimensions[3]{};
    float boundsMin[3]{};
    float boundsMax[3]{};
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    file.read(reinterpret_cast<char*>(dimensions), sizeof(dimensions));
    file.read(reinterpret_cast<char*>(boundsMin), sizeof(boundsMin));
    file.read(reinterpret_cast<char*>(boundsMax), sizeof(boundsMax));
    if (!file || version != 1) {
        error = "unreadable header or unsupported version in " + path;
        return false;
    }
    if (dimensions[0] == 0 || dimensions[1] == 0 || dimensions[2] == 0) {
        error = "empty volume in " + path;
        return false;
    }
    const size_t voxelCount =
        size_t(dimensions[0]) * dimensions[1] * dimensions[2];
    out.voxels.resize(voxelCount);
    file.read(reinterpret_cast<char*>(out.voxels.data()),
        std::streamsize(voxelCount * sizeof(float)));
    if (!file) {
        error = "volume data shorter than its header says in " + path;
        return false;
    }
    out.dimensions = {dimensions[0], dimensions[1], dimensions[2]};
    out.boundsMin = {boundsMin[0], boundsMin[1], boundsMin[2]};
    out.boundsMax = {boundsMax[0], boundsMax[1], boundsMax[2]};
    return true;
}

constexpr uint64_t FnvOffset = 1469598103934665603ull;
constexpr uint64_t FnvPrime = 1099511628211ull;

void fnv_append(uint64_t& hash, const void* data, size_t bytes)
{
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= p[i];
        hash *= FnvPrime;
    }
}

// The triangles of one instance that are drawn opaque, with only the
// vertices they use.  A mesh can mix opaque and transparent surfaces (a
// window frame and its glass in one mesh); the raster pass skips the glass,
// so the field must too, or it would block light the render shows passing
// through.
struct OpaqueGeometry {
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;
};

using IndexRange = std::pair<uint32_t, uint32_t>; // first index, count

OpaqueGeometry extract_opaque_geometry(
    const TraceMeshSource& source, const std::vector<IndexRange>& ranges)
{
    OpaqueGeometry geometry;
    std::unordered_map<uint32_t, uint32_t> remap;
    for (const auto& [first, count] : ranges) {
        const size_t end = std::min(source.indices.size(), size_t(first) + count);
        for (size_t i = first; i + 2 < end; i += 3) {
            for (int corner = 0; corner < 3; ++corner) {
                const uint32_t original = source.indices[i + corner];
                if (original >= source.vertices.size()) {
                    continue;
                }
                auto [it, inserted] = remap.emplace(
                    original, static_cast<uint32_t>(geometry.positions.size()));
                if (inserted) {
                    geometry.positions.push_back(source.vertices[original].position);
                }
                geometry.indices.push_back(it->second);
            }
        }
    }
    return geometry;
}

// Positions and indices only: a material or UV change does not change the
// distance field, so it must not invalidate a bake either.
uint64_t geometry_hash(const OpaqueGeometry& geometry)
{
    uint64_t hash = FnvOffset;
    fnv_append(hash, geometry.positions.data(),
        geometry.positions.size() * sizeof(glm::vec3));
    fnv_append(hash, geometry.indices.data(),
        geometry.indices.size() * sizeof(uint32_t));
    return hash;
}

std::string hash_name(uint64_t hash)
{
    return fmt::format("{:016x}", hash);
}

// The exact local-space geometry the engine draws, for the offline baker.
bool write_mesh_obj(const std::string& path, const OpaqueGeometry& geometry)
{
    std::ofstream file(path + ".partial");
    if (!file) {
        return false;
    }
    for (const glm::vec3& position : geometry.positions) {
        file << "v " << position.x << ' ' << position.y << ' ' << position.z << '\n';
    }
    for (size_t i = 0; i + 2 < geometry.indices.size(); i += 3) {
        file << "f " << geometry.indices[i] + 1 << ' ' << geometry.indices[i + 1] + 1
             << ' ' << geometry.indices[i + 2] + 1 << '\n';
    }
    file.close();
    if (!file) {
        return false;
    }
    std::error_code error;
    std::filesystem::rename(path + ".partial", path, error);
    return !error;
}

} // namespace

// Exact 32-bit distances when the device can filter them, half floats
// otherwise.  Half precision still resolves millimetres at the few-metre
// distances one mesh volume holds.
bool VulkanEngine::upload_sdf_voxels(
    const std::vector<float>& voxels, glm::uvec3 dimensions,
    AllocatedImage& image, VkFormat& format)
{
    const VkFormatFeatureFlags2 required =
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
    format = pick_format(_chosenGPU,
        {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16_SFLOAT}, required).format;
    if (format == VK_FORMAT_UNDEFINED) {
        return false;
    }
    const VkExtent3D extent{dimensions.x, dimensions.y, dimensions.z};
    std::vector<uint16_t> halfFloats;
    // create_image only copies from this pointer; it takes it non-const.
    void* data = const_cast<float*>(voxels.data());
    size_t dataSize = voxels.size() * sizeof(float);
    if (format == VK_FORMAT_R16_SFLOAT) {
        halfFloats.resize(voxels.size());
        for (size_t i = 0; i < voxels.size(); ++i) {
            halfFloats[i] = glm::packHalf1x16(voxels[i]);
        }
        data = halfFloats.data();
        dataSize = halfFloats.size() * sizeof(uint16_t);
    }
    image = create_image(
        data, dataSize, extent, format, VK_IMAGE_USAGE_SAMPLED_BIT, false);
    return true;
}

void VulkanEngine::init_sdf_resources()
{
    // Linear, so the march reads a continuous distance between texels rather
    // than stepping in voxel-sized plateaus; clamped, so a sample at the
    // volume's edge never wraps to the far side.
    VkSamplerCreateInfo samplerInfo = sampler_info(
        VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    VK_CHECK(vkCreateSampler(_device, &samplerInfo, nullptr, &_sdf.sampler));

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        _sdf.descriptorLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
    }
    // Written on every field build; zeroed until then, which reads as no
    // cascades to any tracer that runs early.
    _sceneSdf.cascadeBuffer = create_buffer(sizeof(SceneFieldCascades),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    std::memset(_sceneSdf.cascadeBuffer.info.pMappedData, 0, sizeof(SceneFieldCascades));
    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _sceneSdf.compositeLayout =
            builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
    }

    const auto makePipeline = [this](
        const char* path, VkDescriptorSetLayout setLayout, uint32_t pushSize,
        VkPipelineLayout& layout, VkPipeline& pipeline) {
        VkPushConstantRange pushRange{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = pushSize};
        VkPipelineLayoutCreateInfo layoutInfo =
            vkinit::pipeline_layout_create_info();
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        VK_CHECK(vkCreatePipelineLayout(_device, &layoutInfo, nullptr, &layout));

        ScopedShaderModule shader(_device);
        if (!shader.load(path)) {
            // Reported rather than fatal: the views simply have nothing to draw.
            fmt::print("Error loading {}\n", path);
            return false;
        }
        VkPipelineShaderStageCreateInfo stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader.get();
        stage.pName = "main";
        VkComputePipelineCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        createInfo.layout = layout;
        createInfo.stage = stage;
        VK_CHECK(vkCreateComputePipelines(
            _device, VK_NULL_HANDLE, 1, &createInfo, nullptr, &pipeline));
        return true;
    };
    if (!makePipeline("../../shaders/sdf_debug.comp.spv", _sdf.descriptorLayout,
            sizeof(SdfDebugPushConstants), _sdf.pipelineLayout, _sdf.pipeline)) {
        _sdf.status = "sdf_debug.comp.spv failed to load";
    }
    if (!makePipeline("../../shaders/sdf_composite.comp.spv",
            _sceneSdf.compositeLayout, sizeof(SdfCompositePushConstants),
            _sceneSdf.compositePipelineLayout, _sceneSdf.compositePipeline)) {
        _sceneSdf.status = "sdf_composite.comp.spv failed to load";
    }

    _mainDeletionQueue.push_function([this]() {
        if (_sdf.loaded) {
            destroy_image(_sdf.volume);
        }
        for (auto& [hash, volume] : _sceneSdf.volumes) {
            destroy_image(volume.image);
        }
        if (_sceneSdf.field.image != VK_NULL_HANDLE) {
            destroy_image(_sceneSdf.field);
        }
        destroy_buffer(_sceneSdf.cascadeBuffer);
        vkDestroyPipeline(_device, _sceneSdf.compositePipeline, nullptr);
        vkDestroyPipelineLayout(_device, _sceneSdf.compositePipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _sceneSdf.compositeLayout, nullptr);
        vkDestroyPipeline(_device, _sdf.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _sdf.pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _sdf.descriptorLayout, nullptr);
        vkDestroySampler(_device, _sdf.sampler, nullptr);
    });

    // Each value is copied out before the next SDL_getenv call, which may
    // reuse the storage the previous one returned.
    if (const char* offset = SDL_getenv("MIRABILIS_SDF_OFFSET")) {
        std::sscanf(offset, "%f %f %f",
            &_sdf.offset.x, &_sdf.offset.y, &_sdf.offset.z);
    }
    if (const char* mode = SDL_getenv("MIRABILIS_SDF_VIEW_MODE")) {
        _sdf.viewMode = std::clamp(std::atoi(mode), 0, 4);
    }
    if (const char* source = SDL_getenv("MIRABILIS_SDF_SOURCE")) {
        _sdf.source = std::clamp(std::atoi(source), 0, 1);
    }
    if (const char* path = SDL_getenv("MIRABILIS_SDF_VOLUME")) {
        _sdf.path = path;
        load_sdf_volume(_sdf.path);
    }
}

bool VulkanEngine::load_sdf_volume(const std::string& path)
{
    SdfFile file;
    std::string error;
    if (!read_sdf_file(path, file, error)) {
        _sdf.status = "Load failed: " + error;
        fmt::print("SDF volume {}\n", _sdf.status);
        return false;
    }

    // A frame already submitted may still hold a descriptor set naming the
    // old image, so it is only safe to free once the GPU is idle.
    if (_sdf.loaded) {
        VK_CHECK(vkDeviceWaitIdle(_device));
        destroy_image(_sdf.volume);
        _sdf.loaded = false;
    }
    if (!upload_sdf_voxels(file.voxels, file.dimensions, _sdf.volume, _sdf.format)) {
        _sdf.status = "Load failed: no filterable float format for a 3D volume";
        fmt::print("SDF volume {}\n", _sdf.status);
        return false;
    }
    _sdf.dimensions = file.dimensions;
    _sdf.boundsMin = file.boundsMin;
    _sdf.boundsMax = file.boundsMax;
    _sdf.path = path;
    _sdf.loaded = true;

    const auto [minimum, maximum] =
        std::minmax_element(file.voxels.begin(), file.voxels.end());
    _sdf.status = fmt::format(
        "{}x{}x{} {}, distance {:.3f}..{:.3f}",
        file.dimensions.x, file.dimensions.y, file.dimensions.z,
        _sdf.format == VK_FORMAT_R32_SFLOAT ? "R32F" : "R16F",
        *minimum, *maximum);
    fmt::print("SDF volume loaded {}: {}\n", path, _sdf.status);
    return true;
}

uint64_t VulkanEngine::collect_opaque_instances(
    std::vector<SceneOpaqueInstance>& instances) const
{
    // One instance per distinct mesh buffer and transform.  A mesh with
    // several materials arrives as several draws sharing both.
    instances.clear();
    std::unordered_map<uint64_t, size_t> instanceByKey;
    uint64_t drawHash = FnvOffset;
    for (const RenderObject& draw : worldDrawContext.OpaqueSurfaces) {
        if (draw.material == nullptr ||
            draw.material->passType == MaterialPass::Transparent) {
            continue;
        }
        uint64_t key = FnvOffset;
        fnv_append(key, &draw.vertexBufferAddress, sizeof(draw.vertexBufferAddress));
        fnv_append(key, &draw.transform, sizeof(draw.transform));
        auto [it, inserted] = instanceByKey.emplace(key, instances.size());
        if (inserted) {
            instances.push_back({draw.vertexBufferAddress, draw.transform, {}});
        }
        instances[it->second].draws.push_back(draw);
        fnv_append(drawHash, &key, sizeof(key));
        fnv_append(drawHash, &draw.firstIndex, sizeof(draw.firstIndex));
        fnv_append(drawHash, &draw.indexCount, sizeof(draw.indexCount));
        fnv_append(drawHash, &draw.material, sizeof(draw.material));
    }
    return drawHash;
}

void VulkanEngine::update_scene_sdf()
{
    if (_sceneSdf.compositePipeline == VK_NULL_HANDLE) {
        return;
    }

    std::vector<SceneOpaqueInstance> sceneInstances;
    const uint64_t drawHash = collect_opaque_instances(sceneInstances);
    struct Instance {
        VkDeviceAddress address;
        glm::mat4 transform;
        std::vector<IndexRange> ranges;
    };
    std::vector<Instance> instances;
    instances.reserve(sceneInstances.size());
    for (const SceneOpaqueInstance& sceneInstance : sceneInstances) {
        Instance instance{sceneInstance.address, sceneInstance.transform, {}};
        for (const RenderObject& draw : sceneInstance.draws) {
            instance.ranges.emplace_back(draw.firstIndex, draw.indexCount);
        }
        instances.push_back(std::move(instance));
    }
    // The camera cascades follow the camera: once it is an eighth of the
    // finest cascade away from where they were placed, they are placed again.
    bool recentre = false;
    if (_sceneSdf.fieldValid && _sceneSdf.cascades.size() > 1) {
        const SceneSdfState::Cascade& fine = _sceneSdf.cascades.front();
        recentre = glm::length(render_camera().position - _sceneSdf.cascadeCentre) >
            0.125f * float(fine.dimensions.x) * fine.voxel;
    }
    if (drawHash == _sceneSdf.drawHash && !_sceneSdf.rebuildRequested && !recentre) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    if (_sceneSdf.rebuildRequested) {
        // A bake may have finished since the last check.
        _sceneSdf.missing.clear();
    }
    _sceneSdf.drawHash = drawHash;
    _sceneSdf.rebuildRequested = false;

    std::error_code directoryError;
    std::filesystem::create_directories(_sceneSdf.queueDirectory, directoryError);

    // Resolve each instance to a loaded mesh volume, queueing unbaked meshes.
    struct Placed {
        const SceneSdfState::MeshVolume* volume;
        glm::mat4 transform;
    };
    std::vector<Placed> placed;
    int queuedNow = 0;
    for (const Instance& instance : instances) {
        auto sourceIt = _traceMeshSources.find(instance.address);
        auto source = sourceIt == _traceMeshSources.end()
            ? nullptr : sourceIt->second.lock();
        if (!source) {
            continue;
        }
        // Which surfaces of a mesh are opaque can differ between instances
        // (a scene material override), so the lookup key includes them.
        uint64_t sourceKey = FnvOffset;
        fnv_append(sourceKey, &instance.address, sizeof(instance.address));
        fnv_append(sourceKey, instance.ranges.data(),
            instance.ranges.size() * sizeof(IndexRange));
        auto hashIt = _sceneSdf.meshHashes.find(sourceKey);
        if (hashIt == _sceneSdf.meshHashes.end()) {
            hashIt = _sceneSdf.meshHashes.emplace(sourceKey, geometry_hash(
                extract_opaque_geometry(*source, instance.ranges))).first;
        }
        const uint64_t hash = hashIt->second;

        auto volumeIt = _sceneSdf.volumes.find(hash);
        if (volumeIt == _sceneSdf.volumes.end() &&
            !_sceneSdf.missing.contains(hash)) {
            const std::string name = hash_name(hash);
            SdfFile file;
            std::string error;
            if (read_sdf_file(_sceneSdf.cacheDirectory + "/" + name + ".sdf",
                    file, error)) {
                SceneSdfState::MeshVolume volume;
                VkFormat format = VK_FORMAT_UNDEFINED;
                if (upload_sdf_voxels(file.voxels, file.dimensions, volume.image, format)) {
                    volume.boundsMin = file.boundsMin;
                    volume.boundsMax = file.boundsMax;
                    volume.bakeVoxel = (file.boundsMax.x - file.boundsMin.x) /
                        float(file.dimensions.x);
                    volumeIt = _sceneSdf.volumes.emplace(hash, volume).first;
                }
            } else {
                _sceneSdf.missing.insert(hash);
                const std::string objPath =
                    _sceneSdf.queueDirectory + "/" + name + ".obj";
                if (!std::filesystem::exists(objPath) &&
                    write_mesh_obj(objPath,
                        extract_opaque_geometry(*source, instance.ranges))) {
                    ++queuedNow;
                }
            }
        }
        if (volumeIt != _sceneSdf.volumes.end()) {
            placed.push_back({&volumeIt->second, instance.transform});
        }
    }
    _sceneSdf.instanceCount = static_cast<int>(instances.size());
    _sceneSdf.placedCount = static_cast<int>(placed.size());

    const auto finishStatus = [&](const std::string& detail) {
        _sceneSdf.buildMilliseconds = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        _sceneSdf.status = fmt::format(
            "{} of {} instances placed, {} meshes awaiting bake ({} newly "
            "queued). {}",
            _sceneSdf.placedCount, _sceneSdf.instanceCount,
            _sceneSdf.missing.size(), queuedNow, detail);
        fmt::print("Scene SDF: {} Built in {:.0f} ms.\n", _sceneSdf.status, _sceneSdf.buildMilliseconds);
    };

    if (placed.empty()) {
        _sceneSdf.fieldValid = false;
        finishStatus("Nothing to merge.");
        return;
    }

    // World bounds of every placed volume, padded by two field voxels.
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    for (const Placed& p : placed) {
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 local(
                corner & 1 ? p.volume->boundsMax.x : p.volume->boundsMin.x,
                corner & 2 ? p.volume->boundsMax.y : p.volume->boundsMin.y,
                corner & 4 ? p.volume->boundsMax.z : p.volume->boundsMin.z);
            const glm::vec3 world = glm::vec3(p.transform * glm::vec4(local, 1.0f));
            lo = glm::min(lo, world);
            hi = glm::max(hi, world);
        }
    }
    // The field is a stack of cascades (shaders/scene_field.glsl).  The last
    // covers every placed volume at up to maxDimension voxels a side, as the
    // single field did, and never moves.  Before it, cascades of fineVoxel,
    // twice that, and so on follow the camera, fineDimension voxels a side,
    // for as long as each is meaningfully finer than the whole-scene one: a
    // shell sits three quarters of a field voxel in front of its surface, so
    // a large scene's hits land early by that much unless the field near the
    // camera is fine.  lumen_lite_design.md has the measurement that sized
    // these.  A scene small enough to be fine as a whole gets one cascade.
    constexpr int Pad = 2;
    const glm::vec3 extent = hi - lo;
    const int maxDimension = std::max(16, _sceneSdf.maxDimension);
    const float baseVoxel = std::max(
        std::max({extent.x, extent.y, extent.z}) / float(maxDimension - 2 * Pad),
        1e-4f);
    const glm::vec3 camera = render_camera().position;
    std::vector<SceneSdfState::Cascade> cascades;
    for (float fine = _sceneSdf.fineVoxel;
         cascades.size() + 1 < SceneSdfState::MaxCascades && fine * 1.5f < baseVoxel;
         fine *= 2.0f) {
        const float span = float(_sceneSdf.fineDimension) * fine;
        // On the cascade grid, so a recentred cascade samples the same
        // points the old one did where the two overlap.
        const glm::vec3 min = glm::floor((camera - glm::vec3(0.5f * span)) / fine) * fine;
        cascades.push_back({min, fine, glm::uvec3(_sceneSdf.fineDimension), 0u});
    }
    {
        const glm::uvec3 dimensions = glm::uvec3(glm::min(
            glm::ceil(extent / baseVoxel) + glm::vec3(2 * Pad), glm::vec3(maxDimension)));
        const glm::vec3 centre = (lo + hi) * 0.5f;
        cascades.push_back({centre - glm::vec3(dimensions) * baseVoxel * 0.5f,
            baseVoxel, dimensions, 0u});
    }
    glm::uvec3 stacked(0u);
    for (SceneSdfState::Cascade& cascade : cascades) {
        cascade.zOffset = stacked.z;
        stacked = glm::uvec3(glm::max(stacked.x, cascade.dimensions.x),
            glm::max(stacked.y, cascade.dimensions.y), stacked.z + cascade.dimensions.z);
    }
    const SceneSdfState::Cascade& base = cascades.back();
    _sceneSdf.voxelSize = baseVoxel;
    _sceneSdf.fieldMin = base.min;
    _sceneSdf.fieldMax = base.min + glm::vec3(base.dimensions) * baseVoxel;
    _sceneSdf.cascades = cascades;
    _sceneSdf.cascadeCentre = camera;

    // The merge reads and writes the field in place, so it needs a storage
    // format the shader can name; r8 is the one sdf_composite.comp uses.
    // Eight bits of a distance truncated at maxDistanceVoxels resolve 1/32 of
    // a voxel, and cost a quarter of R32F: the fine cascade is 128 MB.
    const VkFormatFeatureFlags2 fieldFeatures =
        VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (pick_format(_chosenGPU, {VK_FORMAT_R8_UNORM}, fieldFeatures).format ==
        VK_FORMAT_UNDEFINED) {
        _sceneSdf.fieldValid = false;
        finishStatus("This device cannot store and filter an R8 volume.");
        return;
    }

    // Anything submitted earlier may still sample the old field.
    VK_CHECK(vkDeviceWaitIdle(_device));
    if (_sceneSdf.field.image == VK_NULL_HANDLE ||
        stacked != _sceneSdf.fieldDimensions) {
        if (_sceneSdf.field.image != VK_NULL_HANDLE) {
            destroy_image(_sceneSdf.field);
        }
        _sceneSdf.field = create_image(
            VkExtent3D{stacked.x, stacked.y, stacked.z},
            VK_FORMAT_R8_UNORM,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        _sceneSdf.fieldDimensions = stacked;
    }

    // Everything starts as far as a cascade stores: 1.
    // ponytail: a recentre rebuilds every cascade whole, the static one
    // included; scroll the fine ones in slabs if recentring ever hitches.
    immediate_submit([&](VkCommandBuffer cmd) {
        vkutil::transition_image(cmd, _sceneSdf.field.image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        VkClearColorValue clear{};
        clear.float32[0] = 1.0f;
        const VkImageSubresourceRange range =
            vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
        vkCmdClearColorImage(
            cmd, _sceneSdf.field.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
    });

    std::array<DescriptorAllocatorGrowable::PoolSizeRatio, 2> ratios{{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1.0f},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1.0f}}};
    DescriptorAllocatorGrowable pool;
    pool.init(_device, static_cast<uint32_t>(placed.size() * cascades.size()), ratios);

    // Submitted in batches of bounded voxel work, so one long dispatch list
    // cannot run into the driver's GPU timeout on a large scene.
    constexpr uint64_t VoxelBudget = 24'000'000;
    for (const SceneSdfState::Cascade& cascade : cascades) {
    const float voxel = cascade.voxel;
    const glm::uvec3 dimensions = cascade.dimensions;
    const glm::vec3 fieldMin = cascade.min;
    const float maxDistance = _sceneSdf.maxDistanceVoxels * voxel;
    size_t next = 0;
    while (next < placed.size()) {
        struct Job {
            VkDescriptorSet set;
            SdfCompositePushConstants push;
        };
        std::vector<Job> jobs;
        uint64_t voxels = 0;
        for (; next < placed.size() && (jobs.empty() || voxels < VoxelBudget); ++next) {
            const Placed& p = placed[next];
            glm::vec3 boxLo(std::numeric_limits<float>::max());
            glm::vec3 boxHi(std::numeric_limits<float>::lowest());
            for (int corner = 0; corner < 8; ++corner) {
                const glm::vec3 local(
                    corner & 1 ? p.volume->boundsMax.x : p.volume->boundsMin.x,
                    corner & 2 ? p.volume->boundsMax.y : p.volume->boundsMin.y,
                    corner & 4 ? p.volume->boundsMax.z : p.volume->boundsMin.z);
                const glm::vec3 world = glm::vec3(p.transform * glm::vec4(local, 1.0f));
                boxLo = glm::min(boxLo, world);
                boxHi = glm::max(boxHi, world);
            }
            // Beyond the truncation distance this instance cannot lower any
            // voxel below the value it was cleared to.
            boxLo -= glm::vec3(maxDistance);
            boxHi += glm::vec3(maxDistance);
            const glm::ivec3 first = glm::clamp(
                glm::ivec3(glm::floor((boxLo - fieldMin) / voxel)),
                glm::ivec3(0), glm::ivec3(dimensions));
            const glm::ivec3 last = glm::clamp(
                glm::ivec3(glm::ceil((boxHi - fieldMin) / voxel)),
                glm::ivec3(0), glm::ivec3(dimensions));
            const glm::ivec3 size = last - first;
            if (size.x <= 0 || size.y <= 0 || size.z <= 0) {
                continue;
            }

            const glm::mat4 worldToLocal = glm::inverse(p.transform);
            const glm::mat3 linear(p.transform);
            const float smallestScale = std::min({
                glm::length(linear[0]), glm::length(linear[1]),
                glm::length(linear[2])});

            Job job{};
            job.push.worldToLocal0 = glm::row(worldToLocal, 0);
            job.push.worldToLocal1 = glm::row(worldToLocal, 1);
            job.push.worldToLocal2 = glm::row(worldToLocal, 2);
            job.push.volumeMin = glm::vec4(p.volume->boundsMin, smallestScale);
            job.push.volumeMax = glm::vec4(p.volume->boundsMax, maxDistance);
            job.push.fieldOrigin = glm::vec4(fieldMin, voxel);
            job.push.regionOffset = first;
            // A shell thinner than the scene field can resolve is invisible to
            // it: trilinear filtering between voxel centres on either side of
            // the surface never dips below the hit threshold, so rays pass
            // straight through.  That happens whenever the field voxel is
            // coarser than the bake's, and in the extreme when non-uniform
            // scale squashes one baked mesh -- a unit cube scaled to a 20 cm
            // wall has 5 mm shells -- which let daylight into a sealed room.
            // Thickening to at least three quarters of a field voxel keeps
            // every surface solid to the march at the cost of surfaces sitting
            // that much further forward.
            const float shellHalf = 0.5f * p.volume->bakeVoxel * smallestScale;
            job.push.shellPad = std::max(0.0f, 0.75f * voxel - shellHalf);
            job.push.regionSize = glm::ivec4(size, int(cascade.zOffset));

            job.set = pool.allocate(_device, _sceneSdf.compositeLayout);
            DescriptorWriter writer;
            writer.write_image(0, _sceneSdf.field.imageView, VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            writer.write_image(1, p.volume->image.imageView, _sdf.sampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            writer.update_set(_device, job.set);
            jobs.push_back(job);
            voxels += uint64_t(size.x) * uint64_t(size.y) * uint64_t(size.z);
        }
        if (jobs.empty()) {
            continue;
        }
        immediate_submit([&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(
                cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _sceneSdf.compositePipeline);
            for (const Job& job : jobs) {
                // Each dispatch is a load-min-store on voxels a neighbouring
                // instance's region also covers.  Unordered, two of them race
                // and one minimum is lost -- which one depends on scheduling,
                // so the field differed from run to run.
                vkutil::memory_barrier(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    _sceneSdf.compositePipelineLayout, 0, 1, &job.set, 0, nullptr);
                vkCmdPushConstants(cmd, _sceneSdf.compositePipelineLayout,
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(job.push), &job.push);
                vkCmdDispatch(cmd,
                    (uint32_t(job.push.regionSize.x) + 7) / 8,
                    (uint32_t(job.push.regionSize.y) + 7) / 8,
                    (uint32_t(job.push.regionSize.z) + 7) / 8);
            }
        });
    }
    }

    // The table every tracer reads the cascades from.
    SceneFieldCascades table{};
    table.info = glm::vec4(float(cascades.size()), _sceneSdf.maxDistanceVoxels, 0.0f, 0.0f);
    for (size_t i = 0; i < cascades.size(); ++i) {
        table.cascade[i].origin = glm::vec4(cascades[i].min, cascades[i].voxel);
        table.cascade[i].size = glm::vec4(
            glm::vec3(cascades[i].dimensions), float(cascades[i].zOffset));
    }
    std::memcpy(_sceneSdf.cascadeBuffer.info.pMappedData, &table, sizeof(table));

    immediate_submit([&](VkCommandBuffer cmd) {
        vkutil::transition_image(cmd, _sceneSdf.field.image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    pool.destroy_pools(_device);
    _sceneSdf.fieldValid = true;

    std::string detail = "Field";
    for (size_t i = 0; i < cascades.size(); ++i) {
        detail += fmt::format("{} {}x{}x{} at {:.1f} cm{}", i ? "," : "",
            cascades[i].dimensions.x, cascades[i].dimensions.y, cascades[i].dimensions.z,
            cascades[i].voxel * 100.0f, i + 1 < cascades.size() ? " round the camera" : "");
    }
    finishStatus(detail + ".");
}

void VulkanEngine::draw_sdf_debug(VkCommandBuffer cmd)
{
    if (_sdf.pipeline == VK_NULL_HANDLE) {
        return;
    }
    const bool sceneField = _sdf.source == 1;
    if (sceneField ? !_sceneSdf.fieldValid : !_sdf.loaded) {
        return;
    }
    const AllocatedImage& volume = sceneField ? _sceneSdf.field : _sdf.volume;
    const glm::vec3 boundsMin = sceneField
        ? _sceneSdf.fieldMin : _sdf.boundsMin + _sdf.offset;
    const glm::vec3 boundsMax = sceneField
        ? _sceneSdf.fieldMax : _sdf.boundsMax + _sdf.offset;

    // Allocated from this frame's pool, so a resized draw image or a newly
    // loaded volume is picked up without rewriting a set still in flight.
    VkDescriptorSet set = get_current_frame()._frameDescriptors.allocate(
        _device, _sdf.descriptorLayout);
    DescriptorWriter writer;
    writer.write_image(0, _drawImage.imageView, VK_NULL_HANDLE,
        VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    writer.write_image(1, volume.imageView, _sdf.sampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.write_image(2, _prepass.depthImage.imageView, _prepass.sampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.write_buffer(3, _sceneSdf.cascadeBuffer.buffer, sizeof(SceneFieldCascades), 0,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    writer.update_set(_device, set);

    SdfDebugPushConstants push{};
    push.inverseViewProjection = glm::inverse(sceneData.viewproj);
    push.cameraPosition = glm::vec4(render_camera().position, 1.0f);
    push.boundsMin = glm::vec4(boundsMin, static_cast<float>(_sdf.maxSteps));
    push.boundsMax = glm::vec4(boundsMax, static_cast<float>(_sdf.viewMode));
    push.extent = glm::vec4(
        static_cast<float>(_drawExtent.width),
        static_cast<float>(_drawExtent.height),
        _sdf.hitThresholdTexels,
        sceneField ? 1.0f : 0.0f);

    vkutil::transition_image(cmd, _drawImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _sdf.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        _sdf.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, _sdf.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(push), &push);
    vkCmdDispatch(cmd,
        (_drawExtent.width + 15) / 16, (_drawExtent.height + 15) / 16, 1);
    // Back to where draw() expects the image after a debug view.
    vkutil::transition_image(cmd, _drawImage.image,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

void VulkanEngine::draw_sdf_settings()
{
    ImGui::SeparatorText("Lumen-lite distance field");
    const char* sources[] = {"Single volume", "Scene field"};
    ImGui::Combo("SDF Source", &_sdf.source, sources, IM_ARRAYSIZE(sources));
    const char* modes[] = {
        "Shaded", "Normals", "March steps", "Raster agreement",
        "Raster agreement (data)"};
    ImGui::Combo("SDF View", &_sdf.viewMode, modes, IM_ARRAYSIZE(modes));
    if (_sdf.viewMode == 3) {
        ImGui::TextDisabled("Green <=1 voxel, yellow <=3, red further;");
        ImGui::TextDisabled("blue raster-only, magenta field-only.");
    }
    ImGui::SliderInt("Max March Steps", &_sdf.maxSteps, 8, 512);
    ImGui::SliderFloat(
        "Hit Threshold (texels)", &_sdf.hitThresholdTexels, 0.01f, 2.0f, "%.2f");

    if (_sdf.source == 1) {
        ImGui::TextWrapped("%s", _sceneSdf.status.c_str());
        ImGui::SliderInt("Field Max Dimension", &_sceneSdf.maxDimension, 32, 512);
        ImGui::SliderFloat("Truncation (voxels)",
            &_sceneSdf.maxDistanceVoxels, 2.0f, 64.0f, "%.1f");
        if (ImGui::Button("Rebuild Scene Field")) {
            _sceneSdf.rebuildRequested = true;
        }
        if (!_sceneSdf.missing.empty()) {
            ImGui::TextDisabled("Bake the queue, then rebuild:");
            ImGui::TextDisabled(
                "python scripts/bake_sdf.py --queue assets/sdf/queue assets/sdf/cache");
        }
        return;
    }

    ImGui::TextDisabled("%s", _sdf.status.c_str());
    char pathBuffer[512];
    std::snprintf(pathBuffer, sizeof(pathBuffer), "%s", _sdf.path.c_str());
    if (ImGui::InputText("Volume Path", pathBuffer, sizeof(pathBuffer))) {
        _sdf.path = pathBuffer;
    }
    if (ImGui::Button("Load Volume")) {
        load_sdf_volume(_sdf.path);
    }
    ImGui::DragFloat3("Volume Offset", &_sdf.offset.x, 0.05f);
    if (_sdf.loaded) {
        const glm::vec3 lo = _sdf.boundsMin + _sdf.offset;
        const glm::vec3 hi = _sdf.boundsMax + _sdf.offset;
        ImGui::TextDisabled("World bounds (%.2f, %.2f, %.2f)", lo.x, lo.y, lo.z);
        ImGui::TextDisabled("          .. (%.2f, %.2f, %.2f)", hi.x, hi.y, hi.z);
    }
}

void VulkanEngine::validate_sdf_bake(const char* path)
{
    SdfFile file;
    std::string error;
    if (!read_sdf_file(path, file, error)) {
        fmt::print("SDF validate FAIL: {}\n", error);
        std::abort();
    }
    const size_t voxelCount = file.voxels.size();
    const VkExtent3D extent{
        file.dimensions.x, file.dimensions.y, file.dimensions.z};

    // R32_SFLOAT without filtering: sampled use of it is required of every
    // Vulkan device, and a byte-exact readback needs the full-precision copy.
    AllocatedImage sdfImage = create_image(
        file.voxels.data(), voxelCount * sizeof(float), extent,
        VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT, false);

    VK_CHECK(vkDeviceWaitIdle(_device));
    AllocatedBuffer readback = create_buffer(
        voxelCount * sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_TO_CPU);
    immediate_submit([&](VkCommandBuffer cmd) {
        vkutil::transition_image(cmd, sdfImage.image,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = extent;
        vkCmdCopyImageToBuffer(cmd, sdfImage.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
    });
    void* mapped = nullptr;
    VK_CHECK(vmaMapMemory(_allocator, readback.allocation, &mapped));
    vmaInvalidateAllocation(_allocator, readback.allocation, 0, VK_WHOLE_SIZE);
    const auto* readBack = static_cast<const float*>(mapped);
    float maxError = 0.0f;
    for (size_t i = 0; i < voxelCount; ++i) {
        maxError = std::max(maxError, std::abs(readBack[i] - file.voxels[i]));
    }
    vmaUnmapMemory(_allocator, readback.allocation);
    destroy_buffer(readback);
    destroy_image(sdfImage);

    fmt::print(
        "SDF validate {}: {}x{}x{} voxels, bounds [{},{},{}]..[{},{},{}], "
        "max round-trip error {}\n",
        maxError < 1e-6f ? "PASS" : "FAIL",
        extent.width, extent.height, extent.depth,
        file.boundsMin.x, file.boundsMin.y, file.boundsMin.z,
        file.boundsMax.x, file.boundsMax.y, file.boundsMax.z,
        maxError);
    if (maxError >= 1e-6f) std::abort();
}
