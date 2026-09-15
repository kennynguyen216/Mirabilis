#include "vk_engine.h"

#include <cmath>

#include <stb_image.h>

namespace {

bool nearly_equal(float lhs, float rhs)
{
    return std::abs(lhs - rhs) <= 0.0001f;
}

bool nearly_equal(const glm::vec2& lhs, const glm::vec2& rhs)
{
    return nearly_equal(lhs.x, rhs.x) && nearly_equal(lhs.y, rhs.y);
}

bool nearly_equal(const glm::vec4& lhs, const glm::vec4& rhs)
{
    return nearly_equal(lhs.x, rhs.x) && nearly_equal(lhs.y, rhs.y) &&
        nearly_equal(lhs.z, rhs.z) && nearly_equal(lhs.w, rhs.w);
}

} // namespace

const AllocatedImage& VulkanEngine::load_scene_texture(
    std::string_view texturePath, bool srgb)
{
    if (texturePath.empty()) {
        return _whiteImage;
    }

    // The colour space is part of the key: one file used both as colour and
    // as data needs two GPU images, and sharing one would decode the data.
    const std::string path{texturePath};
    const std::string key = srgb ? path : path + "#linear";
    if (const auto existing = _sceneTextureCache.find(key);
        existing != _sceneTextureCache.end()) {
        return existing->second;
    }
    if (_failedSceneTextures.contains(key)) {
        return _errorCheckerboardImage;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(
        path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        fmt::print("Failed to load scene texture {}: {}\n",
            key, stbi_failure_reason());
        if (pixels != nullptr) {
            stbi_image_free(pixels);
        }
        _failedSceneTextures.insert(key);
        return _errorCheckerboardImage;
    }

    AllocatedImage image = create_image(
        pixels,
        VkExtent3D{
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height),
            1},
        srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT,
        true);
    // The path tracer samples base colour only, so only colour textures keep
    // a CPU copy for it.
    if (srgb) {
        image.traceSource = make_trace_texture(pixels, image.imageExtent);
    }
    stbi_image_free(pixels);

    auto [inserted, unused] = _sceneTextureCache.emplace(key, image);
    fmt::print("Loaded scene texture: {}\n", key);
    return inserted->second;
}

MaterialInstance* VulkanEngine::resolve_scene_material(const SceneObject& object)
{
    SceneMaterialRuntime& runtime = _sceneMaterialRuntimes[object.id];
    const SceneMaterial& source = object.material;
    runtime.material.traceEmission=glm::vec4(source.emissionColor*source.emissionStrength,0);
    runtime.material.traceParameters=glm::vec4(source.metallic,source.roughness,source.transmission,source.ior);
    runtime.material.traceUVScale=source.uvScale;
    const glm::vec4 emission(source.emissionColor * source.emissionStrength, 0.0f);
    const bool changed = !runtime.initialized ||
        !nearly_equal(runtime.emission, emission) ||
        runtime.texturePath != source.baseColorTexturePath ||
        runtime.normalTexturePath != source.normalTexturePath ||
        runtime.metalRoughTexturePath != source.metalRoughTexturePath ||
        !nearly_equal(runtime.colorTint, source.colorTint) ||
        !nearly_equal(runtime.uvScale, source.uvScale) ||
        !nearly_equal(runtime.metallic, source.metallic) ||
        !nearly_equal(runtime.roughness, source.roughness) ||
        runtime.debugChecker != source.debugChecker;
    if (!changed) {
        return &runtime.material;
    }

    // Material edits occur only in the editor and are intentionally rare.
    // Waiting here makes overwriting a mapped UBO/descriptor safe even when a
    // previous frame is still using it.
    VK_CHECK(vkDeviceWaitIdle(_device));
    if (!runtime.initialized) {
        runtime.constantsBuffer = create_buffer(
            sizeof(GLTFMetallic_Roughness::MaterialConstants),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU);
    }

    auto* constants =
        static_cast<GLTFMetallic_Roughness::MaterialConstants*>(
            runtime.constantsBuffer.info.pMappedData);
    *constants = {};
    constants->colorFactors = source.colorTint;
    constants->metal_rough_factors = glm::vec4(
        source.metallic, source.roughness, 0.0f, 0.0f);
    // x = normal-map scale: glTF's default of 1 with a map, and 0 without one.
    // The flat fallback texel is 128/255, not exactly 0.5, so scaling it by 1
    // would tilt every normal slightly; 0 keeps the geometric normal exact.
    constants->materialFlags = glm::vec4(
        source.normalTexturePath.empty() ? 0.0f : 1.0f,
        source.debugChecker ? 1.0f : 0.0f,
        0.0f,
        0.0f);
    constants->uvTransform = glm::vec4(source.uvScale, 0.0f, 0.0f);
    constants->emission = emission;

    const AllocatedImage& colorImage =
        load_scene_texture(source.baseColorTexturePath);
    GLTFMetallic_Roughness::MaterialResources resources{};
    resources.colorImage = colorImage;
    resources.colorSampler = _defaultSamplerLinear;
    // Both are data, so they load linear.  White metallic/roughness leaves the
    // factors as they are; the flat normal leaves the geometric normal.
    resources.metalRoughImage = source.metalRoughTexturePath.empty()
        ? _whiteImage
        : load_scene_texture(source.metalRoughTexturePath, false);
    resources.metalRoughSampler = _defaultSamplerLinear;
    resources.normalImage = source.normalTexturePath.empty()
        ? _flatNormalImage
        : load_scene_texture(source.normalTexturePath, false);
    resources.normalSampler = _defaultSamplerLinear;
    resources.dataBuffer = runtime.constantsBuffer.buffer;
    resources.dataBufferOffset = 0;

    if (!runtime.initialized) {
        runtime.material = metalRoughMaterial.write_material(
            _device,
            MaterialPass::MainColor,
            resources,
            globalDescriptorAllocator);
    } else {
        // The same writes write_material() makes, so a binding added there
        // cannot be forgotten here.
        metalRoughMaterial.write_material_set(
            _device, resources, runtime.material.materialSet);
    }

    runtime.material.traceBaseColor = source.colorTint;
    runtime.material.traceEmission=glm::vec4(source.emissionColor*source.emissionStrength,0);
    runtime.material.traceParameters = glm::vec4(source.metallic,source.roughness,source.transmission,source.ior);
    runtime.material.traceTexture=colorImage.traceSource;
    runtime.material.traceUVScale=source.uvScale;
    runtime.texturePath = source.baseColorTexturePath;
    runtime.normalTexturePath = source.normalTexturePath;
    runtime.metalRoughTexturePath = source.metalRoughTexturePath;
    runtime.colorTint = source.colorTint;
    runtime.uvScale = source.uvScale;
    runtime.metallic = source.metallic;
    runtime.roughness = source.roughness;
    runtime.emission = emission;
    runtime.debugChecker = source.debugChecker;
    runtime.initialized = true;
    return &runtime.material;
}

void VulkanEngine::clear_scene_material_resources()
{
    for (auto& [unused, runtime] : _sceneMaterialRuntimes) {
        if (runtime.constantsBuffer.buffer != VK_NULL_HANDLE) {
            destroy_buffer(runtime.constantsBuffer);
        }
    }
    _sceneMaterialRuntimes.clear();

    for (auto& [unused, texture] : _sceneTextureCache) {
        if (texture.image != VK_NULL_HANDLE) {
            destroy_image(texture);
        }
    }
    _sceneTextureCache.clear();
    _failedSceneTextures.clear();
}
