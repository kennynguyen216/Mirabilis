#version 450

// The shadow pass's first fragment stage, and it exists only to discard.  It
// writes no colour - the pass has no colour attachment - and leaves the depth
// the rasterizer produced, so a surviving fragment casts exactly the shadow
// the opaque pipeline would have cast for it.
layout(set = 0, binding = 0) uniform GLTFMaterialData {
    vec4 colorFactors;
    vec4 metal_rough_factors;
    vec4 uvTransform;
    vec4 alphaMask;
} materialData;

layout(set = 0, binding = 1) uniform sampler2D colorTex;

layout(location = 0) in vec2 inUV;

void main()
{
    float alpha = materialData.colorFactors.a * texture(colorTex, inUV).a;
    if (alpha < materialData.alphaMask.x) {
        discard;
    }
}
