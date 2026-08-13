#include "vk_loader.h"

#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/parser.hpp>
#include <fastgltf/tools.hpp>

#include <fmt/core.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "vk_engine.h"

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
