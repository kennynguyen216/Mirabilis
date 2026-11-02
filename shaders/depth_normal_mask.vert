#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

// The alpha-tested variant of depth_normal.vert.  Unlike that shader this one
// does include the material set, because its fragment stage has to sample the
// base colour to know whether the fragment exists.
#include "input_structures.glsl"

layout(location = 0) out vec3 outViewNormal;
layout(location = 2) flat out float outDoubleSided;
layout(location=3) out vec3 outViewPosition;
layout(location = 1) out vec2 outUV;

#include "vertex.glsl"
#include "draw_instance.glsl"

layout(push_constant) uniform constants {
    mat4 render_matrix;
    VertexBuffer vertexBuffer;
    DrawInstances instances;
    layout(offset=80) vec4 flags;

} PushConstants;

void main()
{
    Vertex vertex = PushConstants.vertexBuffer.vertices[gl_VertexIndex];
    vec4 worldPosition = PushConstants.render_matrix * vec4(vertex.position, 1.0);
    gl_Position = sceneData.viewproj * worldPosition;

    // Identical to depth_normal.vert, so the silhouettes the two pipelines
    // produce line up to the pixel where they meet.
    bool instanced=any(notEqual(uvec2(PushConstants.instances),uvec2(0)));
    DrawInstance instance;
    if(instanced)instance=PushConstants.instances.values[0];
    vec3 worldNormal=instanced?instance_normal(instance,vertex.normal):normalize(transpose(inverse(mat3(PushConstants.render_matrix)))*vertex.normal);
    outViewNormal = mat3(sceneData.view) * worldNormal;
    outViewPosition=(sceneData.view*worldPosition).xyz;
    outDoubleSided=PushConstants.flags.x;

    vec2 uv = vec2(vertex.uv_x, vertex.uv_y);
    vec2 scale = materialData.uvTransform.xy;
    if (scale == vec2(0.0)) {
        scale = vec2(1.0);
    }
    outUV = uv * scale + materialData.uvTransform.zw;
}
