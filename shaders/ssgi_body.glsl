// The SSGI trace, shared by two entry points: ssgi.comp (screen trace,
// environment for misses) and ssgi_lumen.comp, which defines LUMEN_LITE and
// sends misses through the scene distance field into the surface cache.
// Kept in one body so the plain pipeline compiles from exactly the code it
// always has.

#include "scene_data.glsl"
#include "ssgi_common.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(rgba16f, set = 1, binding = 0) uniform image2D rawIndirectImage;
layout(rgba16f, set = 1, binding = 1) uniform image2D diagnosticImage;
layout(set = 1, binding = 2) uniform sampler2D prepassDepth;
layout(set = 1, binding = 3) uniform sampler2D prepassNormal;
layout(set = 1, binding = 5) uniform sampler2D previousDirectLighting;
layout(set = 1, binding = 6) uniform sampler2D portalMask;
layout(set = 1, binding = 7) uniform sampler2D environmentTexture;
layout(set = 1, binding = 8) uniform sampler2D gbufferVelocity;

layout(push_constant) uniform constants {
    // xy = live extent, z = history valid, w = frame seed.
    uvec4 control;
    // x = ray length, y = surface thickness, z = origin offset,
    // w = step count.
    vec4 settings;
    // x = history weight, y = depth rejection threshold,
    // z = normal dot threshold, w = velocity rejection threshold.
    vec4 temporal;
    // x = rays per pixel, yz = live draw extent.
    uvec4 quality;
} PushConstants;

// The sky every miss ray is filled from, and the policy that decides how
// much of it a miss is worth.  Shared with the portal cameras, which have
// no screen-space pass of their own to fall back from.
#include "environment.glsl"

#ifdef LUMEN_LITE
// Lumen-lite world fallback: set 2 holds the scene distance field and the
// surface cache.  See docs/lumen_lite_design.md.
layout(set = 2, binding = 0) uniform sampler3D sceneField;
layout(set = 2, binding = 1) uniform sampler2D cacheAlbedo;
layout(set = 2, binding = 2) uniform sampler2D cacheEmissive;
layout(set = 2, binding = 3) uniform sampler2D cacheDepth;
layout(set = 2, binding = 4) uniform sampler2D cacheDirect;
layout(set = 2, binding = 5) uniform sampler2D cacheIndirect;
#define SURFACE_CACHE_SET 2
#define SURFACE_CACHE_BINDING_CARDS 6
#define SURFACE_CACHE_BINDING_GRID 7
#define SURFACE_CACHE_BINDING_INDICES 8
#define SURFACE_CACHE_READ_INDIRECT(texel) texelFetch(cacheIndirect, texel, 0).rgb
#include "surface_cache_lookup.glsl"

float lumen_field_distance(vec3 world)
{
    return textureLod(sceneField,
        (world - fieldMin.xyz) / (fieldMax.xyz - fieldMin.xyz), 0.0).r;
}

vec3 lumen_field_normal(vec3 p)
{
    float h = fieldMin.w;
    vec3 gradient = vec3(
        lumen_field_distance(p + vec3(h, 0, 0)) - lumen_field_distance(p - vec3(h, 0, 0)),
        lumen_field_distance(p + vec3(0, h, 0)) - lumen_field_distance(p - vec3(0, h, 0)),
        lumen_field_distance(p + vec3(0, 0, h)) - lumen_field_distance(p - vec3(0, 0, h)));
    return normalize(gradient + vec3(1e-8));
}

// Light arriving along one ray the screen could not answer: the surface the
// scene field hits, read from the surface cache, or the sky for a ray that
// leaves the scene.
vec3 lumen_trace(vec3 worldPosition, vec3 worldNormal, vec3 worldDirection)
{
    float voxel = fieldMin.w;
    vec3 origin = worldPosition + worldNormal * (2.5 * voxel);
    vec3 inverse = 1.0 / worldDirection;
    vec3 t0 = (fieldMin.xyz - origin) * inverse;
    vec3 t1 = (fieldMax.xyz - origin) * inverse;
    float tExit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    float t = 0.0;
    for (int step = 0; step < 192 && t < tExit; ++step) {
        float d = lumen_field_distance(origin + worldDirection * t);
        if (d < 0.25 * voxel) {
            vec3 p = origin + worldDirection * t;
            SurfaceCacheSample surface = surface_cache_lookup(p, lumen_field_normal(p));
            if (surface.weight > 0.0) {
                return surface.albedo * (surface.direct + surface.indirect) +
                    surface.emissive;
            }
            // A surface no card saw: its light is unknown.  The sky is the
            // same guess plain SSGI makes for every miss.
            return environment_radiance(worldDirection);
        }
        t += max(d, 0.5 * voxel);
    }
    // Left the field: the sky.  Ran out of steps while still inside: no
    // estimate rather than the sky, which would let daylight into a sealed
    // room (the same rule the cache's radiosity follows).
    return t >= tExit ? environment_radiance(worldDirection) : vec3(0.0);
}
#endif

const float Pi = 3.14159265359;
const float BackgroundDepth = 1e-6;

float randomFloat(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    word = (word >> 22u) ^ word;
    return float(word) * (1.0 / 4294967296.0);
}

vec2 frame_uv(vec2 screenUV, sampler2D source)
{
    return live_uv(screenUV, source, vec2(PushConstants.quality.yz));
}

vec3 view_position(vec2 screenUV, float depth)
{
    vec4 clip = vec4(screenUV * 2.0 - 1.0, depth, 1.0);
    vec4 view = sceneData.inverseProjection * clip;
    return view.xyz / view.w;
}

vec3 cosine_direction(vec3 normal, inout uint state)
{
    float u1 = randomFloat(state);
    float u2 = randomFloat(state);
    float radius = sqrt(u1);
    float angle = 2.0 * Pi * u2;
    vec3 local = vec3(radius * cos(angle), radius * sin(angle), sqrt(1.0 - u1));
    vec3 helper = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0)
                                          : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(helper, normal));
    vec3 bitangent = cross(normal, tangent);
    return normalize(tangent * local.x + bitangent * local.y + normal * local.z);
}

void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 extent = ivec2(PushConstants.control.xy);
    if (any(greaterThanEqual(pixel, extent))) {
        return;
    }

    vec2 screenUV = (vec2(pixel) + 0.5) / vec2(extent);
    vec2 depthUV = frame_uv(screenUV, prepassDepth);
    float depth = texture(prepassDepth, depthUV).r;
    if (depth <= BackgroundDepth) {
        imageStore(rawIndirectImage, pixel, vec4(0.0));
        imageStore(diagnosticImage, pixel, vec4(0.0));
        return;
    }

    vec3 position = view_position(screenUV, depth);
    vec3 normal = normalize(texture(prepassNormal,
        frame_uv(screenUV, prepassNormal)).xyz);
    uint state = uint(pixel.x) * 1973u ^ uint(pixel.y) * 9277u ^
        PushConstants.control.w * 26699u ^ 0x68bc21ebu;
    vec3 origin = position + normal * PushConstants.settings.z;
#ifdef LUMEN_LITE
    vec4 lumenWorld = sceneData.inverseViewProjection *
        vec4(screenUV * 2.0 - 1.0, depth, 1.0);
    vec3 lumenPosition = lumenWorld.xyz / lumenWorld.w;
    vec3 lumenNormal = normalize(transpose(mat3(sceneData.view)) * normal);
#endif
    int stepCount = max(1, int(PushConstants.settings.w + 0.5));
    float stride = PushConstants.settings.x / float(stepCount);
    int rayCount = clamp(int(PushConstants.quality.x), 1, 8);
    vec3 indirect = vec3(0.0);
    int hitSamples = 0;
    int portalSamples = 0;
    int totalSteps = 0;
    for (int rayIndex = 0; rayIndex < rayCount; ++rayIndex) {
        vec3 direction = cosine_direction(normal, state);
        bool hit = false;
        bool portalTermination = false;
        vec2 hitUV = vec2(0.0);
        float hitDepth = 0.0;
        int stepsTaken = 0;
        for (int stepIndex = 1; stepIndex <= stepCount; ++stepIndex) {
            stepsTaken = stepIndex;
            vec3 rayPoint = origin + direction * (stride * float(stepIndex));
            vec4 clip = sceneData.proj * vec4(rayPoint, 1.0);
            if (clip.w <= 0.0) break;
            vec3 ndc = clip.xyz / clip.w;
            vec2 rayUV = ndc.xy * 0.5 + 0.5;
            if (any(lessThan(rayUV, vec2(0.0))) ||
                any(greaterThanEqual(rayUV, vec2(1.0))) ||
                ndc.z < 0.0 || ndc.z > 1.0) break;
            if (texture(portalMask, frame_uv(rayUV, portalMask)).r > 0.5) {
                portalTermination = true;
                break;
            }
            float surfaceDepth = texture(
                prepassDepth, frame_uv(rayUV, prepassDepth)).r;
            if (surfaceDepth <= BackgroundDepth) continue;
            vec3 surfacePosition = view_position(rayUV, surfaceDepth);
            float separation = (-rayPoint.z) - (-surfacePosition.z);
            float allowedThickness = PushConstants.settings.y *
                (1.0 + 0.01 * max(-surfacePosition.z, 0.0));
            if (separation >= 0.0 && separation <= allowedThickness) {
                hit = true;
                hitUV = rayUV;
                hitDepth = surfaceDepth;
                break;
            }
        }

        vec3 incident = vec3(0.0);
        if (hit && PushConstants.control.z != 0u) {
            vec2 hitVelocity = texture(
                gbufferVelocity, frame_uv(hitUV, gbufferVelocity)).xy;
            vec2 previousHitUV = hitUV + hitVelocity;
            if (all(greaterThanEqual(previousHitUV, vec2(0.0))) &&
                all(lessThan(previousHitUV, vec2(1.0)))) {
                incident = texture(previousDirectLighting,
                    frame_uv(previousHitUV, previousDirectLighting)).rgb;
#ifdef LUMEN_LITE
                // The screen knows only the direct light leaving what it hit.
                // The cache knows that surface's bounce light too; adding it
                // gives screen hits the same bounces as world-traced misses,
                // which in a closed room are most rays.
                vec4 hitWorld = sceneData.inverseViewProjection *
                    vec4(hitUV * 2.0 - 1.0, hitDepth, 1.0);
                vec3 hitNormal = normalize(transpose(mat3(sceneData.view)) *
                    texture(prepassNormal, frame_uv(hitUV, prepassNormal)).xyz);
                SurfaceCacheSample hitSurface =
                    surface_cache_lookup(hitWorld.xyz / hitWorld.w, hitNormal);
                if (hitSurface.weight > 0.0) {
                    incident += hitSurface.albedo * hitSurface.indirect;
                }
#endif
            } else {
                hit = false;
            }
        }
        if (!hit || PushConstants.control.z == 0u) {
            vec3 worldDirection = normalize(
                transpose(mat3(sceneData.view)) * direction);
#ifdef LUMEN_LITE
            incident = portalTermination
                ? environment_radiance(worldDirection)
                : lumen_trace(lumenPosition, lumenNormal, worldDirection);
#else
            incident = environment_radiance(worldDirection);
#endif
        }
        indirect += incident;
        hitSamples += hit ? 1 : 0;
        portalSamples += portalTermination ? 1 : 0;
        totalSteps += stepsTaken;
    }
    // Incident radiance, not reflected colour: the composite multiplies by
    // the receiver's albedo after the temporal and spatial filters have run.
    indirect /= float(rayCount);
    imageStore(rawIndirectImage, pixel, vec4(indirect, 1.0));
    float hitFraction = float(hitSamples) / float(rayCount);
    float portalFraction = float(portalSamples) / float(rayCount);
    float classification = hitFraction > 0.0
        ? 1.0 : (portalFraction > 0.0 ? 0.5 : 0.0);
    imageStore(diagnosticImage, pixel, vec4(
        classification,
        float(totalSteps) / float(stepCount * rayCount),
        1.0 - hitFraction,
        portalFraction));

}
