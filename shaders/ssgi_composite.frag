#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

layout(set = 0, binding = 0) uniform sampler2D filteredIndirect;
// r = depth, gb = octahedral view normal, a = portal mask.
layout(set = 0, binding = 1) uniform sampler2D ssgiMetadata;
layout(set = 0, binding = 2) uniform sampler2D prepassDepth;
layout(set = 0, binding = 3) uniform sampler2D prepassNormal;

layout(push_constant) uniform constants {
    // xy = full render extent, zw = active SSGI extent.
    vec4 extents;
    // x = intensity, y = half-resolution flag.
    vec4 settings;
} PushConstants;

vec3 decode_octahedron(vec2 encoded)
{
    vec2 f = encoded * 2.0 - 1.0;
    vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    }
    return normalize(n);
}

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
                float depthWeight = exp(
                    -abs(metadata.r - centerDepth) * 800.0);
                float normalWeight = pow(
                    max(dot(centerNormal, sourceNormal), 0.0), 32.0);
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
    outFragColor = vec4(indirect, PushConstants.settings.x);
}
