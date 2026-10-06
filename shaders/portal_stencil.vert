#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#include "input_structures.glsl"
#include "vertex.glsl"
layout(location = 0) out vec2 outClipDepth;

layout(push_constant) uniform constants {
    mat4 render_matrix;
    VertexBuffer vertexBuffer;
} PushConstants;

void main()
{
    Vertex vertex = PushConstants.vertexBuffer.vertices[gl_VertexIndex];
    gl_Position = sceneData.viewproj * PushConstants.render_matrix *
        vec4(vertex.position, 1.0);
    outClipDepth = gl_Position.zw;
    // Rasterize the aperture's unchanged XY/W silhouette. Per-vertex near
    // clamping deforms eye-plane clipping when only some corners are near.
    gl_Position.z = 0.0;
}
