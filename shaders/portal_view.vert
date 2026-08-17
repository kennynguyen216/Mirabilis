#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

#include "input_structures.glsl"

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec2 outUV;

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
    // Clip before rasterization. Doing this in the fragment shader with
    // discard lets the rejected wall fragments reach depth/stencil first,
    // which is exactly what can leave a black aperture at a portal crossing.
    gl_ClipDistance[0] = sceneData.portalClipEnabled.x > 0.5
        ? dot(worldPosition, sceneData.portalClipPlane)
        : 1.0;
    outNormal = normalize((PushConstants.render_matrix * vec4(vertex.normal, 0.0)).xyz);
    outColor = vertex.color * materialData.colorFactors;
    outUV = vec2(vertex.uv_x, vertex.uv_y);
}
