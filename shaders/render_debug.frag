#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_data.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

// The depth/normal prepass targets.  They are read with texelFetch, so the
// images may be larger than the region actually rendered this frame.
layout(set = 1, binding = 0) uniform sampler2D prepassDepth;
layout(set = 1, binding = 1) uniform sampler2D prepassNormal;

// The three occlusion stages, at half resolution.  They are read with
// texelFetch too, so what appears on screen is the stored texel rather than a
// filtered version of it.
layout(set = 2, binding = 0) uniform sampler2D occlusionRaw;
layout(set = 2, binding = 1) uniform sampler2D occlusionBlurred;
layout(set = 2, binding = 2) uniform sampler2D occlusionFinal;

layout(push_constant) uniform constants {
    // x = debug mode, yz = rendered extent, w = distance that maps to white
    // in the depth view.
    vec4 settings;
} PushConstants;

const int ModeDepth = 1;
const int ModeViewNormal = 2;
const int ModeViewPosition = 3;
const int ModeWorldPosition = 4;
const int ModeOcclusionRaw = 5;
const int ModeOcclusionBlurred = 6;
const int ModeOcclusionFinal = 7;

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    int mode = int(PushConstants.settings.x + 0.5);

    // The occlusion views come first because they must show the background as
    // white.  The reconstruction below returns early on background depth, and
    // black there would look exactly like full occlusion.
    if (mode >= ModeOcclusionRaw) {
        // Match composition's normalized mapping, including odd dimensions.
        vec2 aoExtent = ceil(PushConstants.settings.yz * 0.5);
        ivec2 occlusionPixel = min(ivec2(gl_FragCoord.xy /
            PushConstants.settings.yz * aoExtent), ivec2(aoExtent) - 1);
        if (sceneData.screenSpaceSettings.x < 0.5) {
            outFragColor = vec4(1.0);
            return;
        }
        float occlusion = 1.0;
        if (mode == ModeOcclusionRaw) {
            occlusion = texelFetch(occlusionRaw, occlusionPixel, 0).r;
        } else if (mode == ModeOcclusionBlurred) {
            occlusion = texelFetch(occlusionBlurred, occlusionPixel, 0).r;
        } else {
            occlusion = texelFetch(occlusionFinal, occlusionPixel, 0).r;
        }
        outFragColor = vec4(vec3(occlusion), 1.0);
        return;
    }

    float depth = texelFetch(prepassDepth, pixel, 0).r;

    // The camera's depth is reversed, so the cleared far value is 0.  Nothing
    // was drawn here and there is no position to reconstruct.
    if (depth <= 0.0) {
        outFragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // gl_FragCoord and Vulkan's NDC both run down the screen, and the stored
    // projection already carries the Y flip, so its inverse undoes both
    // without a correction here.
    vec2 ndc = (gl_FragCoord.xy / PushConstants.settings.yz) * 2.0 - 1.0;
    vec4 clipPosition = vec4(ndc, depth, 1.0);

    vec4 viewPosition = sceneData.inverseProjection * clipPosition;
    viewPosition.xyz /= viewPosition.w;

    vec3 color = vec3(0.0);
    if (mode == ModeDepth) {
        // Metres in front of the camera rather than the raw non-linear value,
        // which would be almost entirely white at every playable distance.
        float distanceAhead = -viewPosition.z;
        color = vec3(clamp(distanceAhead / max(PushConstants.settings.w, 0.001), 0.0, 1.0));
    } else if (mode == ModeViewNormal) {
        vec3 viewNormal = normalize(texelFetch(prepassNormal, pixel, 0).xyz);
        color = viewNormal * 0.5 + 0.5;
    } else if (mode == ModeViewPosition) {
        // A repeating one-metre ramp: flat surfaces read as smooth bands and
        // any projection mistake shows up as a discontinuity or a warp.
        color = fract(viewPosition.xyz);
    } else if (mode == ModeWorldPosition) {
        vec4 worldPosition = sceneData.inverseViewProjection * clipPosition;
        worldPosition.xyz /= worldPosition.w;
        color = fract(worldPosition.xyz);
    }

    outFragColor = vec4(color, 1.0);
}
