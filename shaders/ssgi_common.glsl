// Helpers shared by the SSGI passes: the resampling every pass needs because
// its inputs are allocated at the full window size and only partly written,
// and the octahedral normal packing the metadata history is stored in.

// A depth sample describes the centre of its full-resolution texel. Half-
// resolution GI centres land between those texels; reconstructing there
// instead puts the surface off its real plane (below the floor at distance).
vec2 ssgi_geometry_uv(vec2 screenUV, vec2 liveExtent)
{
    vec2 pixel = clamp(floor(screenUV * liveExtent), vec2(0.0), liveExtent - 1.0);
    return (pixel + 0.5) / liveExtent;
}

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
    if (n.z < 0.0) encoded = (1.0 - abs(encoded.yx)) *
        mix(vec2(-1.0), vec2(1.0), greaterThanEqual(encoded.xy, vec2(0.0)));
    return encoded * 0.5 + 0.5;
}

vec3 decode_octahedron(vec2 encoded)
{
    vec2 f = encoded * 2.0 - 1.0;
    vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) *
        mix(vec2(-1.0), vec2(1.0), greaterThanEqual(n.xy, vec2(0.0)));
    return normalize(n);
}

// Perspective coefficients: P[0][0], P[1][1], P[2][2], P[3][2].
vec3 ssgi_view_position(vec2 uv, float depth, vec4 projection)
{
    float distance = projection.w / (depth + projection.z);
    return vec3((uv * 2.0 - 1.0) * distance / projection.xy, -distance);
}

float ssgi_plane_weight(vec3 center, vec3 samplePosition, vec3 normal, float falloff)
{
    // Coplanar taps keep their weight even on a sloping wall; other planes do not.
    float planeDistance = abs(dot(normal, samplePosition - center));
    return exp(-planeDistance / max(-center.z, 1.0) * falloff);
}
