#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

#include "input_structures.glsl"

// This pass writes no colour; it only marks the portal silhouette in stencil
// and sets its depth to the farthest possible value.  That clears the wall's
// main-camera depth only inside the opening before the virtual camera draws.
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
    gl_Position = sceneData.viewproj * PushConstants.render_matrix *
        vec4(vertex.position, 1.0);

    // The engine uses reversed depth (near = 1, far = 0).
    gl_Position.z = 0.0;
    outNormal = vec3(0.0, 1.0, 0.0);
    outColor = vec4(1.0);
    outUV = vec2(vertex.uv_x, vertex.uv_y);
}
