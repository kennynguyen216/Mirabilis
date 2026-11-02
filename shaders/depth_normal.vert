#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

// Only the camera block is included, not input_structures.glsl: this pass
// reads no material, so declaring set 1 here would force the pipeline layout
// to carry descriptors the prepass never binds.
#include "scene_data.glsl"

layout(location = 0) out vec3 outViewNormal;
layout(location = 2) flat out float outDoubleSided;
layout(location=3) out vec3 outViewPosition;

#include "vertex.glsl"
#include "draw_instance.glsl"

// Deliberately the same push-constant block as the main pass, so the prepass
// can be fed by exactly the same draw loop and produce silhouettes that line
// up with the shaded image to the pixel.
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

    // Same inverse transpose as mesh.vert, so a non-uniformly scaled object
    // reports the same normal to ambient occlusion as it does to lighting.
    bool instanced=any(notEqual(uvec2(PushConstants.instances),uvec2(0)));
    DrawInstance instance;
    if(instanced)instance=PushConstants.instances.values[0];
    vec3 worldNormal=instanced?instance_normal(instance,vertex.normal):normalize(transpose(inverse(mat3(PushConstants.render_matrix)))*vertex.normal);
    // View space, because screen-space occlusion reconstructs its positions
    // in view space too.  Keeping both in one space avoids a per-sample
    // transform in the inner loop of the AO pass.
    outViewNormal = mat3(sceneData.view) * worldNormal;
    outViewPosition=(sceneData.view*worldPosition).xyz;
    outDoubleSided=PushConstants.flags.x;
}
