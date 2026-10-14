#ifndef MIRABILIS_EMITTER_SAMPLING_GLSL
#define MIRABILIS_EMITTER_SAMPLING_GLSL

// Explicit emitter sampling shared by the surface cache's direct page and the
// forward pass (docs/lumen_lite_design.md R4.7, R4.9, R4.12), so both
// estimate the same light the path tracer's sampleLights() does: a triangle
// chosen uniformly from the emitter list, a point uniform in its area
// (u = sqrt(xi1), v = xi2), 1/pdf = cos(light) * area * count / distance^2,
// and sidedness from authoredSideNormal().
//
// The includer declares the path tracer's buffers as `triangles`,
// `materials` and `emitters` (see trace_scene.glsl).  To use
// emitter_visibility() it also defines EMITTER_FIELD_VISIBILITY and
// `float emitter_field_distance(vec3 world)`, the scene distance field in
// world units, before including this.

// Endpoint clearance, in field voxels.  Both ends of a visibility segment
// leave their surface along that surface's own normal: the receiver along
// its normal, the emitter along its authored side.  Stopping short along the
// ray instead left a grazing segment inside the emitter's own field shell,
// which blocked half the wall samples in Cornell (R4.11).
const float EmitterClearanceVoxels = 2.5;

#ifdef EMITTER_FIELD_VISIBILITY
// Hard visibility along the whole segment through the scene field.
float emitter_visibility(vec3 origin, vec3 target, float voxel)
{
    vec3 delta = target - origin;
    float span = length(delta);
    if (span <= 1e-6) {
        return 1.0;
    }
    vec3 direction = delta / span;
    float t = 0.0;
    for (int step = 0; step < 96 && t < span; ++step) {
        float d = emitter_field_distance(origin + direction * t);
        if (d < 0.25 * voxel) {
            return 0.0;
        }
        t += max(d, 0.5 * voxel);
    }
    return 1.0;
}
#endif

uint emitter_hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Sample i of n: x chooses the triangle (golden-ratio sequence), yz place the
// point (Hammersley), all rotated by `rotation` so neighbours differ.
vec3 emitter_stratum(uint i, uint n, vec3 rotation)
{
    return fract(vec3(float(i) * 0.6180339887, (float(i) + 0.5) / float(n),
        float(bitfieldReverse(i)) * 2.3283064365386963e-10) + rotation);
}

vec3 emitter_rotation(uint seed)
{
    uint h = emitter_hash(seed);
    return vec3(float(h & 1023u), float((h >> 10) & 1023u),
        float((h >> 20) & 1023u)) / 1024.0;
}

// One sample of the estimator, unshadowed.  Returns the light arriving along
// `direction` as irradiance / pi per unit receiver cosine,
// Le * cos(light) * area * count / (pi * distance^2), or zero when the point
// is behind the receiver's `normal` or the emitter faces away.
// `visibilityTarget` is the sample point lifted off the emitter by the
// endpoint clearance, for emitter_visibility().
vec3 emitter_sample(vec3 world, vec3 normal, uint count, vec3 xi, float voxel,
    out vec3 direction, out vec3 visibilityTarget)
{
    direction = normal;
    visibilityTarget = world;
    Triangle emitter = triangles[emitters[min(uint(xi.x * float(count)), count - 1u)]];
    float u = sqrt(xi.y);
    float v = xi.z;
    vec3 lightPoint = emitter.p0.xyz * (1.0 - u) + emitter.p1.xyz * (u * (1.0 - v)) +
        emitter.p2.xyz * (u * v);
    vec3 delta = lightPoint - world;
    float distanceSquared = dot(delta, delta);
    if (distanceSquared <= 1e-10) {
        return vec3(0.0);
    }
    direction = delta * inversesqrt(distanceSquared);
    vec3 crossEdges = cross(emitter.p1.xyz - emitter.p0.xyz, emitter.p2.xyz - emitter.p0.xyz);
    float twiceArea = length(crossEdges);
    vec3 emitterNormal = authoredSideNormal(emitter, crossEdges / twiceArea);
    float cosineLight = max(dot(emitterNormal, -direction), 0.0);
    if (dot(normal, direction) <= 0.0 || cosineLight <= 1e-8) {
        return vec3(0.0);
    }
    visibilityTarget = lightPoint + emitterNormal * (EmitterClearanceVoxels * voxel);
    return materials[emitter.meta.x].emission.rgb * cosineLight * (0.5 * twiceArea) *
        float(count) / (3.14159265359 * distanceSquared);
}

#endif
