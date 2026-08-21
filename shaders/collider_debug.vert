#version 450

layout(push_constant) uniform constants
{
    mat4 viewProjection;
    mat4 model;
} PushConstants;

void main()
{
    const vec3 corners[8] = vec3[](
        vec3(-0.5, -0.5, -0.5), vec3( 0.5, -0.5, -0.5),
        vec3( 0.5,  0.5, -0.5), vec3(-0.5,  0.5, -0.5),
        vec3(-0.5, -0.5,  0.5), vec3( 0.5, -0.5,  0.5),
        vec3( 0.5,  0.5,  0.5), vec3(-0.5,  0.5,  0.5));
    const int edges[24] = int[](
        0, 1, 1, 2, 2, 3, 3, 0,
        4, 5, 5, 6, 6, 7, 7, 4,
        0, 4, 1, 5, 2, 6, 3, 7);

    vec3 position = corners[edges[gl_VertexIndex]];
    gl_Position = PushConstants.viewProjection * PushConstants.model *
        vec4(position, 1.0);
}
