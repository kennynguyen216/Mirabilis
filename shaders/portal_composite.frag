#version 450
#extension GL_GOOGLE_include_directive : require

#include "input_structures.glsl"

layout(location = 0) in vec4 inClip;
layout(location = 0) out vec4 outFragColor;

// The offscreen-camera experiment has already lit the destination scene.
// Sample it directly here: lighting it a second time would darken or tint the
// view and make the A/B comparison misleading.
void main()
{
    if (inClip.z < 0.0 || inClip.w <= 0.0) discard;
    gl_FragDepth = clamp(inClip.z / inClip.w, 0.0, 1.0);
    // This camera image already uses the main view's projection. Mesh UVs
    // zoom into one sky texel as the aperture approaches the eye.
    vec2 screenUV = inClip.xy / inClip.w * 0.5 + 0.5;
    outFragColor = texture(colorTex, screenUV);
}
