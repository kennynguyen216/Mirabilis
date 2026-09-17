#version 450
#extension GL_GOOGLE_include_directive : require

#include "ssgi_common.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

layout(set = 0, binding = 0) uniform sampler2D filteredIndirect;
// r = depth, gb = octahedral view normal, a = portal mask.
layout(set = 0, binding = 1) uniform sampler2D ssgiMetadata;
layout(set = 0, binding = 2) uniform sampler2D prepassDepth;
layout(set = 0, binding = 3) uniform sampler2D prepassNormal;
// The forward pass's base-colour target.  The trace stores incident radiance,
// so the receiver's albedo is applied here: once, at full resolution, and
// after every filter that would otherwise have been smearing one surface's
// colour across its neighbour.
layout(set = 0, binding = 4) uniform sampler2D gbufferAlbedo;

layout(push_constant) uniform constants {
    // xy = full render extent, zw = active SSGI extent.
    vec4 extents;
    // x = intensity, y = half-resolution flag, z = depth falloff,
    // w = normal exponent (the same two the bilateral filter uses).
    vec4 settings;
} PushConstants;

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    ivec2 giExtent = ivec2(PushConstants.extents.zw);
    vec3 indirect = vec3(0.0);
    if (PushConstants.settings.y < 0.5) {
        indirect = texelFetch(filteredIndirect, pixel, 0).rgb;
    } else {
        float centerDepth = texelFetch(prepassDepth, pixel, 0).r;
        vec3 centerNormal = normalize(
            texelFetch(prepassNormal, pixel, 0).xyz);
        ivec2 base = pixel / 2;
        float weightSum = 0.0;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                ivec2 sourcePixel = clamp(base + ivec2(x, y),
                    ivec2(0), giExtent - ivec2(1));
                vec4 metadata = texelFetch(
                    ssgiMetadata, sourcePixel, 0);
                vec3 sourceNormal = decode_octahedron(metadata.gb);
                float depthWeight = exp(-abs(metadata.r - centerDepth) *
                    PushConstants.settings.z);
                float normalWeight = pow(
                    max(dot(centerNormal, sourceNormal), 0.0),
                    PushConstants.settings.w);
                vec2 sourceCenter = vec2(sourcePixel * 2 + ivec2(1));
                float spatialWeight = exp(
                    -dot(sourceCenter - vec2(pixel),
                         sourceCenter - vec2(pixel)) * 0.18);
                float weight = depthWeight * normalWeight * spatialWeight;
                indirect += texelFetch(
                    filteredIndirect, sourcePixel, 0).rgb * weight;
                weightSum += weight;
            }
        }
        if (weightSum > 0.00001) {
            indirect /= weightSum;
        } else {
            indirect = texelFetch(filteredIndirect,
                clamp(base, ivec2(0), giExtent - ivec2(1)), 0).rgb;
        }
    }
    // texelFetch, not texture(): this pass is one output pixel per full-
    // resolution G-buffer pixel, so there is nothing to interpolate and no
    // live-region bound to clamp against.
    vec3 albedo = texelFetch(gbufferAlbedo, pixel, 0).rgb;
    outFragColor = vec4(indirect * albedo, PushConstants.settings.x);
}
