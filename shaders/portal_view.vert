#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

#include "input_structures.glsl"

layout(location=7) flat out float outStaticCacheMember;
layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec3 outWorldPosition;
// xyz = world-space tangent, w = bitangent sign; w = 0 means no tangent.
layout(location = 4) out vec4 outTangent;

#include "vertex.glsl"
#include "draw_instance.glsl"

layout(push_constant) uniform constants {
    mat4 render_matrix;
    VertexBuffer vertexBuffer;
    DrawInstances instances;
} PushConstants;

void main()
{
    Vertex vertex = PushConstants.vertexBuffer.vertices[gl_VertexIndex];
    bool instanced=false;
    DrawInstance instance;
    mat4 world=PushConstants.render_matrix;
    if(instanced){instance=PushConstants.instances.values[gl_InstanceIndex];world=instance.world;}
    vec4 worldPosition = world * vec4(vertex.position, 1.0);
    gl_Position = sceneData.viewproj * worldPosition;
    outWorldPosition = worldPosition.xyz;
    // Clip before rasterization. Doing this in the fragment shader with
    // discard lets the rejected wall fragments reach depth/stencil first,
    // which is exactly what can leave a black aperture at a portal crossing.
    gl_ClipDistance[0] = sceneData.portalClipEnabled.x > 0.5
        ? dot(worldPosition, sceneData.portalClipPlane)
        : 1.0;
    // Keep lighting and shadow bias correct under non-uniform object scale.
    mat3 model = mat3(world);
    float modelDeterminant=determinant(model);
    mat3 fallbackNormal=(!isnan(modelDeterminant) && !isinf(modelDeterminant) && abs(modelDeterminant)>1e-12)
        ? transpose(inverse(model)) : mat3(1);
    bool hasInstance=any(notEqual(uvec2(PushConstants.instances),uvec2(0)));
    outStaticCacheMember=instanced ? 1.0-instance.flags.z :
        (hasInstance ? 1.0-PushConstants.instances.values[0].flags.z : 1.0);
    outNormal=instanced?instance_normal(instance,vertex.normal):normalize(fallbackNormal*vertex.normal);
    // Same tangent frame as mesh.vert: model matrix, handedness flipped by a
    // mirroring transform.
    outTangent = vec4(
        model * vertex.tangent.xyz,
        vertex.tangent.w * (instanced?instance.flags.y:(modelDeterminant < 0.0 ? -1.0 : 1.0)));
    outColor = vertex.color * materialData.colorFactors;
    vec2 uvScale = materialData.uvTransform.xy;
    if (all(lessThan(abs(uvScale), vec2(0.0001)))) {
        uvScale = vec2(1.0);
    }
    outUV = vec2(vertex.uv_x, vertex.uv_y) * uvScale +
        materialData.uvTransform.zw;
}
