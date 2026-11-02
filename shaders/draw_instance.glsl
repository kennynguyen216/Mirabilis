#extension GL_EXT_buffer_reference_uvec2 : require
struct DrawInstance {
    mat4 world;
    vec4 previousRows[3];
    vec4 normalRows[3];
    vec4 flags;
};
layout(buffer_reference,std430,buffer_reference_align=16) readonly buffer DrawInstances {DrawInstance values[];};
vec3 instance_normal(DrawInstance instance,vec3 normal)
{
    return normalize(vec3(dot(instance.normalRows[0].xyz,normal),dot(instance.normalRows[1].xyz,normal),dot(instance.normalRows[2].xyz,normal)));
}
