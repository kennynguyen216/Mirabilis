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

layout(set = 3, binding = 0) uniform sampler2D gbufferAlbedo;
layout(set = 3, binding = 1) uniform sampler2D gbufferVelocity;
layout(set = 3, binding = 2) uniform sampler2D portalMask;
layout(set = 3, binding = 3) uniform sampler2D directLighting;
layout(set = 3, binding = 4) uniform sampler2D ssgiRaw;
layout(set = 3, binding = 5) uniform sampler2D ssgiDiagnostic;
layout(set = 3, binding = 6) uniform sampler2D ssgiTemporal;
layout(set = 3, binding = 7) uniform sampler2D ssgiTemporalDiagnostic;
layout(set = 3, binding = 8) uniform sampler2D ssgiFiltered;
layout(set = 3, binding = 9) uniform sampler2D ssgiReference;
layout(set = 3, binding = 10) uniform sampler2D ssgiFallback;

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
const int ModeAlbedo = 8;
const int ModeMotionVectors = 9;
const int ModePortalMask = 10;
const int ModeDirectLighting = 11;
const int ModeSSGIRaw = 12;
const int ModeSSGIHitMiss = 13;
const int ModeSSGISteps = 14;
const int ModeSSGITemporal = 15;
const int ModeSSGIHistoryRejection = 16;
const int ModeSSGIReprojection = 17;
const int ModeSSGIFiltered = 18;
const int ModeSSGIFallback = 19;
const int ModeSSGIReferenceDifference = 20;

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    int mode = int(PushConstants.settings.x + 0.5);

    if (mode >= ModeAlbedo) {
        ivec2 ssgiPixel = sceneData.screenSpaceSettings.w > 0.5
            ? pixel / 2 : pixel;
        if (mode == ModeAlbedo) {
            outFragColor = vec4(texelFetch(gbufferAlbedo, pixel, 0).rgb, 1.0);
        } else if (mode == ModeMotionVectors) {
            vec2 velocity = texelFetch(gbufferVelocity, pixel, 0).xy;
            outFragColor = vec4(
                clamp(vec3(0.5 + velocity.x * 20.0,
                           0.5 + velocity.y * 20.0,
                           length(velocity) * 40.0), 0.0, 1.0), 1.0);
        } else if (mode == ModePortalMask) {
            float mask = texelFetch(portalMask, pixel, 0).r;
            outFragColor = vec4(mask, 0.0, mask, 1.0);
        } else if (mode == ModeDirectLighting) {
            outFragColor = vec4(texelFetch(directLighting, pixel, 0).rgb, 1.0);
        } else if (mode == ModeSSGIRaw) {
            outFragColor = vec4(texelFetch(ssgiRaw, ssgiPixel, 0).rgb, 1.0);
        } else if (mode == ModeSSGITemporal) {
            outFragColor = vec4(texelFetch(ssgiTemporal, ssgiPixel, 0).rgb, 1.0);
        } else if (mode == ModeSSGIHistoryRejection) {
            vec4 diagnostic = texelFetch(
                ssgiTemporalDiagnostic, ssgiPixel, 0);
            vec3 reason = diagnostic.r < 0.125
                ? vec3(0.1, 0.9, 0.1)
                : (diagnostic.r < 0.375 ? vec3(0.15, 0.25, 1.0)
                : (diagnostic.r < 0.625 ? vec3(1.0, 0.15, 0.1)
                : (diagnostic.r < 0.875 ? vec3(1.0, 0.85, 0.1)
                                        : vec3(1.0, 0.0, 1.0))));
            outFragColor = vec4(reason, 1.0);
        } else if (mode == ModeSSGIReprojection) {
            vec4 diagnostic = texelFetch(
                ssgiTemporalDiagnostic, ssgiPixel, 0);
            outFragColor = vec4(diagnostic.g, diagnostic.b,
                1.0 - diagnostic.b, 1.0);
        } else if (mode == ModeSSGIFiltered) {
            outFragColor = vec4(texelFetch(
                ssgiFiltered, ssgiPixel, 0).rgb, 1.0);
        } else if (mode == ModeSSGIFallback) {
            outFragColor = vec4(
                texelFetch(ssgiFallback, ssgiPixel, 0).rgb, 1.0);
        } else if (mode == ModeSSGIReferenceDifference) {
            // The reference is path-traced reflected radiance, while the SSGI
            // buffers now hold incident radiance with the receiver's albedo
            // left for the composite to apply.  Applying it here is what
            // keeps the two sides of this subtraction the same quantity.
            vec3 filtered = texelFetch(ssgiFiltered, ssgiPixel, 0).rgb *
                texelFetch(gbufferAlbedo, pixel, 0).rgb;
            vec3 reference = texelFetch(ssgiReference, pixel, 0).rgb;
            vec3 difference = abs(filtered - reference);
            // Log-like false colour keeps both subtle and large errors visible.
            float error = 1.0 - exp(-8.0 * max(
                difference.r, max(difference.g, difference.b)));
            outFragColor = vec4(
                clamp(vec3(error * 2.0, 1.0 - abs(error * 2.0 - 1.0),
                    1.0 - error * 2.0), 0.0, 1.0), 1.0);
        } else {
            vec2 diagnostic = texelFetch(
                ssgiDiagnostic, ssgiPixel, 0).rg;
            if (mode == ModeSSGIHitMiss) {
                vec3 classification = diagnostic.r > 0.75
                    ? vec3(0.1, 1.0, 0.1)
                    : (diagnostic.r > 0.25
                        ? vec3(1.0, 0.0, 1.0)
                        : vec3(0.05, 0.2, 1.0));
                outFragColor = vec4(classification, 1.0);
            } else {
                float steps = diagnostic.g;
                outFragColor = vec4(
                    clamp(vec3(steps * 2.0, 1.0 - abs(steps * 2.0 - 1.0),
                               1.0 - steps * 2.0), 0.0, 1.0), 1.0);
            }
        }
        return;
    }

    // The occlusion views come first because they must show the background as
    // white.  The reconstruction below returns early on background depth, and
    // black there would look exactly like full occlusion.
    if (mode >= ModeOcclusionRaw) {
        // Half resolution, so each occlusion texel covers a 2x2 block.  The
        // allocation is half the draw image in the same way, which is why the
        // active region needs no scaling here.
        ivec2 occlusionPixel = pixel / 2;
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
