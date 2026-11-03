// Lumen-lite surface cache lookup: what the cache says about a world-space
// surface point.  Shared by every pass that reads the cache at an arbitrary
// position -- the comparison debug view now, radiosity and the final gather
// later.
//
// The including shader declares, before including this file:
//   sampler2D cacheAlbedo, cacheEmissive, cacheDepth, cacheDirect
//   SURFACE_CACHE_READ_INDIRECT(ivec2 texel) -> vec3, reading the indirect
//     page (the radiosity pass reads the storage image it is writing)
//   SURFACE_CACHE_READ_SKY(ivec2 texel) -> vec3 only for primary sky lookups
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
    // x first candidate, y count, z exact-lookup acceleration enabled.
    uvec4 cells[];
};

layout(std430, set = SURFACE_CACHE_SET, binding = SURFACE_CACHE_BINDING_INDICES)
readonly buffer SurfaceCacheIndices {
    // x card index; y first candidate after its exact-direction group.
    uvec2 cardIndices[];
};

struct SurfaceCacheSample {
    // Sum of weights; 0 when no card saw the point.
    float weight;
    vec3 albedo;
    vec3 emissive;
    // Light arriving at the surface (no albedo), as in the direct page.
    vec3 direct;
    // The direct page's alpha, blended the same way (the emitter page keeps
    // its visible fraction there, R4.12).
    float directAlpha;
    // Bounce light arriving at the surface, in the same convention.
    vec3 indirect;
    vec3 sky;
};

#ifndef SURFACE_CACHE_READ_SKY
#define SURFACE_CACHE_READ_SKY(texel) vec3(0.0)
#endif

// Where a card sees a world point, if it sees it at all: the atlas texel, how
// far behind the point the captured surface lies along the card's viewing
// direction (negative in front), and how squarely the surface faces the card.
bool card_probe(uint cardIndex, vec4 p, vec3 normal, float minimumFacing,
    out ivec2 texel, out float behind, out float facing, out vec2 cardUV)
{
    SurfaceCacheCard card = cards[cardIndex];
    facing = dot(normal, card.direction.xyz);
    if (facing < minimumFacing) {
        return false;
    }
    vec3 c = vec3(dot(card.worldToCard0, p), dot(card.worldToCard1, p),
                  dot(card.worldToCard2, p));
    if (any(lessThan(c.xy, vec2(0.0))) || any(greaterThan(c.xy, vec2(1.0)))) {
        return false;
    }
    cardUV = c.xy;
    texel = ivec2(card.rect.xy) + min(
        ivec2(c.xy * vec2(card.rect.zw)), ivec2(card.rect.zw) - 1);
    float stored = texelFetch(cacheDepth, texel, 0).r;
    if (stored >= 1.0) {
        return false;
    }
    behind = (stored - c.z) * card.direction.w;
    return true;
}

// IQ10: the sky page read bilinearly inside one card, over the texels that
// captured the same surface as the centre texel; the nearest read turned each
// 5 cm texel into a flat square wherever the cache is seen directly.
vec3 surface_cache_sky_bilinear(SurfaceCacheCard card, vec2 cardUV, ivec2 centre, float margin)
{
    vec2 f = cardUV * vec2(card.rect.zw) - 0.5;
    ivec2 base = ivec2(floor(f));
    vec2 t = f - vec2(base);
    float centreDepth = texelFetch(cacheDepth, centre, 0).r;
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int i = 0; i < 4; ++i) {
        ivec2 corner = ivec2(i & 1, i >> 1);
        ivec2 texel = ivec2(card.rect.xy) +
            clamp(base + corner, ivec2(0), ivec2(card.rect.zw) - 1);
        float stored = texelFetch(cacheDepth, texel, 0).r;
        float weight = (corner.x == 1 ? t.x : 1.0 - t.x) * (corner.y == 1 ? t.y : 1.0 - t.y);
        if (stored >= 1.0 || abs(stored - centreDepth) * card.direction.w > margin) {
            weight = 0.0;
        }
        sum += weight * SURFACE_CACHE_READ_SKY(texel);
        total += weight;
    }
    return total > 1e-4 ? sum / total : SURFACE_CACHE_READ_SKY(centre);
}

// The cache's fixed sky noise needs a spatial filter even at the main camera.
// Portal receivers have no screen-space denoiser. Filter their incoming light
// in card space, rejecting other surfaces just as the bilinear sky read does.
// page: 0 sky, 1 indirect, 2 direct lighting used by GI transport.
vec3 surface_cache_light(ivec2 texel, int page)
{
    if (page == 0) return SURFACE_CACHE_READ_SKY(texel);
    if (page == 1) return SURFACE_CACHE_READ_INDIRECT(texel);
#ifdef SURFACE_CACHE_READ_DIRECT
    return SURFACE_CACHE_READ_DIRECT(texel, true).rgb;
#else
    return texelFetch(cacheDirect, texel, 0).rgb;
#endif
}

vec3 surface_cache_receiver_filtered(SurfaceCacheCard card, vec2 cardUV, ivec2 centre,
    float margin, int page, int radius)
{
    vec2 f = cardUV * vec2(card.rect.zw) - 0.5;
    ivec2 base = ivec2(floor(f));
    float centreDepth = texelFetch(cacheDepth, centre, 0).r;
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int y = 1 - radius; y <= radius; ++y) {
        for (int x = 1 - radius; x <= radius; ++x) {
            ivec2 local = base + ivec2(x, y);
            ivec2 texel = ivec2(card.rect.xy) + clamp(local, ivec2(0), ivec2(card.rect.zw) - 1);
            float stored = texelFetch(cacheDepth, texel, 0).r;
            if (stored >= 1.0 || abs(stored - centreDepth) * card.direction.w > margin)
                continue;
            vec2 tent = max(vec2(0.0), 1.0 - abs(vec2(local) - f) / float(radius));
            float weight = tent.x * tent.y;
            sum += weight * surface_cache_light(texel, page);
            total += weight;
        }
    }
    return total > 1e-4 ? sum / total :
        surface_cache_light(centre, page);
}

SurfaceCacheSample surface_cache_lookup(vec3 world, vec3 normal, bool readSky, bool bilinearSky,
    bool readLighting, int lightingRadius)
{
    SurfaceCacheSample result;
    result.weight = 0.0;
    result.albedo = vec3(0.0);
    result.emissive = vec3(0.0);
    result.direct = vec3(0.0);
    result.directAlpha = 0.0;
    result.indirect = vec3(0.0);
    result.sky = vec3(0.0);

    vec3 cellCoord = (world - gridMin.xyz) / gridMin.w;
    ivec3 cell = ivec3(floor(cellCoord));
    ivec3 dims = ivec3(gridDimensions.xyz);
    if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, dims))) {
        return result;
    }
    uvec4 range = cells[(cell.z * dims.y + cell.y) * dims.x + cell.x];
    // Every candidate matters: truncating changes coverage with instance order.
    uint count = range.y;
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
    // the tolerance instead averaged a ceiling light with the dark ceiling two
    // centimetres above it and halved the light in the room.  The first pass
    // finds that nearest surface; the second blends only the cards that
    // captured it.
    float nearest = 1e30;
    float secondNearest = 1e30;
    uint nearestCard = 0u;
    ivec2 nearestTexel = ivec2(0);
    vec2 nearestUV = vec2(0);
    float nearestFacing = 0.0;
    for (uint i = 0u; i < count;) {
        uvec2 candidate = cardIndices[range.x + i];
        ++i;
        ivec2 texel;
        float behind;
        float facing;
        vec2 cardUV;
        if (!card_probe(candidate.x, p, normal, minimumFacing,
                texel, behind, facing, cardUV)) {
            if (facing < minimumFacing) i = candidate.y - range.x;
            continue;
        }
        if (behind < -frontTolerance) {
            continue;
        }
        if (behind < nearest) {
            secondNearest = nearest;
            nearest = behind;
            nearestCard = candidate.x;
            nearestTexel = texel;
            nearestUV = cardUV;
            nearestFacing = facing;
        } else secondNearest = min(secondNearest, behind);
    }
    if (nearest > depthTolerance) {
        return result;
    }

    // Every other card then has weight < exp(-9), below the existing 1e-3 cutoff.
    bool single = range.z != 0u && secondNearest - nearest > 3.0 * sameSurfaceMargin;
    for (uint i = 0u; i < (single ? 1u : count);) {
        uvec2 candidate = single ? uvec2(nearestCard, 0) : cardIndices[range.x + i];
        ++i;
        ivec2 texel = nearestTexel;
        float behind = nearest;
        float facing = nearestFacing;
        vec2 cardUV = nearestUV;
        if (!single) {
            if (!card_probe(candidate.x, p, normal, minimumFacing,
                    texel, behind, facing, cardUV)) {
                if (facing < minimumFacing) i = candidate.y - range.x;
                continue;
            }
            if (behind < -frontTolerance) continue;
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
        if (readSky) {
            result.sky += weight * (readLighting && bilinearSky
                ? surface_cache_receiver_filtered(cards[candidate.x], cardUV, texel,
                    sameSurfaceMargin, 0, 3)
                : bilinearSky
                ? surface_cache_sky_bilinear(cards[candidate.x], cardUV, texel,
                    sameSurfaceMargin)
                : SURFACE_CACHE_READ_SKY(texel));
            continue;
        }
        result.albedo += weight * texelFetch(cacheAlbedo, texel, 0).rgb;
        result.emissive += weight * texelFetch(cacheEmissive, texel, 0).rgb;
#ifdef SURFACE_CACHE_READ_DIRECT
        vec4 direct = SURFACE_CACHE_READ_DIRECT(texel, readLighting);
#else
        vec4 direct = texelFetch(cacheDirect, texel, 0);
#endif
        // GI integrates reflected radiance over a footprint. A nearest sun
        // shadow or noisy bounce texel otherwise prints a grid on the receiver.
        // The forward sun shadow remains evaluated at the actual surface.
        if (readLighting) {
            direct.rgb = surface_cache_receiver_filtered(cards[candidate.x],
                cardUV, texel, sameSurfaceMargin, 2, lightingRadius);
        }
        result.direct += weight * direct.rgb;
        result.directAlpha += weight * direct.a;
        result.indirect += weight * (bilinearSky
            ? surface_cache_receiver_filtered(cards[candidate.x], cardUV, texel,
                sameSurfaceMargin, 1, lightingRadius)
            : SURFACE_CACHE_READ_INDIRECT(texel));
    }
    if (result.weight > 0.0) {
        result.albedo /= result.weight;
        result.emissive /= result.weight;
        result.direct /= result.weight;
        result.directAlpha /= result.weight;
        result.indirect /= result.weight;
        result.sky /= result.weight;
    }
    return result;
}

SurfaceCacheSample surface_cache_lookup(vec3 world, vec3 normal, bool readSky, bool bilinearSky, bool readLighting)
{
    // Ray hits get bilinear interpolation; portal receivers explicitly ask
    // for the wider filter because they have no screen-space denoiser.
    return surface_cache_lookup(world, normal, readSky, bilinearSky, readLighting, 1);
}

SurfaceCacheSample surface_cache_lookup(vec3 world, vec3 normal, bool readSky, bool bilinearSky)
{
    return surface_cache_lookup(world, normal, readSky, bilinearSky, false);
}

SurfaceCacheSample surface_cache_lookup(vec3 world, vec3 normal)
{
    return surface_cache_lookup(world, normal, false, false);
}
