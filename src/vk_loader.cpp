#include "vk_loader.h"

#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/parser.hpp>
#include <fastgltf/tools.hpp>

#include <algorithm>
#include <cstring>
#include <fmt/core.h>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <stb_image.h>
#include <unordered_set>

#include "vk_engine.h"
#include "vk_engine_render_helpers.h"

namespace {

template <typename StringLike>
std::string object_name(const StringLike& sourceName, std::string_view prefix, size_t index)
{
    std::string name(sourceName.data(), sourceName.size());
    if (name.empty()) {
        name = std::string(prefix) + std::to_string(index);
    }
    return name;
}

VkFilter extract_filter(fastgltf::Filter filter)
{
    switch (filter) {
    case fastgltf::Filter::Nearest:
    case fastgltf::Filter::NearestMipMapNearest:
    case fastgltf::Filter::NearestMipMapLinear:
        return VK_FILTER_NEAREST;
    case fastgltf::Filter::Linear:
    case fastgltf::Filter::LinearMipMapNearest:
    case fastgltf::Filter::LinearMipMapLinear:
    default:
        return VK_FILTER_LINEAR;
    }
}

VkSamplerMipmapMode extract_mipmap_mode(fastgltf::Filter filter)
{
    switch (filter) {
    case fastgltf::Filter::NearestMipMapNearest:
    case fastgltf::Filter::LinearMipMapNearest:
        return VK_SAMPLER_MIPMAP_MODE_NEAREST;
    case fastgltf::Filter::NearestMipMapLinear:
    case fastgltf::Filter::LinearMipMapLinear:
    default:
        return VK_SAMPLER_MIPMAP_MODE_LINEAR;
    }
}

std::optional<fastgltf::Asset> parse_gltf_asset(
    fastgltf::GltfDataBuffer& data,
    const std::filesystem::path& filePath,
    fastgltf::Options options)
{
    fastgltf::Parser parser{};
    const auto fileType = fastgltf::determineGltfFileType(&data);

    if (fileType == fastgltf::GltfType::glTF) {
        auto load = parser.loadGLTF(&data, filePath.parent_path(), options);
        if (!load) {
            fmt::print(
                "Failed to load GLTF: {}\n",
                fastgltf::to_underlying(load.error()));
            return {};
        }
        return std::move(load.get());
    }
    if (fileType == fastgltf::GltfType::GLB) {
        auto load = parser.loadBinaryGLTF(&data, filePath.parent_path(), options);
        if (!load) {
            fmt::print(
                "Failed to load GLB: {}\n",
                fastgltf::to_underlying(load.error()));
            return {};
        }
        return std::move(load.get());
    }

    fmt::print("Failed to determine GLTF container type\n");
    return {};
}

void create_gltf_samplers(
    VulkanEngine* engine,
    LoadedGLTF& scene,
    const fastgltf::Asset& gltf)
{
    for (const fastgltf::Sampler& sampler : gltf.samplers) {
        VkSamplerCreateInfo samplerInfo = sampler_info(
            extract_filter(
                sampler.magFilter.value_or(fastgltf::Filter::Nearest)),
            VK_SAMPLER_ADDRESS_MODE_REPEAT,
            extract_mipmap_mode(
                sampler.minFilter.value_or(fastgltf::Filter::Nearest)),
            VK_LOD_CLAMP_NONE);
        samplerInfo.minLod = 0.0f;
        samplerInfo.minFilter = extract_filter(
            sampler.minFilter.value_or(fastgltf::Filter::Nearest));

        // Anisotropy only means anything for a minifying linear filter, which
        // is exactly the oblique floor and arch case it exists for.  Leaving it
        // off for NEAREST keeps an intentionally crisp texture crisp.
        const float anisotropy = engine->material_anisotropy();
        if (anisotropy > 1.0f && samplerInfo.minFilter == VK_FILTER_LINEAR) {
            samplerInfo.anisotropyEnable = VK_TRUE;
            samplerInfo.maxAnisotropy = anisotropy;
        }

        VkSampler newSampler = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSampler(
            engine->_device, &samplerInfo, nullptr, &newSampler));
        scene.samplers.push_back(newSampler);
    }
}

void print_gltf_texture_summary(
    const std::unordered_map<uint64_t, AllocatedImage>& imageCache,
    VulkanEngine* engine,
    size_t boundBaseColorCount,
    size_t boundMetalRoughCount,
    size_t materialCount,
    const std::filesystem::path& filePath)
{
    size_t srgbCount = 0;
    size_t linearCount = 0;
    size_t estimatedBytes = 0;
    for (const auto& [key, image] : imageCache) {
        if (image.image == engine->_errorCheckerboardImage.image) {
            continue;
        }
        (key & 1) ? ++srgbCount : ++linearCount;
        const size_t baseLevel = size_t(image.imageExtent.width) *
            image.imageExtent.height * 4;
        // A full chain converges on 4/3 of the base level.
        estimatedBytes += baseLevel + baseLevel / 3;
    }
    fmt::print(
        "GLTF textures: {} sRGB + {} linear, ~{:.1f} MB with mips; "
        "{}/{} materials bound base colour, {}/{} metallic-roughness ({})\n",
        srgbCount,
        linearCount,
        double(estimatedBytes) / (1024.0 * 1024.0),
        boundBaseColorCount,
        materialCount,
        boundMetalRoughCount,
        materialCount,
        filePath.filename().string());
}

void build_gltf_nodes(
    LoadedGLTF& scene,
    const fastgltf::Asset& gltf,
    const std::vector<std::shared_ptr<MeshAsset>>& meshes)
{
    std::vector<std::shared_ptr<Node>> nodes;
    nodes.reserve(gltf.nodes.size());
    for (size_t nodeIndex = 0; nodeIndex < gltf.nodes.size(); ++nodeIndex) {
        const fastgltf::Node& source = gltf.nodes[nodeIndex];
        std::shared_ptr<Node> newNode;
        if (source.meshIndex.has_value() &&
            source.meshIndex.value() < meshes.size()) {
            auto meshNode = std::make_shared<MeshNode>();
            meshNode->mesh = meshes[source.meshIndex.value()];
            newNode = meshNode;
        } else {
            newNode = std::make_shared<Node>();
        }

        std::visit(
            fastgltf::visitor{
                [&](const fastgltf::Node::TransformMatrix& matrix) {
                    std::memcpy(
                        &newNode->localTransform,
                        matrix.data(),
                        sizeof(newNode->localTransform));
                },
                [&](const fastgltf::Node::TRS& transform) {
                    const glm::vec3 translation(
                        transform.translation[0],
                        transform.translation[1],
                        transform.translation[2]);
                    const glm::quat rotation(
                        transform.rotation[3],
                        transform.rotation[0],
                        transform.rotation[1],
                        transform.rotation[2]);
                    const glm::vec3 scale(
                        transform.scale[0],
                        transform.scale[1],
                        transform.scale[2]);
                    newNode->localTransform =
                        glm::translate(glm::mat4(1.0f), translation) *
                        glm::toMat4(rotation) *
                        glm::scale(glm::mat4(1.0f), scale);
                }},
            source.transform);

        nodes.push_back(newNode);
        scene.nodes[object_name(source.name, "node_", nodeIndex)] = newNode;
    }

    for (size_t nodeIndex = 0; nodeIndex < gltf.nodes.size(); ++nodeIndex) {
        for (const size_t childIndex : gltf.nodes[nodeIndex].children) {
            if (childIndex >= nodes.size()) {
                continue;
            }
            nodes[nodeIndex]->children.push_back(nodes[childIndex]);
            nodes[childIndex]->parent = nodes[nodeIndex];
        }
    }

    for (const auto& node : nodes) {
        if (node->parent.expired()) {
            scene.topNodes.push_back(node);
            node->refreshTransform(glm::mat4(1.0f));
        }
    }
}

template <typename UploadMesh>
std::vector<std::shared_ptr<MeshAsset>> build_gltf_meshes(
    LoadedGLTF& scene,
    const fastgltf::Asset& gltf,
    const std::vector<std::shared_ptr<GLTFMaterial>>& materials,
    UploadMesh&& uploadMesh)
{
    std::vector<std::shared_ptr<MeshAsset>> meshes;
    meshes.reserve(gltf.meshes.size());
    std::vector<uint32_t> indices;
    std::vector<Vertex> vertices;

    for (size_t meshIndex = 0; meshIndex < gltf.meshes.size(); ++meshIndex) {
        const fastgltf::Mesh& mesh = gltf.meshes[meshIndex];
        auto newMesh = std::make_shared<MeshAsset>();
        newMesh->name = object_name(mesh.name, "mesh_", meshIndex);
        meshes.push_back(newMesh);
        scene.meshes[newMesh->name] = newMesh;

        indices.clear();
        vertices.clear();

        for (const auto& primitive : mesh.primitives) {
            const auto positionAttribute = primitive.findAttribute("POSITION");
            if (positionAttribute == primitive.attributes.end() ||
                !primitive.indicesAccessor.has_value()) {
                continue;
            }

            GeoSurface surface{};
            surface.startIndex = static_cast<uint32_t>(indices.size());
            const auto& indexAccessor =
                gltf.accessors[primitive.indicesAccessor.value()];
            surface.count = static_cast<uint32_t>(indexAccessor.count);

            const size_t firstVertex = vertices.size();
            const auto& positionAccessor = gltf.accessors[positionAttribute->second];
            vertices.resize(vertices.size() + positionAccessor.count);
            for (size_t i = firstVertex; i < vertices.size(); ++i) {
                vertices[i].normal = glm::vec3(0.0f, 0.0f, 1.0f);
                vertices[i].color = glm::vec4(1.0f);
            }

            fastgltf::iterateAccessorWithIndex<glm::vec3>(
                gltf,
                positionAccessor,
                [&](glm::vec3 position, size_t index) {
                    vertices[firstVertex + index].position = position;
                });
            fastgltf::iterateAccessor<uint32_t>(
                gltf,
                indexAccessor,
                [&](uint32_t index) {
                    indices.push_back(index + static_cast<uint32_t>(firstVertex));
                });

            if (const auto normals = primitive.findAttribute("NORMAL");
                normals != primitive.attributes.end()) {
                fastgltf::iterateAccessorWithIndex<glm::vec3>(
                    gltf,
                    gltf.accessors[normals->second],
                    [&](glm::vec3 normal, size_t index) {
                        vertices[firstVertex + index].normal = normal;
                    });
            }
            if (const auto uv = primitive.findAttribute("TEXCOORD_0");
                uv != primitive.attributes.end()) {
                fastgltf::iterateAccessorWithIndex<glm::vec2>(
                    gltf,
                    gltf.accessors[uv->second],
                    [&](glm::vec2 texcoord, size_t index) {
                        vertices[firstVertex + index].uv_x = texcoord.x;
                        vertices[firstVertex + index].uv_y = texcoord.y;
                    });
            }
            if (const auto colors = primitive.findAttribute("COLOR_0");
                colors != primitive.attributes.end()) {
                fastgltf::iterateAccessorWithIndex<glm::vec4>(
                    gltf,
                    gltf.accessors[colors->second],
                    [&](glm::vec4 color, size_t index) {
                        vertices[firstVertex + index].color = color;
                    });
            }

            if (positionAccessor.count > 0) {
                glm::vec3 minPosition = vertices[firstVertex].position;
                glm::vec3 maxPosition = vertices[firstVertex].position;
                for (size_t i = firstVertex; i < vertices.size(); ++i) {
                    minPosition = glm::min(minPosition, vertices[i].position);
                    maxPosition = glm::max(maxPosition, vertices[i].position);
                }
                surface.bounds.origin = (maxPosition + minPosition) * 0.5f;
                surface.bounds.extents = (maxPosition - minPosition) * 0.5f;
                surface.bounds.sphereRadius = glm::length(surface.bounds.extents);
            }

            if (primitive.materialIndex.has_value() &&
                primitive.materialIndex.value() < materials.size()) {
                surface.material = materials[primitive.materialIndex.value()];
            } else {
                surface.material = materials.front();
            }
            newMesh->surfaces.push_back(surface);
        }

        if (!vertices.empty() && !indices.empty()) {
            newMesh->meshBuffers = uploadMesh(indices, vertices);
        }
    }

    return meshes;
}

} // namespace

LoadedGLTF::~LoadedGLTF()
{
    clearAll();
}

void LoadedGLTF::clearAll()
{
    if (creator == nullptr) {
        return;
    }

    for (auto& [name, mesh] : meshes) {
        if (mesh == nullptr) {
            continue;
        }
        creator->destroy_buffer(mesh->meshBuffers.indexBuffer);
        creator->destroy_buffer(mesh->meshBuffers.vertexBuffer);
    }

    if (materialDataBuffer.buffer != VK_NULL_HANDLE) {
        creator->destroy_buffer(materialDataBuffer);
    }

    descriptorPool.destroy_pools(creator->_device);

    std::unordered_set<VkImage> destroyedImages;
    for (auto& [name, image] : images) {
        if (image.image == VK_NULL_HANDLE ||
            image.image == creator->_errorCheckerboardImage.image ||
            image.image == creator->_whiteImage.image ||
            image.image == creator->_blackImage.image ||
            image.image == creator->_greyImage.image) {
            continue;
        }
        if (destroyedImages.insert(image.image).second) {
            creator->destroy_image(image);
        }
    }

    for (VkSampler sampler : samplers) {
        if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(creator->_device, sampler, nullptr);
        }
    }

    creator = nullptr;
}

void LoadedGLTF::Draw(const glm::mat4& topMatrix, DrawContext& ctx)
{
    for (const auto& node : topNodes) {
        node->Draw(topMatrix, ctx);
    }
}

std::optional<std::shared_ptr<LoadedGLTF>> loadGltf(
    VulkanEngine* engine,
    std::filesystem::path filePath)
{
    fmt::print("Loading GLTF scene: {}\n", filePath.string());

    auto scene = std::make_shared<LoadedGLTF>();
    scene->creator = engine;

    fastgltf::GltfDataBuffer data;
    if (!data.loadFromFile(filePath)) {
        fmt::print("Failed to read GLTF file\n");
        return {};
    }

    constexpr auto gltfOptions =
        fastgltf::Options::DontRequireValidAssetMember |
        fastgltf::Options::AllowDouble |
        fastgltf::Options::LoadGLBBuffers |
        fastgltf::Options::LoadExternalBuffers;

    std::optional<fastgltf::Asset> parsed =
        parse_gltf_asset(data, filePath, gltfOptions);
    if (!parsed.has_value()) {
        return {};
    }
    fastgltf::Asset gltf = std::move(*parsed);

    // The format is the caller's, not the file's: the same PNG byte for byte
    // is sRGB-encoded when it arrives as base color and linear when it arrives
    // as a normal or metallic/roughness map.  stb hands back the stored bytes
    // either way, and only the Vulkan format decides whether the hardware
    // decodes them on sample.
    auto loadImage = [&](fastgltf::Image& source, VkFormat format)
        -> std::optional<AllocatedImage> {
        int width = 0;
        int height = 0;
        int channels = 0;
        AllocatedImage loadedImage{};

        auto uploadPixels = [&](const unsigned char* pixels) {
            if (pixels == nullptr || width <= 0 || height <= 0) {
                return;
            }
            loadedImage = engine->create_image(
                const_cast<unsigned char*>(pixels),
                VkExtent3D{
                    static_cast<uint32_t>(width),
                    static_cast<uint32_t>(height),
                    1},
                format,
                VK_IMAGE_USAGE_SAMPLED_BIT,
                true);
            // Base colour is the only sRGB role, and the only map the path
            // tracer samples, so it is the only one worth a CPU copy.
            if (format == VK_FORMAT_R8G8B8A8_SRGB) {
                loadedImage.traceSource = VulkanEngine::make_trace_texture(
                    pixels, loadedImage.imageExtent);
            }
        };

        auto reportDecodeFailure = [&]() {
            fmt::print(
                "stb_image failed for {}: {}\n",
                source.name,
                stbi_failure_reason() != nullptr ? stbi_failure_reason() : "unknown");
        };

        std::visit(
            fastgltf::visitor{
                [](auto&) {},
                [&](fastgltf::sources::URI& uri) {
                    if (uri.fileByteOffset != 0 || !uri.uri.isLocalPath()) {
                        return;
                    }
                    const std::string relativePath(
                        uri.uri.path().begin(), uri.uri.path().end());
                    const std::filesystem::path imagePath =
                        filePath.parent_path() / relativePath;
                    stbi_uc* pixels = stbi_load(
                        imagePath.string().c_str(),
                        &width,
                        &height,
                        &channels,
                        4);
                    if (pixels != nullptr) {
                        uploadPixels(pixels);
                        stbi_image_free(pixels);
                    } else {
                        reportDecodeFailure();
                    }
                },
                [&](fastgltf::sources::Vector& vector) {
                    const stbi_uc* encoded = reinterpret_cast<const stbi_uc*>(
                        vector.bytes.data());
                    stbi_uc* pixels = stbi_load_from_memory(
                        encoded,
                        static_cast<int>(vector.bytes.size()),
                        &width,
                        &height,
                        &channels,
                        4);
                    if (pixels != nullptr) {
                        uploadPixels(pixels);
                        stbi_image_free(pixels);
                    } else {
                        reportDecodeFailure();
                    }
                },
                [&](fastgltf::sources::ByteView& view) {
                    const stbi_uc* encoded = reinterpret_cast<const stbi_uc*>(
                        view.bytes.data());
                    stbi_uc* pixels = stbi_load_from_memory(
                        encoded,
                        static_cast<int>(view.bytes.size()),
                        &width,
                        &height,
                        &channels,
                        4);
                    if (pixels != nullptr) {
                        uploadPixels(pixels);
                        stbi_image_free(pixels);
                    } else {
                        reportDecodeFailure();
                    }
                },
                [&](fastgltf::sources::BufferView& view) {
                    const auto& bufferView = gltf.bufferViews[view.bufferViewIndex];
                    const auto& buffer = gltf.buffers[bufferView.bufferIndex];
                    if (auto* vector = std::get_if<fastgltf::sources::Vector>(&buffer.data)) {
                        const stbi_uc* encoded = reinterpret_cast<const stbi_uc*>(
                            vector->bytes.data() + bufferView.byteOffset);
                        stbi_uc* pixels = stbi_load_from_memory(
                            encoded,
                            static_cast<int>(bufferView.byteLength),
                            &width,
                            &height,
                            &channels,
                            4);
                        if (pixels != nullptr) {
                            uploadPixels(pixels);
                            stbi_image_free(pixels);
                        } else {
                            reportDecodeFailure();
                        }
                    } else if (auto* bytes = std::get_if<fastgltf::sources::ByteView>(&buffer.data)) {
                        const stbi_uc* encoded = reinterpret_cast<const stbi_uc*>(
                            bytes->bytes.data());
                        stbi_uc* pixels = stbi_load_from_memory(
                            encoded,
                            static_cast<int>(bytes->bytes.size()),
                            &width,
                            &height,
                            &channels,
                            4);
                        if (pixels != nullptr) {
                            uploadPixels(pixels);
                            stbi_image_free(pixels);
                        } else {
                            reportDecodeFailure();
                        }
                    }
                }},
            source.data);

        if (loadedImage.image == VK_NULL_HANDLE) {
            return {};
        }
        return loadedImage;
    };

    const uint32_t materialCount = static_cast<uint32_t>(
        std::max<size_t>(1, gltf.materials.size()));
    std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> descriptorRatios = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3.0f},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3.0f},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1.0f}};
    scene->descriptorPool.init(engine->_device, materialCount, descriptorRatios);

    create_gltf_samplers(engine, *scene, gltf);

    // Images are uploaded on demand rather than up front, because the correct
    // format is a property of the role the material assigns the image, not of
    // the image itself.  A glTF may also carry images no material references;
    // those now cost nothing.  The key is (image index, colour space): one
    // source image referenced as both base colour and a normal map legitimately
    // needs two GPU images, and silently sharing one would decode the normals.
    std::unordered_map<uint64_t, AllocatedImage> imageCache;
    auto acquireImage = [&](size_t imageIndex, bool srgb) -> AllocatedImage {
        if (imageIndex >= gltf.images.size()) {
            return engine->_errorCheckerboardImage;
        }
        const uint64_t key = uint64_t(imageIndex) * 2 + (srgb ? 1 : 0);
        if (auto found = imageCache.find(key); found != imageCache.end()) {
            return found->second;
        }

        const VkFormat format = srgb
            ? VK_FORMAT_R8G8B8A8_SRGB
            : VK_FORMAT_R8G8B8A8_UNORM;
        const std::string name =
            object_name(gltf.images[imageIndex].name, "image_", imageIndex) +
            (srgb ? ":srgb" : ":linear");

        AllocatedImage resolved{};
        auto loadedImage = loadImage(gltf.images[imageIndex], format);
        if (loadedImage.has_value()) {
            resolved = *loadedImage;
        } else {
            // A missing texture must not take the scene down with it: the
            // checkerboard is obvious on screen and is owned by the engine, so
            // clearAll already knows not to destroy it.
            resolved = engine->_errorCheckerboardImage;
            fmt::print("Failed to load GLTF image {}\n", name);
        }

        imageCache[key] = resolved;
        scene->images[name] = resolved;
        return resolved;
    };

    // Resolves one glTF texture reference into the image and sampler a
    // descriptor needs, leaving both untouched when the reference is absent or
    // dangling so the caller's default survives.
    auto resolveTexture = [&](const fastgltf::TextureInfo& textureInfo,
                              bool srgb,
                              AllocatedImage& outImage,
                              VkSampler& outSampler) {
        if (textureInfo.textureIndex >= gltf.textures.size()) {
            return;
        }
        const auto& texture = gltf.textures[textureInfo.textureIndex];
        if (texture.imageIndex.has_value()) {
            outImage = acquireImage(texture.imageIndex.value(), srgb);
        }
        if (texture.samplerIndex.has_value() &&
            texture.samplerIndex.value() < scene->samplers.size()) {
            outSampler = scene->samplers[texture.samplerIndex.value()];
        }
    };

    scene->materialDataBuffer = engine->create_buffer(
        sizeof(GLTFMetallic_Roughness::MaterialConstants) * materialCount,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    auto* materialConstants = static_cast<GLTFMetallic_Roughness::MaterialConstants*>(
        scene->materialDataBuffer.info.pMappedData);

    std::vector<std::shared_ptr<GLTFMaterial>> materials;
    materials.reserve(materialCount);

    // Counted per material rather than per image so the log distinguishes "the
    // asset has no metallic/roughness texture" from "it has one and we failed
    // to bind it", which are indistinguishable from the rendered image until
    // the BRDF actually consumes the map.
    size_t boundBaseColorCount = 0;
    size_t boundMetalRoughCount = 0;

    auto createMaterial = [&](size_t index,
                              glm::vec4 color,
                              float metallic,
                              float roughness,
                              MaterialPass pass,
                              const fastgltf::Material* source) {
        auto material = std::make_shared<GLTFMaterial>();
        materials.push_back(material);

        GLTFMetallic_Roughness::MaterialConstants constants{};
        constants.colorFactors = color;
        constants.metal_rough_factors = glm::vec4(metallic, roughness, 0.0f, 0.0f);
        // extra[0] is the UV transform the scene materials write; extra[1].x
        // is the alpha cutoff.  It stays zero for every pass but Mask, which
        // makes the test in the mask shaders a no-op if one is ever bound to
        // an opaque pipeline by mistake.
        if (pass == MaterialPass::Mask && source != nullptr) {
            constants.extra[1].x = source->alphaCutoff;
        }
        materialConstants[index] = constants;

        GLTFMetallic_Roughness::MaterialResources resources{};
        resources.colorImage = engine->_whiteImage;
        resources.colorSampler = engine->_defaultSamplerLinear;
        resources.metalRoughImage = engine->_whiteImage;
        resources.metalRoughSampler = engine->_defaultSamplerLinear;
        resources.dataBuffer = scene->materialDataBuffer.buffer;
        resources.dataBufferOffset = static_cast<uint32_t>(
            index * sizeof(GLTFMetallic_Roughness::MaterialConstants));

        if (source != nullptr) {
            // Base colour is the one sRGB-encoded material texture here.  Every
            // other role stores measurements, not colours, and must stay linear.
            if (source->pbrData.baseColorTexture.has_value()) {
                resolveTexture(
                    source->pbrData.baseColorTexture.value(),
                    true,
                    resources.colorImage,
                    resources.colorSampler);
                ++boundBaseColorCount;
            }
            if (source->pbrData.metallicRoughnessTexture.has_value()) {
                resolveTexture(
                    source->pbrData.metallicRoughnessTexture.value(),
                    false,
                    resources.metalRoughImage,
                    resources.metalRoughSampler);
                ++boundMetalRoughCount;
            }
        }

        material->data = engine->metalRoughMaterial.write_material(
            engine->_device,
            pass,
            resources,
            scene->descriptorPool);
        material->data.traceBaseColor = constants.colorFactors;
        if(source) material->data.traceEmission=glm::vec4(
            source->emissiveFactor[0],source->emissiveFactor[1],source->emissiveFactor[2],0)*source->emissiveStrength.value_or(1.f);
        material->data.traceParameters = constants.metal_rough_factors;
        material->data.traceParameters.z=source&&source->transmission?source->transmission->transmissionFactor:0.f;
        material->data.traceParameters.w=source?source->ior.value_or(1.5f):1.5f;
        return material;
    };

    if (gltf.materials.empty()) {
        auto material = createMaterial(0, glm::vec4(1.0f), 1.0f, 0.5f,
                                       MaterialPass::MainColor, nullptr);
        scene->materials["default"] = material;
    } else {
        for (size_t i = 0; i < gltf.materials.size(); ++i) {
            const fastgltf::Material& source = gltf.materials[i];
            glm::vec4 color(
                source.pbrData.baseColorFactor[0],
                source.pbrData.baseColorFactor[1],
                source.pbrData.baseColorFactor[2],
                source.pbrData.baseColorFactor[3]);
            // MASK is its own pass rather than an opaque material: it still
            // writes depth and still belongs in the prepass, but every stage
            // that draws it has to run the cutoff test or it appears as the
            // solid rectangle its texture is mapped onto.
            MaterialPass pass = MaterialPass::MainColor;
            if (source.alphaMode == fastgltf::AlphaMode::Blend) {
                pass = MaterialPass::Transparent;
            } else if (source.alphaMode == fastgltf::AlphaMode::Mask) {
                pass = MaterialPass::Mask;
            }
            auto material = createMaterial(
                i,
                color,
                source.pbrData.metallicFactor,
                source.pbrData.roughnessFactor,
                pass,
                &source);
            scene->materials[object_name(source.name, "material_", i)] = material;
        }
    }

    // Every material has now declared the roles it uses, so the cache holds
    // exactly the images this scene will occupy.  Reporting it here is what
    // makes a texture budget arguable later: file size on disk says nothing,
    // and the mip chain adds a third again on top of the base level.
    print_gltf_texture_summary(
        imageCache,
        engine,
        boundBaseColorCount,
        boundMetalRoughCount,
        materialCount,
        filePath);

    auto uploadMesh = [&](std::vector<uint32_t>& indices,
                          std::vector<Vertex>& vertices) {
        return engine->uploadMesh(indices, vertices);
    };
    std::vector<std::shared_ptr<MeshAsset>> meshes =
        build_gltf_meshes(*scene, gltf, materials, uploadMesh);

    build_gltf_nodes(*scene, gltf, meshes);

    return scene;
}

std::optional<std::vector<std::shared_ptr<MeshAsset>>> loadGltfMeshes(
    VulkanEngine* engine,
    std::filesystem::path filePath)
{
    fmt::print("Loading GLTF: {}\n", filePath.string());

    fastgltf::GltfDataBuffer data;
    if (!data.loadFromFile(filePath)) {
        fmt::print("Failed to read GLTF file\n");
        return {};
    }

    constexpr auto gltfOptions = fastgltf::Options::LoadGLBBuffers |
        fastgltf::Options::LoadExternalBuffers;

    fastgltf::Parser parser{};
    auto load = parser.loadBinaryGLTF(
        &data, filePath.parent_path(), gltfOptions);
    if (!load) {
        fmt::print(
            "Failed to load GLTF: {}\n",
            fastgltf::to_underlying(load.error()));
        return {};
    }

    fastgltf::Asset gltf = std::move(load.get());
    std::vector<std::shared_ptr<MeshAsset>> meshes;
    std::vector<uint32_t> indices;
    std::vector<Vertex> vertices;

    for (fastgltf::Mesh& mesh : gltf.meshes) {
        auto newMesh = std::make_shared<MeshAsset>();
        newMesh->name = mesh.name;
        indices.clear();
        vertices.clear();

        for (auto&& primitive : mesh.primitives) {
            auto positionAttribute = primitive.findAttribute("POSITION");
            if (positionAttribute == primitive.attributes.end() ||
                !primitive.indicesAccessor.has_value()) {
                continue;
            }

            GeoSurface surface{};
            surface.startIndex = static_cast<uint32_t>(indices.size());
            const auto& indexAccessor =
                gltf.accessors[primitive.indicesAccessor.value()];
            surface.count = static_cast<uint32_t>(indexAccessor.count);

            const size_t firstVertex = vertices.size();
            const auto& positionAccessor =
                gltf.accessors[positionAttribute->second];
            vertices.resize(vertices.size() + positionAccessor.count);

            fastgltf::iterateAccessorWithIndex<glm::vec3>(
                gltf, positionAccessor,
                [&](glm::vec3 position, size_t index) {
                    vertices[firstVertex + index].position = position;
                });

            fastgltf::iterateAccessorWithIndex<uint32_t>(
                gltf, indexAccessor,
                [&](uint32_t index, size_t) {
                    indices.push_back(index + static_cast<uint32_t>(firstVertex));
                });

            if (auto normals = primitive.findAttribute("NORMAL");
                normals != primitive.attributes.end()) {
                const auto& accessor = gltf.accessors[normals->second];
                fastgltf::iterateAccessorWithIndex<glm::vec3>(
                    gltf, accessor,
                    [&](glm::vec3 normal, size_t index) {
                        vertices[firstVertex + index].normal = normal;
                    });
            }

            if (auto uv = primitive.findAttribute("TEXCOORD_0");
                uv != primitive.attributes.end()) {
                const auto& accessor = gltf.accessors[uv->second];
                fastgltf::iterateAccessorWithIndex<glm::vec2>(
                    gltf, accessor,
                    [&](glm::vec2 texcoord, size_t index) {
                        vertices[firstVertex + index].uv_x = texcoord.x;
                        vertices[firstVertex + index].uv_y = texcoord.y;
                    });
            }

            if (auto colors = primitive.findAttribute("COLOR_0");
                colors != primitive.attributes.end()) {
                const auto& accessor = gltf.accessors[colors->second];
                fastgltf::iterateAccessorWithIndex<glm::vec4>(
                    gltf, accessor,
                    [&](glm::vec4 color, size_t index) {
                        vertices[firstVertex + index].color = color;
                    });
            }

            newMesh->surfaces.push_back(surface);
        }

        if (!vertices.empty() && !indices.empty()) {
            for (Vertex& vertex : vertices) {
                vertex.color = glm::vec4(vertex.normal, 1.0f);
            }
            newMesh->meshBuffers = engine->uploadMesh(indices, vertices);
            meshes.push_back(std::move(newMesh));
        }
    }

    return meshes;
}
