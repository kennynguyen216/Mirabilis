#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <vector>

struct MeshCluster {
    glm::vec4 minimum{}, maximum{};
    uint32_t firstIndex{}, indexCount{}, vertexCount{};
};

// Preserve authored triangle order and index buffers. Bound each cluster to
// 32 unique vertices and 60 triangles; alpha-blended meshes never use this path.
template <class Vertex>
std::vector<MeshCluster> build_mesh_clusters(std::span<const Vertex> vertices,
                                             std::span<const uint32_t> indices, uint32_t firstIndex,
                                             uint32_t indexCount) {
    std::vector<MeshCluster> result;
    if (firstIndex > indices.size() || indexCount > indices.size() - firstIndex || indexCount % 3)
        return result;
    MeshCluster cluster;
    std::array<uint32_t, 32> unique{};
    uint32_t used = 0;
    const auto flush = [&] {
        if (cluster.indexCount) {
            cluster.vertexCount = used;
            result.push_back(cluster);
        }
        cluster = {};
        used = 0;
    };
    for (uint32_t triangle = firstIndex; triangle < firstIndex + indexCount; triangle += 3) {
        const uint32_t tri[] = {indices[triangle], indices[triangle + 1], indices[triangle + 2]};
        uint32_t extra = 0;
        for (int i = 0; i < 3; ++i) {
            if (tri[i] >= vertices.size())
                return {};
            bool seen =
                std::find(unique.begin(), unique.begin() + used, tri[i]) != unique.begin() + used;
            for (int j = 0; j < i; ++j)
                seen |= tri[j] == tri[i];
            if (!seen)
                ++extra;
        }
        if (used + extra > 32 || cluster.indexCount == 180)
            flush();
        if (!cluster.indexCount) {
            cluster.firstIndex = triangle;
            cluster.minimum = cluster.maximum = glm::vec4(vertices[tri[0]].position, 0);
        }
        for (auto vertex : tri) {
            if (std::find(unique.begin(), unique.begin() + used, vertex) == unique.begin() + used)
                unique[used++] = vertex;
            const glm::vec4 p(vertices[vertex].position, 0);
            cluster.minimum = glm::min(cluster.minimum, p);
            cluster.maximum = glm::max(cluster.maximum, p);
        }
        cluster.indexCount += 3;
    }
    flush();
    return result;
}
