#include "tangents.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <glm/geometric.hpp>

namespace {

bool finite(const glm::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z);
}

} // namespace

void generate_tangents(
    std::span<Vertex> vertices, std::span<const uint32_t> indices)
{
    std::vector<glm::vec3> tangents(vertices.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> bitangents(vertices.size(), glm::vec3(0.0f));
    std::vector<bool> referenced(vertices.size(), false);

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t a = indices[i];
        const uint32_t b = indices[i + 1];
        const uint32_t c = indices[i + 2];
        if (std::max({a, b, c}) >= vertices.size()) {
            continue;
        }
        referenced[a] = referenced[b] = referenced[c] = true;

        const Vertex& v0 = vertices[a];
        const Vertex& v1 = vertices[b];
        const Vertex& v2 = vertices[c];
        const glm::vec3 edge1 = v1.position - v0.position;
        const glm::vec3 edge2 = v2.position - v0.position;
        const glm::vec2 uv1(v1.uv_x - v0.uv_x, v1.uv_y - v0.uv_y);
        const glm::vec2 uv2(v2.uv_x - v0.uv_x, v2.uv_y - v0.uv_y);
        const float determinant = uv1.x * uv2.y - uv2.x * uv1.y;
        if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12f) {
            continue;
        }
        // Left unnormalized, so larger triangles in UV-to-world terms weigh
        // more in a vertex's average than slivers do.
        const float inverse = 1.0f / determinant;
        const glm::vec3 tangent = (edge1 * uv2.y - edge2 * uv1.y) * inverse;
        const glm::vec3 bitangent = (edge2 * uv1.x - edge1 * uv2.x) * inverse;
        if (!finite(tangent) || !finite(bitangent)) {
            continue;
        }
        for (const uint32_t index : {a, b, c}) {
            tangents[index] += tangent;
            bitangents[index] += bitangent;
        }
    }

    for (size_t i = 0; i < vertices.size(); ++i) {
        if (!referenced[i]) {
            continue;
        }
        Vertex& vertex = vertices[i];
        glm::vec3 normal = vertex.normal;
        normal = glm::dot(normal, normal) > 1e-12f
            ? glm::normalize(normal)
            : glm::vec3(0.0f, 0.0f, 1.0f);

        // Gram-Schmidt: the part of the accumulated tangent that lies in the
        // surface.
        glm::vec3 tangent = tangents[i] - normal * glm::dot(normal, tangents[i]);
        if (glm::dot(tangent, tangent) > 1e-20f && finite(tangent)) {
            tangent = glm::normalize(tangent);
            const float sign =
                glm::dot(glm::cross(normal, tangent), bitangents[i]) < 0.0f
                ? -1.0f
                : 1.0f;
            vertex.tangent = glm::vec4(tangent, sign);
        } else {
            const glm::vec3 reference = std::abs(normal.y) < 0.999f
                ? glm::vec3(0.0f, 1.0f, 0.0f)
                : glm::vec3(1.0f, 0.0f, 0.0f);
            vertex.tangent = glm::vec4(
                glm::normalize(glm::cross(reference, normal)), 1.0f);
        }
    }
}
