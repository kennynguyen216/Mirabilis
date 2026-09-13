#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

// The alpha-tested variant of depth_normal.vert.  Unlike that shader this one
// does include the material set, because its fragment stage has to sample the
// base colour to know whether the fragment exists.
#include "input_structures.glsl"

layout(location = 0) out vec3 outViewNormal;
layout(location = 1) out vec2 outUV;

struct Vertex {
    vec3 position;
    float uv_x;
    vec3 normal;
    float uv_y;
    vec4 color;
};

layout(buffer_reference, std430) readonly buffer VertexBuffer {
    Vertex vertices[];
};

layout(push_constant) uniform constants {
    mat4 render_matrix;
    VertexBuffer vertexBuffer;
} PushConstants;

void main()
{
    Vertex vertex = PushConstants.vertexBuffer.vertices[gl_VertexIndex];
    vec4 worldPosition = PushConstants.render_matrix * vec4(vertex.position, 1.0);
    gl_Position = sceneData.viewproj * worldPosition;

    // Identical to depth_normal.vert, so the silhouettes the two pipelines
    // produce line up to the pixel where they meet.
    mat3 normalMatrix = transpose(inverse(mat3(PushConstants.render_matrix)));
    vec3 worldNormal = normalize(normalMatrix * vertex.normal);
    outViewNormal = mat3(sceneData.view) * worldNormal;

    vec2 uv = vec2(vertex.uv_x, vertex.uv_y);
    vec2 scale = materialData.uvTransform.xy;
    if (scale == vec2(0.0)) {
        scale = vec2(1.0);
    }
    outUV = uv * scale + materialData.uvTransform.zw;
}
