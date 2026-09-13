#version 450
#extension GL_EXT_buffer_reference : require

// The alpha-tested variant of shadow_depth.vert.  That shader binds nothing
// at all; this one needs the base colour texture to know which fragments
// exist, so it declares the material set - and only the material set - at
// set 0.  A descriptor set layout is compatible by shape rather than by the
// index it was first written for, so the same per-material set the forward
// pass binds at 1 binds here at 0.
layout(set = 0, binding = 0) uniform GLTFMaterialData {
    vec4 colorFactors;
    vec4 metal_rough_factors;
    vec4 uvTransform;
    vec4 alphaMask;
} materialData;

layout(location = 0) out vec2 outUV;

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
    // sunViewProjection * model, combined once per object, exactly as in
    // shadow_depth.vert so both pipelines take the same push constant.
    mat4 lightMatrix;
    VertexBuffer vertexBuffer;
} PushConstants;

void main()
{
    Vertex vertex = PushConstants.vertexBuffer.vertices[gl_VertexIndex];
    gl_Position = PushConstants.lightMatrix * vec4(vertex.position, 1.0);

    // Same convention as mesh.vert: a zero scale means the material predates
    // the UV transform and is sampled untransformed.
    vec2 scale = materialData.uvTransform.xy;
    if (scale == vec2(0.0)) {
        scale = vec2(1.0);
    }
    outUV = vec2(vertex.uv_x, vertex.uv_y) * scale + materialData.uvTransform.zw;
}
