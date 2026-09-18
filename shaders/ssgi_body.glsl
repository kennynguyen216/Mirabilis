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
    // x = rays per pixel, yz = live draw extent, w = bit 0 screen probes,
    // bit 1 hierarchical screen trace.
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
#define SCENE_FIELD_SET 2
#define SCENE_FIELD_BINDING_CASCADES 9
#include "scene_field.glsl"

// Light arriving along one ray the screen could not answer: the surface the
// scene field hits, read from the surface cache, or the sky for a ray that
// leaves the scene.
vec3 lumen_trace(vec3 worldPosition, vec3 worldNormal, vec3 worldDirection)
{
    vec3 origin = worldPosition + worldNormal * (2.5 * scene_field_voxel(worldPosition));
    vec3 inverse = 1.0 / worldDirection;
    vec3 t0 = (fieldMin.xyz - origin) * inverse;
    vec3 t1 = (fieldMax.xyz - origin) * inverse;
    float tExit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    float t = 0.0;
    for (int step = 0; step < 192 && t < tExit; ++step) {
        float voxel;
        float d = scene_field_distance(origin + worldDirection * t, voxel);
        if (d < 0.25 * voxel) {
            vec3 p = origin + worldDirection * t;
            SurfaceCacheSample surface = surface_cache_lookup(p, scene_field_normal(p));
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

// Hierarchical screen trace (quality.w bit 1) through the depth pyramid
// hzb_build.comp writes: each level keeps the nearest and farthest depth of
// the 2x2 below it.  A ray in front of a cell's nearest, or behind its
// farthest by more than a hit's thickness, cannot hit anything in it, so it
// skips the whole cell and tries the next a level up; otherwise it drops a
// level.  At level 0 the same thickness test as the fixed march decides a
// hit, and a ray behind a thin surface carries on past it, so a hit means
// what it did before.  Without the farthest depth, those rays had to walk
// behind every column in Sponza a pixel at a time.
layout(set = 1, binding = 15) uniform sampler2D hzbDepth;
const int HzbLevels = 8;

bool hzb_enabled()
{
    return (PushConstants.quality.w & 2u) != 0u;
}

// View-space distance in front of the camera for a reversed NDC depth.
float linear_depth(float ndcDepth)
{
    vec4 view = sceneData.inverseProjection * vec4(0.0, 0.0, ndcDepth, 1.0);
    return -view.z / view.w;
}

bool hzb_march(vec3 origin, vec3 direction, out vec2 hitUV, out float hitDepth,
    out bool portalTermination, out int iterations)
{
    hitUV = vec2(0.0);
    hitDepth = 0.0;
    portalTermination = false;
    iterations = 0;
    // Keep the far end in front of the camera (view space looks down -z).
    const float CameraClearance = -0.05;
    if (origin.z > CameraClearance) return false;
    float rayLength = PushConstants.settings.x;
    vec3 end = origin + direction * rayLength;
    if (end.z > CameraClearance) {
        end = origin + direction * ((CameraClearance - origin.z) / direction.z);
    }
    // The ray as a segment in pyramid pixels and reversed NDC depth.  A line
    // stays a line under projection, so depth varies linearly along it too.
    vec2 extent = vec2(PushConstants.quality.yz);
    vec4 c0 = sceneData.proj * vec4(origin, 1.0);
    vec4 c1 = sceneData.proj * vec4(end, 1.0);
    vec3 p0 = vec3((c0.xy / c0.w * 0.5 + 0.5) * extent, c0.z / c0.w);
    vec3 p1 = vec3((c1.xy / c1.w * 0.5 + 0.5) * extent, c1.z / c1.w);
    vec3 d = p1 - p0;
    // Clip to the screen: past its edge the screen has no answer.
    float tEnd = 1.0;
    for (int axis = 0; axis < 2; ++axis) {
        if (d[axis] < 0.0) tEnd = min(tEnd, -p0[axis] / d[axis]);
        else if (d[axis] > 0.0) tEnd = min(tEnd, (extent[axis] - p0[axis]) / d[axis]);
    }
    float pixelsPerT = max(abs(d.x), abs(d.y));
    if (tEnd <= 0.0 || pixelsPerT < 1e-3) return false;
    float nudge = 0.01 / pixelsPerT;
    vec2 inverse = vec2(abs(d.x) > 1e-6 ? 1.0 / d.x : 1e30, abs(d.y) > 1e-6 ? 1.0 / d.y : 1e30);

    // Start one pixel out, clear of the surface the ray leaves.  A ray that is
    // already behind what the screen shows there started inside something --
    // a surface whose normal points into it, like the Cornell box's emitter,
    // offsets the origin through itself -- and the screen cannot say what it
    // sees.  The fixed march never noticed, as its first sample lands a whole
    // stride out, past the thickness a hit allows.
    float t = 1.0 / pixelsPerT;
    vec3 start = p0 + d * t;
    if (start.z < texelFetch(hzbDepth, ivec2(floor(start.xy)), 0).r) return false;
    int level = 0;
    int maxIterations = 4 * max(1, int(PushConstants.settings.w + 0.5));
    while (iterations < maxIterations && t < tEnd) {
        ++iterations;
        vec3 p = p0 + d * t;
        float cellSize = float(1 << level);
        vec2 cell = floor(p.xy / cellSize);
        vec2 boundary = (cell + step(0.0, d.xy)) * cellSize;
        vec2 tBoundary = (boundary - p0.xy) * inverse;
        tBoundary = mix(tBoundary, vec2(1e30), lessThan(tBoundary, vec2(t)));
        float tExit = min(min(tBoundary.x, tBoundary.y), tEnd);
        vec2 range = texelFetch(hzbDepth, ivec2(cell), level).rg;
        float nearest = range.r;
        float zExit = p0.z + d.z * tExit;
        // Skippable if the ray is in front of everything in this cell for as
        // long as it is in it, or behind everything by more than the thickness
        // a hit allows (checked against the farthest surface, whose allowance
        // is the largest).
        float farDepth = linear_depth(range.g);
        float allowedBehind = PushConstants.settings.y * (1.0 + 0.01 * farDepth);
        bool inFront = p.z > nearest && zExit > nearest;
        bool behind = linear_depth(p.z) - farDepth > allowedBehind &&
            linear_depth(zExit) - farDepth > allowedBehind;
        if (inFront || behind) {
            t = tExit + nudge;
            level = min(level + 1, HzbLevels - 1);
            continue;
        }
        // Behind at entry, or crossing behind inside the cell.
        if (p.z > nearest) t = max(t, (nearest - p0.z) / d.z);
        if (level > 0) {
            --level;
            continue;
        }
        vec2 uv = (cell + 0.5) / extent;
        if (nearest > BackgroundDepth) {
            vec3 rayPoint = view_position((p0.xy + d.xy * t) / extent, p0.z + d.z * t);
            vec3 surfacePosition = view_position(uv, nearest);
            float separation = (-rayPoint.z) - (-surfacePosition.z);
            float allowedThickness = PushConstants.settings.y *
                (1.0 + 0.01 * max(-surfacePosition.z, 0.0));
            if (separation >= 0.0 && separation <= allowedThickness) {
                // A portal is a surface the screen cannot see through.
                if (texture(portalMask, frame_uv(uv, portalMask)).r > 0.5) {
                    portalTermination = true;
                    return false;
                }
                hitUV = uv;
                hitDepth = nearest;
                return true;
            }
        }
        // Behind a thin surface: walk on to the next pixel.
        t = tExit + nudge;
    }
    return false;
}

// A surface point the trace starts from, in the view space the screen march
// works in and the world space the scene field works in.
struct TraceOrigin {
    vec3 position;
    vec3 normal;
    vec3 worldPosition;
    vec3 worldNormal;
};

TraceOrigin trace_origin(vec2 screenUV, float depth)
{
    TraceOrigin o;
    o.position = view_position(screenUV, depth);
    o.normal = normalize(texture(prepassNormal,
        frame_uv(screenUV, prepassNormal)).xyz);
    vec4 world = sceneData.inverseViewProjection *
        vec4(screenUV * 2.0 - 1.0, depth, 1.0);
    o.worldPosition = world.xyz / world.w;
    o.worldNormal = normalize(transpose(mat3(sceneData.view)) * o.normal);
    return o;
}

// Radiance arriving at o along one view-space direction: the screen march
// first, then the scene field for what the screen cannot answer.
vec3 trace_incident(TraceOrigin o, vec3 direction,
    out bool hit, out bool portalTermination, out int stepsTaken)
{
    vec3 origin = o.position + o.normal * PushConstants.settings.z;
    int stepCount = max(1, int(PushConstants.settings.w + 0.5));
    float stride = PushConstants.settings.x / float(stepCount);
    hit = false;
    portalTermination = false;
    vec2 hitUV = vec2(0.0);
    float hitDepth = 0.0;
    stepsTaken = 0;
    if (hzb_enabled()) {
        hit = hzb_march(origin, direction, hitUV, hitDepth,
            portalTermination, stepsTaken);
    } else for (int stepIndex = 1; stepIndex <= stepCount; ++stepIndex) {
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
            : lumen_trace(o.worldPosition, o.worldNormal, worldDirection);
#else
        incident = environment_radiance(worldDirection);
#endif
    }
    return incident;
}

#ifdef LUMEN_LITE
// Screen probes.  One probe per ProbeTile x ProbeTile pixel tile, placed on a
// different pixel of its tile every frame, traces the rays its tile's pixels
// would have traced (the same budget) over its hemisphere and keeps them as
// band-2 spherical harmonics.  Each pixel then reads irradiance for its own
// normal from the probes around it, so it is estimated from every ray in
// several tiles rather than the handful it traced itself.  The existing
// temporal pass accumulates across the jittered placements.
//
// ponytail: no importance sampling, probe-space filtering or adaptive
// placement yet; pixels no probe can serve fall back to their own rays.
const int ProbeTile = 8;
layout(rgba16f, set = 1, binding = 14) uniform image2D probeRadiance;

bool probes_enabled()
{
    return (PushConstants.quality.w & 1u) != 0u;
}

// The pixel a probe sits on this frame.  A hash of the tile and the frame, so
// the probe pass and every pixel that gathers from it agree without storing
// it.
ivec2 probe_pixel(ivec2 probe, ivec2 extent)
{
    uint h = uint(probe.x) * 73856093u ^ uint(probe.y) * 19349663u ^
        PushConstants.control.w * 83492791u;
    h = (h ^ (h >> 16)) * 0x7feb352du;
    h ^= h >> 15;
    ivec2 offset = ivec2(h & 7u, (h >> 3) & 7u);
    return min(probe * ProbeTile + offset, extent - 1);
}

// Real band-2 SH, the same basis and order as sky_irradiance() in
// surface_cache_direct.comp.
void sh_basis(vec3 n, out float y[9])
{
    y[0] = 0.282095;
    y[1] = 0.488603 * n.y;
    y[2] = 0.488603 * n.z;
    y[3] = 0.488603 * n.x;
    y[4] = 1.092548 * n.x * n.y;
    y[5] = 1.092548 * n.y * n.z;
    y[6] = 0.315392 * (3.0 * n.z * n.z - 1.0);
    y[7] = 1.092548 * n.x * n.z;
    y[8] = 0.546274 * (n.x * n.x - n.y * n.y);
}

// Mean cosine-weighted incident radiance around n (irradiance / pi, what the
// per-pixel trace averages to) from a probe's radiance coefficients: the
// clamped-cosine convolution, bands scaled by pi, 2pi/3 and pi/4, over pi.
vec3 probe_incident(ivec2 probe, vec3 n)
{
    float y[9];
    sh_basis(n, y);
    const float band[9] = float[9](1.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0,
        0.25, 0.25, 0.25, 0.25, 0.25);
    vec3 e = vec3(0.0);
    for (int i = 0; i < 9; ++i) {
        e += imageLoad(probeRadiance, ivec2(probe.x * 9 + i, probe.y)).rgb *
            band[i] * y[i];
    }
    return max(e, vec3(0.0));
}
#endif

#ifdef SSGI_PROBE_TRACE
// One workgroup per probe; each invocation traces raysPerPixel rays, so a
// probe traces exactly what its tile's pixels would have.
shared vec3 probeSum[64][9];

void main()
{
    ivec2 probe = ivec2(gl_WorkGroupID.xy);
    uint lane = gl_LocalInvocationIndex;
    ivec2 extent = ivec2(PushConstants.control.xy);
    ivec2 pixel = probe_pixel(probe, extent);
    vec2 screenUV = (vec2(pixel) + 0.5) / vec2(extent);
    float depth = texture(prepassDepth, frame_uv(screenUV, prepassDepth)).r;
    // Uniform across the workgroup, so no invocation is left at the barrier.
    if (depth <= BackgroundDepth) {
        if (lane < 9u) {
            imageStore(probeRadiance, ivec2(probe.x * 9 + int(lane), probe.y), vec4(0.0));
        }
        return;
    }
    TraceOrigin o = trace_origin(screenUV, depth);
    vec3 helper = abs(o.normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(helper, o.normal));
    vec3 bitangent = cross(o.normal, tangent);
    uint state = uint(probe.x) * 1973u ^ uint(probe.y) * 9277u ^ lane * 26699u ^
        PushConstants.control.w * 3079u ^ 0x68bc21ebu;

    // Stratified uniform hemisphere: an 8 x (8 * rays) grid over (cos theta,
    // phi), one jittered sample per cell.  Uniform rather than cosine, so the
    // projection's weight stays finite at the horizon.
    int rays = clamp(int(PushConstants.quality.x), 1, 8);
    float coefficients[9];
    vec3 sum[9];
    for (int i = 0; i < 9; ++i) sum[i] = vec3(0.0);
    for (int k = 0; k < rays; ++k) {
        float u1 = (float(lane % 8u) + randomFloat(state)) / 8.0;
        float u2 = (float(lane / 8u + 8u * uint(k)) + randomFloat(state)) /
            float(8 * rays);
        float r = sqrt(max(1.0 - u1 * u1, 0.0));
        float phi = 2.0 * Pi * u2;
        vec3 direction = normalize(tangent * (r * cos(phi)) +
            bitangent * (r * sin(phi)) + o.normal * u1);
        bool hit, portal;
        int steps;
        vec3 incident = trace_incident(o, direction, hit, portal, steps);
        sh_basis(normalize(transpose(mat3(sceneData.view)) * direction), coefficients);
        for (int i = 0; i < 9; ++i) sum[i] += incident * coefficients[i];
    }
    for (int i = 0; i < 9; ++i) probeSum[lane][i] = sum[i];
    barrier();
    if (lane < 9u) {
        vec3 total = vec3(0.0);
        for (int j = 0; j < 64; ++j) total += probeSum[j][lane];
        // Uniform hemisphere pdf is 1 / 2pi.
        total *= 2.0 * Pi / float(64 * rays);
        imageStore(probeRadiance, ivec2(probe.x * 9 + int(lane), probe.y), vec4(total, 1.0));
    }
}
#else
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

    TraceOrigin o = trace_origin(screenUV, depth);
#ifdef LUMEN_LITE
    if (probes_enabled()) {
        // The four probes around this pixel, bilinear in tile space, each
        // kept only if it lies on this pixel's surface and faces its way.
        vec2 f = (vec2(pixel) + 0.5) / float(ProbeTile) - 0.5;
        ivec2 base = ivec2(floor(f));
        vec2 t = f - vec2(base);
        ivec2 probes = (extent + ProbeTile - 1) / ProbeTile;
        vec3 gathered = vec3(0.0);
        float weightSum = 0.0;
        for (int i = 0; i < 4; ++i) {
            ivec2 corner = ivec2(i & 1, i >> 1);
            ivec2 probe = clamp(base + corner, ivec2(0), probes - 1);
            if (imageLoad(probeRadiance, ivec2(probe.x * 9, probe.y)).a < 0.5) continue;
            ivec2 probePixel = probe_pixel(probe, extent);
            vec2 probeUV = (vec2(probePixel) + 0.5) / vec2(extent);
            TraceOrigin p = trace_origin(probeUV,
                texture(prepassDepth, frame_uv(probeUV, prepassDepth)).r);
            float plane = abs(dot(o.worldNormal, p.worldPosition - o.worldPosition));
            float facing = max(dot(o.worldNormal, p.worldNormal), 0.0);
            float bilinear = (corner.x == 1 ? t.x : 1.0 - t.x) *
                (corner.y == 1 ? t.y : 1.0 - t.y);
            // A probe more than a few centimetres off this pixel's plane
            // (scaled with distance, as depth precision is) is on another
            // surface and saw a different hemisphere.
            float planeWeight = exp(-plane / (0.02 * max(-o.position.z, 1.0)));
            float weight = bilinear * planeWeight * pow(facing, 8.0);
            gathered += probe_incident(probe, o.worldNormal) * weight;
            weightSum += weight;
        }
        if (weightSum > 1e-3) {
            imageStore(rawIndirectImage, pixel, vec4(gathered / weightSum, 1.0));
            imageStore(diagnosticImage, pixel, vec4(0.75, 0.0, 0.0, 0.0));
            return;
        }
    }
#endif
    uint state = uint(pixel.x) * 1973u ^ uint(pixel.y) * 9277u ^
        PushConstants.control.w * 26699u ^ 0x68bc21ebu;
    int stepCount = max(1, int(PushConstants.settings.w + 0.5));
    int rayCount = clamp(int(PushConstants.quality.x), 1, 8);
    vec3 indirect = vec3(0.0);
    int hitSamples = 0;
    int portalSamples = 0;
    int totalSteps = 0;
    for (int rayIndex = 0; rayIndex < rayCount; ++rayIndex) {
        vec3 direction = cosine_direction(o.normal, state);
        bool hit, portalTermination;
        int stepsTaken;
        indirect += trace_incident(o, direction, hit, portalTermination, stepsTaken);
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
#endif
