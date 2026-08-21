#version 450
#extension GL_GOOGLE_include_directive : require

#include "input_structures.glsl"

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

// The offscreen-camera experiment has already lit the destination scene.
// Sample it directly here: lighting it a second time would darken or tint the
// view and make the A/B comparison misleading.
void main()
{
    // A Vulkan render target's vertical orientation is opposite the portal
    // quad's mesh UV convention, so flip V while sampling the camera image.
    outFragColor = texture(colorTex, vec2(inUV.x, 1.0 - inUV.y));
}
