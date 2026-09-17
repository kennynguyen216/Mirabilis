#version 450
#extension GL_GOOGLE_include_directive : require

#include "input_structures.glsl"

layout(location = 0) in vec3 inWorldNormal;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) in float inFacing;

// Base colour, alpha 1 wherever something was captured.
layout(location = 0) out vec4 outAlbedo;
// World normal encoded to [0, 1].
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outEmissive;
// Card depth: 0 at the card's face of the bounding box, 1 at the far face.
// Cleared to 1, which is how a lookup tells an empty texel.
layout(location = 3) out float outDepth;

void main()
{
    // The engine draws with culling off, so this is the back-face test: a
    // surface turned away from the card belongs to the card on the opposite
    // side.  Without it, a room's near wall would hide the far wall from the
    // very card meant to capture it.
    if (inFacing <= 0.0) {
        discard;
    }
    vec4 baseColor = inColor * texture(colorTex, inUV);
    // alphaMask.x is 0 on every material that is not alpha-tested, which makes
    // this a no-op there.
    if (baseColor.a < materialData.alphaMask.x) {
        discard;
    }
    outAlbedo = vec4(baseColor.rgb, 1.0);
    outNormal = vec4(normalize(inWorldNormal) * 0.5 + 0.5, 1.0);
    outEmissive = vec4(materialData.emission.rgb, 1.0);
    outDepth = gl_FragCoord.z;
}
