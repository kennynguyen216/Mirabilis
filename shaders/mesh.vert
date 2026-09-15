#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

#include "input_structures.glsl"

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec3 outWorldPosition;
layout(location = 4) out vec4 outCurrentClip;
layout(location = 5) out vec4 outPreviousClip;

#include "vertex.glsl"

layout(push_constant) uniform constants {
    mat4 render_matrix;
    VertexBuffer vertexBuffer;
    vec4 previousWorldRow0;
    vec4 previousWorldRow1;
    vec4 previousWorldRow2;
} PushConstants;

void main()
{
    Vertex vertex = PushConstants.vertexBuffer.vertices[gl_VertexIndex];
    vec4 worldPosition = PushConstants.render_matrix * vec4(vertex.position, 1.0);

    gl_Position = sceneData.viewproj * worldPosition;
    outCurrentClip = gl_Position;
    vec4 localPosition = vec4(vertex.position, 1.0);
    vec4 previousWorldPosition = vec4(
        dot(PushConstants.previousWorldRow0, localPosition),
        dot(PushConstants.previousWorldRow1, localPosition),
        dot(PushConstants.previousWorldRow2, localPosition),
        1.0);
    outPreviousClip = sceneData.previousViewProjection * previousWorldPosition;
    // The shadow lookup happens in world space, so it works unchanged for
    // the main camera and for every portal camera.
    outWorldPosition = worldPosition.xyz;
    // Normals use the inverse transpose so editor-authored non-uniform scale
    // does not skew either diffuse lighting or the shadow normal offset.
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
