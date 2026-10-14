// Portal cameras gather the same scene lighting as Lumen's world fallback.
// No descriptor here refers to the main camera's screen-space buffers.
const uint RayFieldCache = 1u << 6;
const uint RayFieldUncovered = 1u << 7;
const uint RayFieldExit = 1u << 8;
const uint RayFieldExhausted = 1u << 9;
#define LUMEN_HIT_LIGHTING_ENABLED true
#define LUMEN_SURFACE_LOOKUP(p, n) surface_cache_lookup(p, n, false, true, true)
#include "lumen_world_trace.glsl"

vec3 portal_indirect(vec3 world, vec3 normal)
{
    SurfaceCacheSample sky = surface_cache_lookup(world, normal, true, true, true);
    if (sceneData.portalIndirectSettings.w > 0.5 && sky.weight > 0.0) {
        SurfaceCacheSample lighting = surface_cache_lookup(world, normal, false, true, true, 3);
        return sky.sky + lighting.indirect;
    }
    vec3 helper = abs(normal.z) < 0.999 ? vec3(0, 0, 1) : vec3(1, 0, 0);
    vec3 tangent = normalize(cross(helper, normal));
    vec3 bitangent = cross(normal, tangent);
    uvec3 cell = floatBitsToUint(floor(world * 100.0));
    vec3 rotation = emitter_rotation(cell.x * 1973u ^ cell.y * 9277u ^ cell.z * 26699u);
    vec3 incident = vec3(0.0);
    // ponytail: eight fixed spatial samples, add per-camera accumulation if
    // measured portal noise cannot pass the R6 quality and movement gates.
    for (uint i = 0u; i < 8u; ++i) {
        float u = (float(i) + rotation.x) / 8.0;
        float phi = 6.28318530718 * fract(float(bitfieldReverse(i)) *
            (1.0 / 4294967296.0) + rotation.y);
        vec3 direction = tangent * (sqrt(u) * cos(phi)) +
            bitangent * (sqrt(u) * sin(phi)) + normal * sqrt(1.0 - u);
        uint rayClass;
        vec3 sampleLight = lumen_trace(world, normal, direction, rayClass);
        if (sky.weight <= 0.0 || rayClass != RayFieldExit)
            incident += sampleLight;
    }
    return incident / 8.0 + sky.sky;
}
