#include "vk_engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include <glm/packing.hpp>

#include <stb_image.h>

#include <vk_images.h>
#include <vk_initializers.h>

void VulkanEngine::init_default_images_and_samplers()
{
    uint32_t white = glm::packUnorm4x8(glm::vec4(1.0f));
    uint32_t grey = glm::packUnorm4x8(glm::vec4(0.66f, 0.66f, 0.66f, 1.0f));
    uint32_t black = glm::packUnorm4x8(glm::vec4(0.0f));
    _whiteImage = create_image(
        &white, {1, 1, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
    _greyImage = create_image(
        &grey, {1, 1, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
    _blackImage = create_image(
        &black, {1, 1, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

    std::array<uint32_t, 16 * 16> checkerboard{};
    uint32_t magenta = glm::packUnorm4x8(glm::vec4(1, 0, 1, 1));
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            checkerboard[y * 16 + x] = ((x % 2) ^ (y % 2)) ? magenta : black;
        }
    }
    _errorCheckerboardImage = create_image(
        checkerboard.data(),
        {16, 16, 1},
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT);
    
    VkSamplerCreateInfo samplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(_device, &samplerInfo, nullptr, &_defaultSamplerNearest));
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    // This is the fallback material sampler for every glTF texture whose
    // sampler index is absent, so it needs the same anisotropy the explicit
    // ones get; without it a Sponza material with no sampler declaration
    // would blur at exactly the grazing angles the others stay sharp at.
    const float anisotropy = material_anisotropy();
    samplerInfo.anisotropyEnable = anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    samplerInfo.maxAnisotropy = anisotropy;
    VK_CHECK(vkCreateSampler(_device, &samplerInfo, nullptr, &_defaultSamplerLinear));
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    
    // The supplied asset is a 2:1 equirectangular panorama.  Horizontal
    // wrapping joins its left/right edges; clamping vertically avoids pulling
    // texels from the opposite pole when looking straight up or down.
    VkSamplerCreateInfo skyboxSamplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    skyboxSamplerInfo.magFilter = VK_FILTER_LINEAR;
    skyboxSamplerInfo.minFilter = VK_FILTER_LINEAR;
    skyboxSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    skyboxSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    skyboxSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    skyboxSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    skyboxSamplerInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(_device, &skyboxSamplerInfo, nullptr, &_skyboxSampler));
    // The same panorama, but readable past level 0.  Only the SSGI trace uses
    // it: the visible background must stay at full sharpness, while a traced
    // miss ray needs a mip coarse enough that a sun disk does not arrive as a
    // firefly in a four-ray-per-pixel estimate.
    skyboxSamplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(
        _device, &skyboxSamplerInfo, nullptr, &_skyboxEnvironmentSampler));
    
    if (!set_skybox(_skyboxSelection) && !set_skybox(0)) {
        // Keep every descriptor valid even if all packaged sky assets are
        // missing.  The individual load failures above identify the paths.
        const uint32_t fallbackSky =
            glm::packUnorm4x8(glm::vec4(0.25f, 0.45f, 0.75f, 1.0f));
        _skyboxImage = create_image(
            const_cast<uint32_t*>(&fallbackSky),
            {1, 1, 1},
            VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_SAMPLED_BIT);
        _skyboxSelection = 0;
        // set_skybox() does this itself on the paths that succeed; the sets
        // that name the panorama have to be written on this one too.
        update_skybox_descriptors();
    }
    
    // The compute background needs the panorama at binding 1.  The portal
    // graphics pass uses its own single-image descriptor at set 0.
    DescriptorWriter skyboxWriter;
    skyboxWriter.write_image(
        1,
        _skyboxImage.imageView,
        _skyboxSampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    skyboxWriter.update_set(_device, _drawImageDescriptors);
    
    _skyboxDescriptor = globalDescriptorAllocator.allocate(
        _device, _singleImageDescriptorLayout);
    skyboxWriter.clear();
    skyboxWriter.write_image(
        0,
        _skyboxImage.imageView,
        _skyboxSampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    skyboxWriter.update_set(_device, _skyboxDescriptor);

    write_ssgi_trace_descriptors();
}

bool VulkanEngine::set_skybox(int selection)
{
    if (selection < 0 ||
        selection >= static_cast<int>(SkyboxPaths.size())) {
        return false;
    }

    const char* path = SkyboxPaths[selection];
    int width = 0;
    int height = 0;
    int channels = 0;
    AllocatedImage newImage{};

    if (stbi_is_hdr(path)) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(
            _chosenGPU, VK_FORMAT_R16G16B16A16_SFLOAT, &properties);
        constexpr VkFormatFeatureFlags RequiredFeatures =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((properties.optimalTilingFeatures & RequiredFeatures) !=
            RequiredFeatures) {
            fmt::print(
                "Cannot load HDR skybox {}: RGBA16F sampling/filtering is unsupported\n",
                path);
            return false;
        }

        float* pixels = stbi_loadf(
            path, &width, &height, &channels, STBI_rgb_alpha);
        if (pixels == nullptr) {
            fmt::print(
                "Failed to load HDR skybox {}: {}\n",
                path,
                stbi_failure_reason() != nullptr
                    ? stbi_failure_reason()
                    : "unknown");
            return false;
        }

        const size_t pixelCount =
            static_cast<size_t>(width) * static_cast<size_t>(height);
        std::vector<uint32_t> halfPixels(pixelCount * 2);
        const auto finiteHalf = [](float value) {
            return std::isfinite(value)
                ? std::clamp(value, 0.0f, 65504.0f)
                : 0.0f;
        };
        for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
            const float* source = pixels + pixel * 4;
            halfPixels[pixel * 2] = glm::packHalf2x16(glm::vec2(
                finiteHalf(source[0]), finiteHalf(source[1])));
            halfPixels[pixel * 2 + 1] = glm::packHalf2x16(glm::vec2(
                finiteHalf(source[2]), 1.0f));
        }
        // How bright the sky gets before the sun disk takes over.  Below
        // this the panorama is sky; above it, almost every pixel belongs to
        // the sun, whose light the direct term already delivers with a
        // shadow map.  A percentile rather than a fixed number because the
        // ratio between the two is a property of the capture.
        {
            std::vector<float> luminance(pixelCount);
            for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
                const float* source = pixels + pixel * 4;
                luminance[pixel] = 0.2126f * source[0] +
                    0.7152f * source[1] + 0.0722f * source[2];
            }
            const size_t rank = static_cast<size_t>(
                static_cast<double>(pixelCount) * 0.999);
            std::nth_element(
                luminance.begin(),
                luminance.begin() + static_cast<std::ptrdiff_t>(rank),
                luminance.end());
            // A panorama with no sun in it must not have its sky clipped, so
            // the ceiling never drops below plain white.
            _skyboxIndirectClamp = std::max(1.0f, luminance[rank]);
        }
        stbi_image_free(pixels);

        newImage = create_image(
            halfPixels.data(),
            halfPixels.size() * sizeof(uint32_t),
            {static_cast<uint32_t>(width),
             static_cast<uint32_t>(height),
             1},
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_SAMPLED_BIT,
            true);
        fmt::print(
            "Indirect sky/sun split at {:.3f}\n", _skyboxIndirectClamp);
        fmt::print(
            "Loaded HDR equirectangular skybox: {} ({}x{}, RGBA16F)\n",
            path,
            width,
            height);
    } else {
        stbi_uc* pixels = stbi_load(
            path, &width, &height, &channels, STBI_rgb_alpha);
        if (pixels == nullptr) {
            fmt::print(
                "Failed to load skybox {}: {}\n",
                path,
                stbi_failure_reason() != nullptr
                    ? stbi_failure_reason()
                    : "unknown");
            return false;
        }
        // An 8-bit panorama decodes into [0, 1] and has no sun disk to
        // separate from its sky, so nothing here needs clipping.
        _skyboxIndirectClamp = 1.0e4f;
        newImage = create_image(
            pixels,
            {static_cast<uint32_t>(width),
             static_cast<uint32_t>(height),
             1},
            VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_SAMPLED_BIT,
            true);
        stbi_image_free(pixels);
        fmt::print(
            "Loaded equirectangular skybox: {} ({}x{})\n",
            path,
            width,
            height);
    }

    // A cosine-weighted indirect ray stands for a wide cone.  Reading it
    // from a mip about this wide costs one sample and removes almost all of
    // the variance the full-resolution panorama would contribute; anything
    // sharper arrives as fireflies that survive both SSGI filters.  Derived
    // from the panorama actually loaded, so a 1x1 fallback asks for level 0.
    constexpr float EnvironmentSampleWidth = 64.0f;
    _skyboxEnvironmentLod = std::max(
        0.0f,
        std::log2(
            static_cast<float>(std::max(width, 1)) / EnvironmentSampleWidth));

    VK_CHECK(vkDeviceWaitIdle(_device));
    AllocatedImage oldImage = _skyboxImage;
    _skyboxImage = std::move(newImage);
    _skyboxSelection = selection;
    update_skybox_descriptors();
    _ssgiHistoryValid = false;
    destroy_image(oldImage);
    return true;
}

void VulkanEngine::update_skybox_descriptors()
{
    if (_skyboxImage.imageView == VK_NULL_HANDLE) {
        return;
    }

    DescriptorWriter writer;
    if (_drawImageDescriptors != VK_NULL_HANDLE) {
        writer.write_image(
            1,
            _skyboxImage.imageView,
            _skyboxSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writer.update_set(_device, _drawImageDescriptors);
        writer.clear();
    }
    if (_skyboxDescriptor != VK_NULL_HANDLE) {
        writer.write_image(
            0,
            _skyboxImage.imageView,
            _skyboxSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writer.update_set(_device, _skyboxDescriptor);
        writer.clear();
    }
    for (VkDescriptorSet descriptor : _ssgiDescriptors) {
        if (descriptor == VK_NULL_HANDLE) {
            continue;
        }
        writer.write_image(
            7,
            _skyboxImage.imageView,
            _skyboxEnvironmentSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writer.update_set(_device, descriptor);
        writer.clear();
    }
    // Every camera's scene set, main and portal alike.  A portal camera reads
    // the panorama from its forward shader rather than from a screen-space
    // trace it cannot run, and both have to be looking at the same sky or the
    // destination room changes colour when the player crosses.  These sets are
    // allocated by init_descriptors(), which runs before the panorama exists,
    // so this is the only place binding 3 is ever written.  The mip-complete
    // sampler is the one to use: a hemisphere average reads a coarse level.
    for (FrameData& frame : _frames) {
        const auto writeEnvironment = [&](VkDescriptorSet descriptor) {
            if (descriptor == VK_NULL_HANDLE) {
                return;
            }
            writer.write_image(
                3,
                _skyboxImage.imageView,
                _skyboxEnvironmentSampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            writer.update_set(_device, descriptor);
            writer.clear();
        };
        writeEnvironment(frame.sceneDescriptor);
        for (VkDescriptorSet descriptor : frame.portalSceneDescriptors) {
            writeEnvironment(descriptor);
        }
    }
}

void VulkanEngine::init_default_meshes()
{
    std::array<uint32_t, 6> indices{};
    std::array<Vertex, 4> corners{};
    // A unit quad. The floor scene object scales it to the arena size, so
    // the rendered floor and the walkable ground plane cannot disagree.
    corners[0].position = glm::vec3(-0.5f, 0.f, -0.5f);
    corners[0].uv_x = 0.f;
    corners[0].uv_y = 0.f;
    corners[0].normal = glm::vec3(0.f, 1.f, 0.f);
    corners[0].color = glm::vec4(1.f);
    
    corners[1].position = glm::vec3(0.5f, 0.f, -0.5f);
    corners[1].uv_x = 1.f;
    corners[1].uv_y = 0.f;
    corners[1].normal = glm::vec3(0.f, 1.f, 0.f);
    corners[1].color = glm::vec4(1.f);
    
    corners[2].position = glm::vec3(0.5f, 0.f, 0.5f);
    corners[2].uv_x = 1.f;
    corners[2].uv_y = 1.f;
    corners[2].normal = glm::vec3(0.f, 1.f, 0.f);
    corners[2].color = glm::vec4(1.f);
    
    corners[3].position = glm::vec3(-0.5f, 0.f, 0.5f);
    corners[3].uv_x = 0.f;
    corners[3].uv_y = 1.f;
    corners[3].normal = glm::vec3(0.f, 1.f, 0.f);
    corners[3].color = glm::vec4(1.f);
    
    indices[0] = 0;
    indices[1] = 2;
    indices[2] = 1;
    
    indices[3] = 0;
    indices[4] = 3;
    indices[5] = 2;
    
    _floorMesh = uploadMesh(indices, corners);
    
    _floorBounds.origin = glm::vec3(0.0f);
    _floorBounds.extents = glm::vec3(0.5f, 0.0f, 0.5f);
    _floorBounds.sphereRadius = glm::length(_floorBounds.extents);
    
    std::vector<Vertex> wallVertices;
    std::vector<uint32_t> wallIndices;
    const auto makeWallVertex = [](const glm::vec3& position,
                                   const glm::vec3& normal,
                                   float u,
                                   float v) {
        Vertex vertex{};
        vertex.position = position;
        vertex.normal = normal;
        vertex.uv_x = u;
        vertex.uv_y = v;
        vertex.color = glm::vec4(1.0f);
        return vertex;
    };
    const auto addWallFace = [&](const glm::vec3& a,
                                 const glm::vec3& b,
                                 const glm::vec3& c,
                                 const glm::vec3& d,
                                 const glm::vec3& normal) {
        const uint32_t first = static_cast<uint32_t>(wallVertices.size());
        wallVertices.push_back(makeWallVertex(a, normal, 0.0f, 0.0f));
        wallVertices.push_back(makeWallVertex(b, normal, 1.0f, 0.0f));
        wallVertices.push_back(makeWallVertex(c, normal, 1.0f, 1.0f));
        wallVertices.push_back(makeWallVertex(d, normal, 0.0f, 1.0f));
        wallIndices.insert(wallIndices.end(), {
            first, first + 1, first + 2,
            first, first + 2, first + 3});
    };
    
    constexpr float half = 0.5f;
    addWallFace({-half, -half, half}, {half, -half, half},
                {half, half, half}, {-half, half, half}, {0.0f, 0.0f, 1.0f});
    addWallFace({half, -half, -half}, {-half, -half, -half},
                {-half, half, -half}, {half, half, -half}, {0.0f, 0.0f, -1.0f});
    addWallFace({half, -half, half}, {half, -half, -half},
                {half, half, -half}, {half, half, half}, {1.0f, 0.0f, 0.0f});
    addWallFace({-half, -half, -half}, {-half, -half, half},
                {-half, half, half}, {-half, half, -half}, {-1.0f, 0.0f, 0.0f});
    addWallFace({-half, half, -half}, {-half, half, half},
                {half, half, half}, {half, half, -half}, {0.0f, 1.0f, 0.0f});
    addWallFace({-half, -half, half}, {-half, -half, -half},
                {half, -half, -half}, {half, -half, half}, {0.0f, -1.0f, 0.0f});
    
    _wallMesh = uploadMesh(wallIndices, wallVertices);
    _wallBounds.origin = glm::vec3(0.0f);
    _wallBounds.extents = glm::vec3(0.5f);
    _wallBounds.sphereRadius = glm::length(_wallBounds.extents);
    
    // Unit wedge used by the Surf Ramp editor asset. It rises along local +Z:
    // the low edge is y=0 at z=-0.5 and the high edge is y=1 at z=0.5.
    std::vector<Vertex> rampVertices;
    std::vector<uint32_t> rampIndices;
    const auto addRampFace = [&](const glm::vec3& a,
                                 const glm::vec3& b,
                                 const glm::vec3& c,
                                 const glm::vec3& d,
                                 const glm::vec3& normal) {
        const uint32_t first = static_cast<uint32_t>(rampVertices.size());
        rampVertices.push_back(makeWallVertex(a, normal, 0.0f, 0.0f));
        rampVertices.push_back(makeWallVertex(b, normal, 1.0f, 0.0f));
        rampVertices.push_back(makeWallVertex(c, normal, 1.0f, 1.0f));
        rampVertices.push_back(makeWallVertex(d, normal, 0.0f, 1.0f));
        rampIndices.insert(rampIndices.end(), {
            first, first + 1, first + 2, first, first + 2, first + 3});
    };
    const auto addRampTriangle = [&](const glm::vec3& a,
                                     const glm::vec3& b,
                                     const glm::vec3& c,
                                     const glm::vec3& normal) {
        const uint32_t first = static_cast<uint32_t>(rampVertices.size());
        rampVertices.push_back(makeWallVertex(a, normal, 0.0f, 0.0f));
        rampVertices.push_back(makeWallVertex(b, normal, 1.0f, 0.0f));
        rampVertices.push_back(makeWallVertex(c, normal, 0.5f, 1.0f));
        rampIndices.insert(rampIndices.end(), {first, first + 1, first + 2});
    };
    const glm::vec3 rampLowLeft{-0.5f, 0.0f, -0.5f};
    const glm::vec3 rampLowRight{0.5f, 0.0f, -0.5f};
    const glm::vec3 rampHighRight{0.5f, 1.0f, 0.5f};
    const glm::vec3 rampHighLeft{-0.5f, 1.0f, 0.5f};
    const glm::vec3 rampBackRight{0.5f, 0.0f, 0.5f};
    const glm::vec3 rampBackLeft{-0.5f, 0.0f, 0.5f};
    addRampFace(rampLowLeft, rampHighLeft, rampHighRight, rampLowRight,
                glm::normalize(glm::vec3(0.0f, 1.0f, -1.0f)));
    addRampFace(rampLowLeft, rampLowRight, rampBackRight, rampBackLeft,
                glm::vec3(0.0f, -1.0f, 0.0f));
    addRampTriangle(rampLowRight, rampHighRight, rampBackRight,
                    glm::vec3(1.0f, 0.0f, 0.0f));
    addRampTriangle(rampLowLeft, rampBackLeft, rampHighLeft,
                    glm::vec3(-1.0f, 0.0f, 0.0f));
    addRampFace(rampBackLeft, rampBackRight, rampHighRight, rampHighLeft,
                glm::vec3(0.0f, 0.0f, 1.0f));
    
    _rampMesh = uploadMesh(rampIndices, rampVertices);
    _rampBounds.origin = glm::vec3(0.0f, 0.5f, 0.0f);
    _rampBounds.extents = glm::vec3(0.5f);
    _rampBounds.sphereRadius = glm::length(_rampBounds.extents);
    
    std::array<Vertex, 4> portalVertices{};
    portalVertices[0] = {
        .position = {-0.5f, -0.5f, 0.0f}, .uv_x = 0.0f,
        .normal = {0.0f, 0.0f, 1.0f}, .uv_y = 0.0f, .color = glm::vec4(1.0f)};
    portalVertices[1] = {
        .position = {0.5f, -0.5f, 0.0f}, .uv_x = 1.0f,
        .normal = {0.0f, 0.0f, 1.0f}, .uv_y = 0.0f, .color = glm::vec4(1.0f)};
    portalVertices[2] = {
        .position = {0.5f, 0.5f, 0.0f}, .uv_x = 1.0f,
        .normal = {0.0f, 0.0f, 1.0f}, .uv_y = 1.0f, .color = glm::vec4(1.0f)};
    portalVertices[3] = {
        .position = {-0.5f, 0.5f, 0.0f}, .uv_x = 0.0f,
        .normal = {0.0f, 0.0f, 1.0f}, .uv_y = 1.0f, .color = glm::vec4(1.0f)};
    std::array<uint32_t, 6> portalIndices{0, 1, 2, 0, 2, 3};
    _portalMesh = uploadMesh(portalIndices, portalVertices);
    _portalBounds.origin = glm::vec3(0.0f);
    _portalBounds.extents = glm::vec3(0.5f, 0.5f, 0.0f);
    _portalBounds.sphereRadius = glm::length(_portalBounds.extents);
}

void VulkanEngine::init_default_materials()
{
    _floorMaterialBuffer = create_buffer(
    sizeof(GLTFMetallic_Roughness::MaterialConstants),
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
    VMA_MEMORY_USAGE_CPU_TO_GPU);
    
    auto* floorConstants =
    static_cast<GLTFMetallic_Roughness::MaterialConstants*>(
        _floorMaterialBuffer.info.pMappedData);
    
    *floorConstants = {};
    floorConstants->colorFactors = glm::vec4(1.0f);
    floorConstants->metal_rough_factors = glm::vec4(0.0f, 0.8f, 1.0f, 0.0f);
    
    GLTFMetallic_Roughness::MaterialResources floorResources{};
    floorResources.colorImage = _whiteImage;
    floorResources.colorSampler = _defaultSamplerLinear;
    floorResources.metalRoughImage = _whiteImage;
    floorResources.metalRoughSampler = _defaultSamplerLinear;
    floorResources.dataBuffer = _floorMaterialBuffer.buffer;
    floorResources.dataBufferOffset = 0;
    
    _floorMaterial = metalRoughMaterial.write_material(
        _device,
        MaterialPass::MainColor,
        floorResources,
        globalDescriptorAllocator);
    _floorMaterial.traceBaseColor = floorConstants->colorFactors;
    _floorMaterial.traceParameters = floorConstants->metal_rough_factors;
    
    _wallMaterialBuffer = create_buffer(
        sizeof(GLTFMetallic_Roughness::MaterialConstants),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    auto* wallConstants = static_cast<GLTFMetallic_Roughness::MaterialConstants*>(
        _wallMaterialBuffer.info.pMappedData);
    *wallConstants = {};
    wallConstants->colorFactors = glm::vec4(0.18f, 0.32f, 0.55f, 1.0f);
    wallConstants->metal_rough_factors = glm::vec4(0.0f, 0.8f, 0.0f, 0.0f);
    
    GLTFMetallic_Roughness::MaterialResources wallResources = floorResources;
    wallResources.dataBuffer = _wallMaterialBuffer.buffer;
    _wallMaterial = metalRoughMaterial.write_material(
        _device,
        MaterialPass::MainColor,
        wallResources,
        globalDescriptorAllocator);
    _wallMaterial.traceBaseColor = wallConstants->colorFactors;
    _wallMaterial.traceParameters = wallConstants->metal_rough_factors;
    
    // There is no character asset in assets/ yet, so start with a visible
    // collision-sized proxy.  It is rendered only by portal cameras; the
    // first-person main camera never sees the box enclosing itself.
    _playerBounds = _wallBounds;
    _playerMaterialBuffer = create_buffer(
        sizeof(GLTFMetallic_Roughness::MaterialConstants),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    auto* playerConstants = static_cast<GLTFMetallic_Roughness::MaterialConstants*>(
        _playerMaterialBuffer.info.pMappedData);
    *playerConstants = {};
    playerConstants->colorFactors = glm::vec4(0.95f, 0.90f, 0.75f, 1.0f);
    playerConstants->metal_rough_factors = glm::vec4(0.0f, 0.9f, 0.0f, 0.0f);
    
    GLTFMetallic_Roughness::MaterialResources playerResources = floorResources;
    playerResources.dataBuffer = _playerMaterialBuffer.buffer;
    _playerMaterial = metalRoughMaterial.write_material(
        _device,
        MaterialPass::MainColor,
        playerResources,
        globalDescriptorAllocator);
    _playerMaterial.traceBaseColor = playerConstants->colorFactors;
    _playerMaterial.traceParameters = playerConstants->metal_rough_factors;
    
    _bluePortalMaterialBuffer = create_buffer(
        sizeof(GLTFMetallic_Roughness::MaterialConstants),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    auto* bluePortalConstants =
        static_cast<GLTFMetallic_Roughness::MaterialConstants*>(
            _bluePortalMaterialBuffer.info.pMappedData);
    *bluePortalConstants = {};
    // The current material shader has no emissive input yet, so use a bright
    // base color to keep the placement prototype obvious on every wall face.
    bluePortalConstants->colorFactors = glm::vec4(0.2f, 1.5f, 8.0f, 1.0f);
    
    GLTFMetallic_Roughness::MaterialResources bluePortalResources = floorResources;
    bluePortalResources.dataBuffer = _bluePortalMaterialBuffer.buffer;
    _bluePortalMaterial = metalRoughMaterial.write_material(
        _device,
        MaterialPass::MainColor,
        bluePortalResources,
        globalDescriptorAllocator);
    _bluePortalMaterial.traceBaseColor = bluePortalConstants->colorFactors;
    _bluePortalMaterial.traceParameters = bluePortalConstants->metal_rough_factors;
    
    _orangePortalMaterialBuffer = create_buffer(
        sizeof(GLTFMetallic_Roughness::MaterialConstants),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    auto* orangePortalConstants =
        static_cast<GLTFMetallic_Roughness::MaterialConstants*>(
            _orangePortalMaterialBuffer.info.pMappedData);
    *orangePortalConstants = {};
    orangePortalConstants->colorFactors = glm::vec4(8.0f, 1.2f, 0.15f, 1.0f);
    
    GLTFMetallic_Roughness::MaterialResources orangePortalResources = floorResources;
    orangePortalResources.dataBuffer = _orangePortalMaterialBuffer.buffer;
    _orangePortalMaterial = metalRoughMaterial.write_material(
        _device,
        MaterialPass::MainColor,
        orangePortalResources,
        globalDescriptorAllocator);
    _orangePortalMaterial.traceBaseColor = orangePortalConstants->colorFactors;
    _orangePortalMaterial.traceParameters = orangePortalConstants->metal_rough_factors;
    
    init_portal_camera_targets();
}

void VulkanEngine::init_default_scene()
{
    // This model is deliberately not put in loadedScenes: that collection is
    // rendered by the main first-person camera.  We draw this one only into
    // portalViewDrawContext so the player can see their body through a portal.
    auto playerModel = loadGltf(this, "../../assets/tung_tung_tung_sahur.glb");
    if (playerModel) {
        _playerModel = *playerModel;
    } else {
        fmt::print("Failed to load tung_tung_tung_sahur.glb\n");
    }
    
    //auto structureScene = loadGltf(this, "../../assets/structure.glb");
    //if (structureScene) {
      //  loadedScenes["structure"] = *structureScene;
    //} else {
      //  fmt::print("Failed to load structure.glb; trying basicmesh.glb\n");
        //auto basicScene = loadGltf(this, "../../assets/basicmesh.glb");
        //if (basicScene) {
          //  loadedScenes["basicmesh"] = *basicScene;
        //}
    //}
    
    build_sandbox_scene();
    // A missing file simply leaves the starter sandbox intact on the first
    // launch.  After the first File > Save Scene, this restores the level.
    restore_last_editor_scene_name();
    if (const char* testScene = SDL_getenv("MIRABILIS_TEST_SCENE")) _activeSceneFilename = testScene;
    load_editor_scene();
}

void VulkanEngine::init_default_data()
{
    init_default_images_and_samplers();
    init_default_meshes();
    init_default_materials();
    init_default_scene();

    _mainDeletionQueue.push_function([this]() {
        clear_scene_material_resources();
        destroy_buffer(_orangePortalMaterialBuffer);
        destroy_buffer(_bluePortalMaterialBuffer);
        destroy_buffer(_portalMesh.vertexBuffer);
        destroy_buffer(_portalMesh.indexBuffer);
        destroy_buffer(_wallMaterialBuffer);
        destroy_buffer(_rampMesh.vertexBuffer);
        destroy_buffer(_rampMesh.indexBuffer);
        destroy_buffer(_wallMesh.vertexBuffer);
        destroy_buffer(_wallMesh.indexBuffer);
        destroy_buffer(_playerMaterialBuffer);
        destroy_buffer(_floorMaterialBuffer);
        destroy_buffer(_floorMesh.vertexBuffer);
        destroy_buffer(_floorMesh.indexBuffer);
        vkDestroySampler(_device, _skyboxSampler, nullptr);
        vkDestroySampler(_device, _skyboxEnvironmentSampler, nullptr);
        vkDestroySampler(_device, _defaultSamplerNearest, nullptr);
        vkDestroySampler(_device, _defaultSamplerLinear, nullptr);
        destroy_image(_skyboxImage);
        destroy_image(_whiteImage);
        destroy_image(_greyImage);
        destroy_image(_blackImage);
        destroy_image(_errorCheckerboardImage);
    });
}

AllocatedBuffer VulkanEngine::create_buffer(
    size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage)
{
    VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.pNext = nullptr;
    bufferInfo.size = allocSize;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = memoryUsage;
    if (memoryUsage == VMA_MEMORY_USAGE_CPU_ONLY ||
        memoryUsage == VMA_MEMORY_USAGE_CPU_TO_GPU) {
        allocationInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    AllocatedBuffer newBuffer{};
    VK_CHECK(vmaCreateBuffer(
        _allocator,
        &bufferInfo,
        &allocationInfo,
        &newBuffer.buffer,
        &newBuffer.allocation,
        &newBuffer.info));

    return newBuffer;
}

void VulkanEngine::destroy_buffer(const AllocatedBuffer& buffer)
{
    if (buffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(_allocator, buffer.buffer, buffer.allocation);
    }
}

GPUMeshBuffers VulkanEngine::uploadMesh(
    std::span<uint32_t> indices, std::span<Vertex> vertices)
{
    const size_t vertexBufferSize = vertices.size_bytes();
    const size_t indexBufferSize = indices.size_bytes();

    GPUMeshBuffers newSurface{};
    newSurface.vertexBuffer = create_buffer(
        vertexBufferSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    VkBufferDeviceAddressInfo addressInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = newSurface.vertexBuffer.buffer};
    newSurface.vertexBufferAddress = vkGetBufferDeviceAddress(_device, &addressInfo);
    newSurface.traceSource = std::make_shared<TraceMeshSource>();
    newSurface.traceSource->vertices.assign(vertices.begin(), vertices.end());
    newSurface.traceSource->indices.assign(indices.begin(), indices.end());
    _traceMeshSources[newSurface.vertexBufferAddress] = newSurface.traceSource;

    newSurface.indexBuffer = create_buffer(
        indexBufferSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    AllocatedBuffer staging = create_buffer(
        vertexBufferSize + indexBufferSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_CPU_ONLY);

    std::memcpy(staging.info.pMappedData, vertices.data(), vertexBufferSize);
    std::memcpy(
        static_cast<char*>(staging.info.pMappedData) + vertexBufferSize,
        indices.data(),
        indexBufferSize);

    immediate_submit([&](VkCommandBuffer cmd) {
        VkBufferCopy vertexCopy{};
        vertexCopy.srcOffset = 0;
        vertexCopy.dstOffset = 0;
        vertexCopy.size = vertexBufferSize;
        vkCmdCopyBuffer(
            cmd, staging.buffer, newSurface.vertexBuffer.buffer, 1, &vertexCopy);

        VkBufferCopy indexCopy{};
        indexCopy.srcOffset = vertexBufferSize;
        indexCopy.dstOffset = 0;
        indexCopy.size = indexBufferSize;
        vkCmdCopyBuffer(
            cmd, staging.buffer, newSurface.indexBuffer.buffer, 1, &indexCopy);
    });

    destroy_buffer(staging);
    return newSurface;
}

AllocatedImage VulkanEngine::create_image(
    VkExtent3D size,
    VkFormat format,
    VkImageUsageFlags usage,
    bool mipmapped)
{
    AllocatedImage image{};
    image.imageFormat = format;
    image.imageExtent = size;

    VkImageCreateInfo imageInfo = vkinit::image_create_info(format, usage, size);
    if (mipmapped) {
        imageInfo.mipLevels = static_cast<uint32_t>(
            std::floor(std::log2(std::max(size.width, size.height)))) + 1;
    }

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VK_CHECK(vmaCreateImage(
        _allocator,
        &imageInfo,
        &allocationInfo,
        &image.image,
        &image.allocation,
        nullptr));

    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (format == VK_FORMAT_D32_SFLOAT ||
        format == VK_FORMAT_D16_UNORM ||
        format == VK_FORMAT_X8_D24_UNORM_PACK32) {
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    } else if (format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    VkImageViewCreateInfo viewInfo = vkinit::imageview_create_info(format, image.image, aspect);
    viewInfo.subresourceRange.levelCount = imageInfo.mipLevels;
    VK_CHECK(vkCreateImageView(_device, &viewInfo, nullptr, &image.imageView));
    return image;
}

AllocatedImage VulkanEngine::create_image(
    void* data,
    VkExtent3D size,
    VkFormat format,
    VkImageUsageFlags usage,
    bool mipmapped)
{
    const size_t dataSize = static_cast<size_t>(size.width) * size.height * size.depth * 4;
    return create_image(data, dataSize, size, format, usage, mipmapped);
}

AllocatedImage VulkanEngine::create_image(
    void* data,
    size_t dataSize,
    VkExtent3D size,
    VkFormat format,
    VkImageUsageFlags usage,
    bool mipmapped)
{
    AllocatedBuffer uploadBuffer = create_buffer(
        dataSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    std::memcpy(uploadBuffer.info.pMappedData, data, dataSize);

    AllocatedImage image = create_image(
        size,
        format,
        usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        mipmapped);

    immediate_submit([&](VkCommandBuffer cmd) {
        vkutil::transition_image(
            cmd,
            image.image,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy copyRegion{};
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageExtent = size;
        vkCmdCopyBufferToImage(
            cmd,
            uploadBuffer.buffer,
            image.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &copyRegion);

        if (mipmapped) {
            vkutil::generate_mipmaps(
                cmd,
                image.image,
                VkExtent2D{image.imageExtent.width, image.imageExtent.height});
        } else {
            vkutil::transition_image(
                cmd,
                image.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });

    destroy_buffer(uploadBuffer);
    if(format==VK_FORMAT_R8G8B8A8_UNORM||format==VK_FORMAT_R8G8B8A8_SRGB) {
        image.traceSource=std::make_shared<TraceTextureSource>();
        image.traceSource->width=size.width; image.traceSource->height=size.height;
        image.traceSource->rgba.resize(size_t(size.width)*size.height);
        std::memcpy(image.traceSource->rgba.data(),data,image.traceSource->rgba.size()*sizeof(uint32_t));
    }
    return image;
}

void VulkanEngine::destroy_image(const AllocatedImage& image)
{
    if (image.imageView != VK_NULL_HANDLE) {
        vkDestroyImageView(_device, image.imageView, nullptr);
    }
    if (image.image != VK_NULL_HANDLE) {
        vmaDestroyImage(_allocator, image.image, image.allocation);
    }
}
