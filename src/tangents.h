#pragma once

#include <cstdint>
#include <span>

#include <vk_types.h>

// Fills Vertex::tangent for every vertex the index list references, from the
// UV gradient of each triangle it belongs to.  Contributions are accumulated
// per vertex and orthogonalized against the vertex normal; w carries the
// bitangent sign.  Triangles with degenerate UVs contribute nothing, and a
// vertex that ends with no usable tangent gets an arbitrary one perpendicular
// to its normal with w = 1.  Vertices the indices do not reference are left
// untouched, so one primitive can be generated without disturbing another
// that shares the vertex array and already has tangents.
//
// This differs from MikkTSpace only across smoothed UV seams.
void generate_tangents(
    std::span<Vertex> vertices, std::span<const uint32_t> indices);
