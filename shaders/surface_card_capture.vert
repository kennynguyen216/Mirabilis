#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

// Lumen-lite surface cache capture: rasterises one instance into one card's
// atlas rectangle with an orthographic projection of its local bounds.

#include "input_structures.glsl"

layout(location = 0) out vec3 outWorldNormal;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec2 outUV;
// How much the vertex normal faces the card.  Interpolated, so a surface
// turned away from the card can be discarded per fragment.
layout(location = 3) out float outFacing;
// The same quantity before the two-sided flip below.  Reflectance is captured
// from both sides of a thin surface, but emission is one-sided, so the
// emissive page needs to know which card the authored normal actually faces.
layout(location = 4) out float outAuthoredFacing;

#include "vertex.glsl"

// The forward pass's push-constant block, reinterpreted: the three rows that
// carry the previous transform there carry the card projection here, mapping
// the mesh's LOCAL position to card clip space.  An orthographic projection's
// last row is (0, 0, 0, 1), so three rows are the whole matrix.
layout(push_constant) uniform constants {
    mat4 worldMatrix;
    VertexBuffer vertexBuffer;
    // The four bytes the forward pass leaves as alignment padding: 1 when
    // this instance is captured two-sided (see update_surface_cache).
    uint twoSided;
    vec4 cardRow0;
    vec4 cardRow1;
    vec4 cardRow2;
} PushConstants;

void main()
{
    Vertex vertex = PushConstants.vertexBuffer.vertices[gl_VertexIndex];
    vec4 local = vec4(vertex.position, 1.0);
    gl_Position = vec4(
        dot(PushConstants.cardRow0, local),
        dot(PushConstants.cardRow1, local),
        dot(PushConstants.cardRow2, local),
        1.0);

    // The depth row decreases along the card's viewing direction, so its
    // negation points from the surface toward the card.
    float facing = dot(vertex.normal, -PushConstants.cardRow2.xyz);
    outAuthoredFacing = facing;
    vec3 normal = vertex.normal;
    if (PushConstants.twoSided != 0u && facing < 0.0) {
        // A thin surface is seen from both sides, and the side this card sees
        // is the one facing it.
        facing = -facing;
        normal = -normal;
    }
    outFacing = facing;

    mat3 model = mat3(PushConstants.worldMatrix);
    outWorldNormal = normalize(transpose(inverse(model)) * normal);
    outColor = vertex.color * materialData.colorFactors;
    vec2 uvScale = materialData.uvTransform.xy;
    if (all(lessThan(abs(uvScale), vec2(0.0001)))) {
        uvScale = vec2(1.0);
    }
    outUV = vec2(vertex.uv_x, vertex.uv_y) * uvScale + materialData.uvTransform.zw;
}
