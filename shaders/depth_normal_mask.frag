#version 450
#extension GL_GOOGLE_include_directive : require

// The alpha-tested variant of depth_normal.frag.  Without it, occlusion and
// screen-space GI would be computed against the solid quad a leaf texture is
// drawn on rather than against the leaves.
#include "input_structures.glsl"
#include "alpha_mask.glsl"

layout(location = 0) in vec3 inViewNormal;
layout(location=2) flat in float inDoubleSided;
layout(location=3) in vec3 inViewPosition;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec4 outNormal;

void main()
{
    apply_alpha_mask(materialData.colorFactors.a * texture(colorTex, inUV).a);
    vec3 normal=normalize(inViewNormal);
    if(inDoubleSided>0.5 && dot(normal,-inViewPosition)<0.0)normal=-normal;
    outNormal=vec4(normal,1);
}
