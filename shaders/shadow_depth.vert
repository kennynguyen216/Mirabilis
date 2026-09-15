#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

// Depth-only pass rendered from the sunlight's orthographic camera.  It
// deliberately binds no descriptor sets: the light matrix is already folded
// into the push constant on the CPU, so this pipeline needs no scene or
// material data at all.

#include "vertex.glsl"

layout(push_constant) uniform constants {
    // sunViewProjection * model, combined once per object.
    mat4 lightMatrix;
    VertexBuffer vertexBuffer;
} PushConstants;

void main()
{
    Vertex vertex = PushConstants.vertexBuffer.vertices[gl_VertexIndex];
    gl_Position = PushConstants.lightMatrix * vec4(vertex.position, 1.0);
}
