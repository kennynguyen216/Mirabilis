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

    fastgltf::Parser parser{};
    fastgltf::Asset gltf;
    const auto fileType = fastgltf::determineGltfFileType(&data);

    if (fileType == fastgltf::GltfType::glTF) {
        auto load = parser.loadGLTF(&data, filePath.parent_path(), gltfOptions);
        if (!load) {
            fmt::print("Failed to load GLTF: {}\n", fastgltf::to_underlying(load.error()));
            return {};
        }
        gltf = std::move(load.get());
    } else if (fileType == fastgltf::GltfType::GLB) {
        auto load = parser.loadBinaryGLTF(&data, filePath.parent_path(), gltfOptions);
        if (!load) {
            fmt::print("Failed to load GLB: {}\n", fastgltf::to_underlying(load.error()));
            return {};
        }
        gltf = std::move(load.get());
    } else {
        fmt::print("Failed to determine GLTF container type\n");
        return {};
    }

    auto loadImage = [&](fastgltf::Image& source)
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
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_SAMPLED_BIT,
                true);
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

    for (const fastgltf::Sampler& sampler : gltf.samplers) {
        VkSamplerCreateInfo samplerInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        samplerInfo.magFilter = extract_filter(
            sampler.magFilter.value_or(fastgltf::Filter::Nearest));
        samplerInfo.minFilter = extract_filter(
            sampler.minFilter.value_or(fastgltf::Filter::Nearest));
        samplerInfo.mipmapMode = extract_mipmap_mode(
            sampler.minFilter.value_or(fastgltf::Filter::Nearest));

        VkSampler newSampler = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSampler(
            engine->_device, &samplerInfo, nullptr, &newSampler));
        scene->samplers.push_back(newSampler);
    }

    std::vector<AllocatedImage> images;
    images.reserve(gltf.images.size());
    for (size_t i = 0; i < gltf.images.size(); ++i) {
        const std::string name = object_name(gltf.images[i].name, "image_", i);
        auto loadedImage = loadImage(gltf.images[i]);
        if (loadedImage.has_value()) {
            images.push_back(*loadedImage);
            scene->images[name] = *loadedImage;
        } else {
            images.push_back(engine->_errorCheckerboardImage);
            scene->images[name] = images.back();
            fmt::print("Failed to load GLTF image {}\n", name);
        }
    }

    scene->materialDataBuffer = engine->create_buffer(
        sizeof(GLTFMetallic_Roughness::MaterialConstants) * materialCount,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    auto* materialConstants = static_cast<GLTFMetallic_Roughness::MaterialConstants*>(
        scene->materialDataBuffer.info.pMappedData);

    std::vector<std::shared_ptr<GLTFMaterial>> materials;
    materials.reserve(materialCount);

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
        materialConstants[index] = constants;

        GLTFMetallic_Roughness::MaterialResources resources{};
        resources.colorImage = engine->_whiteImage;
        resources.colorSampler = engine->_defaultSamplerLinear;
        resources.metalRoughImage = engine->_whiteImage;
        resources.metalRoughSampler = engine->_defaultSamplerLinear;
        resources.dataBuffer = scene->materialDataBuffer.buffer;
        resources.dataBufferOffset = static_cast<uint32_t>(
            index * sizeof(GLTFMetallic_Roughness::MaterialConstants));

        if (source != nullptr && source->pbrData.baseColorTexture.has_value()) {
            const auto& textureInfo = source->pbrData.baseColorTexture.value();
            const auto& texture = gltf.textures[textureInfo.textureIndex];
            if (texture.imageIndex.has_value() &&
                texture.imageIndex.value() < images.size()) {
                resources.colorImage = images[texture.imageIndex.value()];
            }
            if (texture.samplerIndex.has_value() &&
                texture.samplerIndex.value() < scene->samplers.size()) {
                resources.colorSampler = scene->samplers[texture.samplerIndex.value()];
            }
        }

        material->data = engine->metalRoughMaterial.write_material(
            engine->_device,
            pass,
            resources,
            scene->descriptorPool);
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
            const MaterialPass pass = source.alphaMode == fastgltf::AlphaMode::Blend
                ? MaterialPass::Transparent
                : MaterialPass::MainColor;
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

    std::vector<std::shared_ptr<MeshAsset>> meshes;
    meshes.reserve(gltf.meshes.size());
    std::vector<uint32_t> indices;
    std::vector<Vertex> vertices;

    for (size_t meshIndex = 0; meshIndex < gltf.meshes.size(); ++meshIndex) {
        const fastgltf::Mesh& mesh = gltf.meshes[meshIndex];
        auto newMesh = std::make_shared<MeshAsset>();
        newMesh->name = object_name(mesh.name, "mesh_", meshIndex);
        meshes.push_back(newMesh);
        scene->meshes[newMesh->name] = newMesh;

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
            newMesh->meshBuffers = engine->uploadMesh(indices, vertices);
        }
    }

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
        scene->nodes[object_name(source.name, "node_", nodeIndex)] = newNode;
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
            scene->topNodes.push_back(node);
            node->refreshTransform(glm::mat4(1.0f));
        }
    }

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
