// Helpers shared by the SSGI passes: the resampling every pass needs because
// its inputs are allocated at the full window size and only partly written,
// and the octahedral normal packing the metadata history is stored in.

// screenUV is in the live region's own 0..1; this maps it into the texture's
// allocated space and clamps it half a texel inside the live region.
vec2 live_uv(vec2 screenUV, sampler2D source, vec2 liveExtent)
{
    vec2 allocation = vec2(textureSize(source, 0));
    vec2 liveFraction = liveExtent / allocation;
    return clamp(screenUV * liveFraction, vec2(0.5) / allocation,
        liveFraction - vec2(0.5) / allocation);
}

vec2 encode_octahedron(vec3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 encoded = n.xy;
    if (n.z < 0.0) encoded = (1.0 - abs(encoded.yx)) * sign(encoded.xy);
    return encoded * 0.5 + 0.5;
}

vec3 decode_octahedron(vec2 encoded)
{
    vec2 f = encoded * 2.0 - 1.0;
    vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    return normalize(n);
}
