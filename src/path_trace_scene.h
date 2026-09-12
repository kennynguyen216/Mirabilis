#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>
#include <cstddef>

// Explicit vec4 lanes keep the std430 ABI independent of vec3 padding.
struct TraceTriangle {
    glm::vec4 p0{}, p1{}, p2{};
    glm::vec4 n0{}, n1{}, n2{}; // n0.w/n1.w hold UV0
    glm::vec4 uv12{};
    glm::uvec4 meta{}; // material index, reserved
    glm::vec4 c0{1},c1{1},c2{1};
};
struct TraceMaterial {
    glm::vec4 baseColor{1};
    glm::vec4 emission{};
    glm::vec4 parameters{0,0.8f,0,1.5f};
    glm::vec4 uvScale{1,1,0,0};
    glm::uvec4 texture{}; // packed texel offset, width, height, reserved
};
static_assert(sizeof(TraceTriangle)==176 && offsetof(TraceTriangle,meta)==112);
static_assert(sizeof(TraceMaterial)==80);
struct TraceLighting {
    glm::vec4 sunDirection{0,1,0,0};
    glm::vec4 sunRadiance{1,1,1,0};
    glm::vec4 environment{1,0,0,0}; // intensity, 0=gradient / 1=black
    glm::uvec4 counts{}; // emitter count
};
static_assert(sizeof(TraceLighting)==64);
struct TracePortal {
    glm::vec4 center{}; // w=1 for linked, 0 for unlinked
    glm::vec4 rightWidth{};
    glm::vec4 upHeight{};
    glm::vec4 normal{};
    glm::mat4 transfer{1};
    glm::mat4 inverseTransfer{1};
};
static_assert(sizeof(TracePortal)==192);
struct TraceBVHNode {
    glm::vec3 minimum{}; uint32_t first{};
    glm::vec3 maximum{}; uint32_t count{};
};
static_assert(sizeof(TraceBVHNode)==32);
struct TraceCPUHit { float t{1e30f}; int triangle{-1}; glm::vec2 bary{}; };
std::vector<TraceBVHNode> build_trace_bvh(std::vector<TraceTriangle>& triangles);
TraceCPUHit trace_cpu_intersect(const std::vector<TraceTriangle>& triangles,
    const std::vector<TraceBVHNode>& nodes, glm::vec3 origin, glm::vec3 direction, bool bruteForce);
void validate_trace_bvh(const std::vector<TraceTriangle>& triangles,const std::vector<TraceBVHNode>& nodes);
