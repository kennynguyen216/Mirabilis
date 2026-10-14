#version 450
#extension GL_GOOGLE_include_directive : require

#include "ssgi_common.glsl"
#include "indirect_color.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

layout(set = 0, binding = 0) uniform sampler2D filteredIndirect;
// r = depth, gb = octahedral world normal, a = portal mask.
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
    vec4 colorSettings;
    vec4 projection;
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
        if (centerDepth <= 1e-6) {
            outFragColor = vec4(0.0);
            return;
        }
        vec2 centerUV = (vec2(pixel) + 0.5) / PushConstants.extents.xy;
        vec3 centerPosition = ssgi_view_position(centerUV, centerDepth, PushConstants.projection);
        vec3 centerNormal = normalize(
            texelFetch(prepassNormal, pixel, 0).xyz);
        ivec2 base = ivec2(centerUV * vec2(giExtent));
        float weightSum = 0.0;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                ivec2 sourcePixel = clamp(base + ivec2(x, y),
                    ivec2(0), giExtent - ivec2(1));
                vec4 metadata = texelFetch(
                    ssgiMetadata, sourcePixel, 0);
                if (metadata.r <= 1e-6 || metadata.a > 0.5) continue;
                vec2 sourceUV = ssgi_geometry_uv((vec2(sourcePixel) + 0.5) / vec2(giExtent),
                    PushConstants.extents.xy);
                // Metadata normals are world-space for history; the current
                // prepass gives both upsample normals in the same view space.
                vec3 sourceNormal = normalize(texture(prepassNormal,
                    live_uv(sourceUV, prepassNormal, PushConstants.extents.xy)).xyz);
                float depthWeight = ssgi_plane_weight(centerPosition,
                    ssgi_view_position(sourceUV, metadata.r, PushConstants.projection),
                    centerNormal, PushConstants.settings.z);
                float normalWeight = pow(
                    max(dot(centerNormal, sourceNormal), 0.0),
                    PushConstants.settings.w);
                vec2 sourceCenter = sourceUV * PushConstants.extents.xy - 0.5;
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
        }
    }
    // texelFetch, not texture(): this pass is one output pixel per full-
    // resolution G-buffer pixel, so there is nothing to interpolate and no
    // live-region bound to clamp against.
    vec3 albedo = texelFetch(gbufferAlbedo, pixel, 0).rgb;
    outFragColor = vec4(indirect_color(indirect, PushConstants.colorSettings.x) * albedo, PushConstants.settings.x);
}
