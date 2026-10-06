#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#include "input_structures.glsl"
#include "vertex.glsl"

layout(location = 0) out vec4 outClip;
layout(push_constant) uniform constants {
    mat4 render_matrix;
    VertexBuffer vertexBuffer;
} PushConstants;

void main()
{
    Vertex vertex = PushConstants.vertexBuffer.vertices[gl_VertexIndex];
    gl_Position = sceneData.viewproj * PushConstants.render_matrix * vec4(vertex.position, 1.0);
    outClip = gl_Position;
    // The composite must cover the same eye-clipped silhouette as its stencil.
    gl_Position.z = 0.0;
}
