// Lumen-lite surface cache lookup: what the cache says about a world-space
// surface point.  Shared by every pass that reads the cache at an arbitrary
// position -- the comparison debug view now, radiosity and the final gather
// later.
//
// The including shader declares, before including this file:
//   sampler2D cacheAlbedo, cacheEmissive, cacheDepth, cacheDirect
//   SURFACE_CACHE_READ_INDIRECT(ivec2 texel) -> vec3, reading the indirect
//     page (the radiosity pass reads the storage image it is writing)
//   the three SSBOs below at SURFACE_CACHE_SET / SURFACE_CACHE_BINDING_*

struct SurfaceCacheCard {
    // World position -> (u, v, depth) in card space; u and v span the card
    // in [0, 1], depth matches the depth page.
    vec4 worldToCard0;
    vec4 worldToCard1;
    vec4 worldToCard2;
    // xyz = world direction a surface faces to be seen by this card,
    // w = world extent along the card's viewing axis.
    vec4 direction;
    // x, y, width, height in atlas texels.
    uvec4 rect;
};

layout(std430, set = SURFACE_CACHE_SET, binding = SURFACE_CACHE_BINDING_CARDS)
readonly buffer SurfaceCacheCards {
    SurfaceCacheCard cards[];
};

// A uniform world grid; each cell lists the cards whose boxes overlap it.
layout(std430, set = SURFACE_CACHE_SET, binding = SURFACE_CACHE_BINDING_GRID)
readonly buffer SurfaceCacheGrid {
    // xyz = grid minimum corner, w = cell size.
    vec4 gridMin;
    // xyz = cell counts, w = total card count.
    uvec4 gridDimensions;
    // x = how far behind a query point a captured surface may be, in world
    // units; y = minimum facing (cosine); z = how far in front; w = the margin
    // within which cards count as capturing the same surface.
    vec4 lookupParameters;
    // The scene distance field: xyz = minimum corner, w = voxel size.
    vec4 fieldMin;
    // xyz = maximum corner.
    vec4 fieldMax;
    // xyz = direction toward the sun.
    vec4 sunDirection;
    // x = first index into cardIndices, y = count.
    uvec4 cells[];
};

layout(std430, set = SURFACE_CACHE_SET, binding = SURFACE_CACHE_BINDING_INDICES)
readonly buffer SurfaceCacheIndices {
    uint cardIndices[];
};

struct SurfaceCacheSample {
    // Sum of weights; 0 when no card saw the point.
    float weight;
    vec3 albedo;
    vec3 emissive;
    // Light arriving at the surface (no albedo), as in the direct page.
    vec3 direct;
    // Bounce light arriving at the surface, in the same convention.
    vec3 indirect;
};

SurfaceCacheSample surface_cache_lookup(vec3 world, vec3 normal)
{
    SurfaceCacheSample result;
    result.weight = 0.0;
    result.albedo = vec3(0.0);
    result.emissive = vec3(0.0);
    result.direct = vec3(0.0);
    result.indirect = vec3(0.0);

    vec3 cellCoord = (world - gridMin.xyz) / gridMin.w;
    ivec3 cell = ivec3(floor(cellCoord));
    ivec3 dims = ivec3(gridDimensions.xyz);
    if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, dims))) {
        return result;
    }
    uvec4 range = cells[(cell.z * dims.y + cell.y) * dims.x + cell.x];
    const uint MaxCardsPerLookup = 64u;
    uint count = min(range.y, MaxCardsPerLookup);
    float depthTolerance = lookupParameters.x;
    float minimumFacing = lookupParameters.y;
    // How far in front of the query point a captured surface may sit (depth
    // noise when the query is itself a surface position), and how far behind
    // the nearest surface another card may be and still be the same surface.
    float frontTolerance = lookupParameters.z;
    float sameSurfaceMargin = lookupParameters.w;
    vec4 p = vec4(world, 1.0);

    // A distance-field hit lands in front of the surface it found, so the
    // surface meant is the first one behind it.  Blending every card within
    // the tolerance instead averaged a ceiling light with the dark ceiling
    // two centimetres above it and halved the light in the room.  The first
    // pass finds that nearest surface; the second blends only the cards that
    // captured it.
    float nearest = 1e30;
    for (uint i = 0u; i < count; ++i) {
        SurfaceCacheCard card = cards[cardIndices[range.x + i]];
        if (dot(normal, card.direction.xyz) < minimumFacing) {
            continue;
        }
        vec3 c = vec3(dot(card.worldToCard0, p), dot(card.worldToCard1, p),
                      dot(card.worldToCard2, p));
        if (any(lessThan(c.xy, vec2(0.0))) || any(greaterThan(c.xy, vec2(1.0)))) {
            continue;
        }
        ivec2 texel = ivec2(card.rect.xy) + min(
            ivec2(c.xy * vec2(card.rect.zw)), ivec2(card.rect.zw) - 1);
        float stored = texelFetch(cacheDepth, texel, 0).r;
        if (stored >= 1.0) {
            continue;
        }
        // Positive when the captured surface lies behind the query point as
        // seen along the card's viewing direction.
        float behind = (stored - c.z) * card.direction.w;
        if (behind < -frontTolerance || behind > depthTolerance) {
            continue;
        }
        nearest = min(nearest, behind);
    }
    if (nearest > depthTolerance) {
        return result;
    }

    for (uint i = 0u; i < count; ++i) {
        SurfaceCacheCard card = cards[cardIndices[range.x + i]];
        float facing = dot(normal, card.direction.xyz);
        if (facing < minimumFacing) {
            continue;
        }
        vec3 c = vec3(dot(card.worldToCard0, p), dot(card.worldToCard1, p),
                      dot(card.worldToCard2, p));
        if (any(lessThan(c.xy, vec2(0.0))) || any(greaterThan(c.xy, vec2(1.0)))) {
            continue;
        }
        ivec2 texel = ivec2(card.rect.xy) + min(
            ivec2(c.xy * vec2(card.rect.zw)), ivec2(card.rect.zw) - 1);
        float stored = texelFetch(cacheDepth, texel, 0).r;
        if (stored >= 1.0) {
            continue;
        }
        float behind = (stored - c.z) * card.direction.w;
        if (behind < -frontTolerance) {
            continue;
        }
        // Full weight for the nearest surface and any other card that
        // captured it at the same depth; a surface even a couple of
        // centimetres further back fades out fast, because two surfaces that
        // close (a light panel under a ceiling) are still different surfaces.
        float further = max(behind - nearest, 0.0) / sameSurfaceMargin;
        float weight = facing * exp(-further * further);
        if (weight < 1e-3) {
            continue;
        }
        result.weight += weight;
        result.albedo += weight * texelFetch(cacheAlbedo, texel, 0).rgb;
        result.emissive += weight * texelFetch(cacheEmissive, texel, 0).rgb;
        result.direct += weight * texelFetch(cacheDirect, texel, 0).rgb;
        result.indirect += weight * SURFACE_CACHE_READ_INDIRECT(texel);
    }
    if (result.weight > 0.0) {
        result.albedo /= result.weight;
        result.emissive /= result.weight;
        result.direct /= result.weight;
        result.indirect /= result.weight;
    }
    return result;
}
