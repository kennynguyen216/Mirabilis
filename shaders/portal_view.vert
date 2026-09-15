#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

#include "input_structures.glsl"

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec3 outWorldPosition;

#include "vertex.glsl"

layout(push_constant) uniform constants {
    mat4 render_matrix;
    VertexBuffer vertexBuffer;
} PushConstants;

void main()
{
    Vertex vertex = PushConstants.vertexBuffer.vertices[gl_VertexIndex];
    vec4 worldPosition = PushConstants.render_matrix * vec4(vertex.position, 1.0);
    gl_Position = sceneData.viewproj * worldPosition;
    outWorldPosition = worldPosition.xyz;
    // Clip before rasterization. Doing this in the fragment shader with
    // discard lets the rejected wall fragments reach depth/stencil first,
    // which is exactly what can leave a black aperture at a portal crossing.
    gl_ClipDistance[0] = sceneData.portalClipEnabled.x > 0.5
        ? dot(worldPosition, sceneData.portalClipPlane)
        : 1.0;
    // Keep lighting and shadow bias correct under non-uniform object scale.
    mat3 normalMatrix = transpose(inverse(mat3(PushConstants.render_matrix)));
    outNormal = normalize(normalMatrix * vertex.normal);
    outColor = vertex.color * materialData.colorFactors;
    vec2 uvScale = materialData.uvTransform.xy;
    if (all(lessThan(abs(uvScale), vec2(0.0001)))) {
        uvScale = vec2(1.0);
    }
    outUV = vec2(vertex.uv_x, vertex.uv_y) * uvScale +
        materialData.uvTransform.zw;
}
