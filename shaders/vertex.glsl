// The one GLSL declaration of the vertex layout.  Every geometry pass reads
// vertices through this buffer reference, so the std430 layout here must stay
// byte-identical to Vertex in src/vk_types.h.  A shader that declared its own
// copy would read every vertex after the first at the wrong offset, and the
// validation layers cannot detect that.
//
// Requires GL_EXT_buffer_reference.
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
