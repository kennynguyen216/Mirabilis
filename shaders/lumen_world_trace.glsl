#ifdef MIRABILIS_RT_SHADOWS
#include "ray_scene_hit.glsl"
#include "ray_scene_lighting.glsl"
#endif
#include "dynamic_sdf.glsl"
#include "punctual_lights.glsl"
#ifndef LUMEN_HARDWARE_WORLD
#define LUMEN_HARDWARE_WORLD (sceneData.giTracingSettings.x>0.5)
#endif
// Shared Lumen world gather for the main camera and portal cameras.
// The caller supplies field/cache samplers and the two lighting policies.
float lumen_field_distance(vec3 world)
{
    return dynamic_field_union(world,bounded_field_distance(sceneField,world,fieldMin.xyz,fieldMax.xyz));
}

#define ORIGIN_FIELD_DISTANCE(p) lumen_field_distance(p)
#include "sdf_origin.glsl"
#undef ORIGIN_FIELD_DISTANCE

#define DYNAMIC_FIELD_DISTANCE(p) lumen_field_distance(p)
#include "dynamic_hit_lighting.glsl"
#undef DYNAMIC_FIELD_DISTANCE

vec3 lumen_field_normal(vec3 p)
{
    float h = dynamic_field_voxel(p,lumen_field_distance(p),fieldMin.w);
    vec3 gradient = vec3(
        lumen_field_distance(p + vec3(h, 0, 0)) - lumen_field_distance(p - vec3(h, 0, 0)),
        lumen_field_distance(p + vec3(0, h, 0)) - lumen_field_distance(p - vec3(0, h, 0)),
        lumen_field_distance(p + vec3(0, 0, h)) - lumen_field_distance(p - vec3(0, 0, h)));
    return normalize(gradient + vec3(1e-8));
}

// Hit lighting for a field hit no card captured (R4.61): the sun it reflects,
// in the cache's convention (albedo x sunColor x N.L x visibility), with
// visibility from the forward pass's own shadow map.  Its sky, emitters and
// bounce light are unknown here and stay 0.  Half of the living room's first
// sun bounce lands on stacked window bars one card layer cannot hold (R4.60).
vec3 lumen_hit_sun(vec3 p,vec3 n)
{
    if (!LUMEN_HIT_LIGHTING_ENABLED || (uint(sceneData.materialDebug.z)&1u)==0u) {
        return vec3(0.0);
    }
    vec4 albedo = textureLod(albedoVolume,
        (p - fieldMin.xyz) / (fieldMax.xyz - fieldMin.xyz), 0.0);
    float noL = dot(n, normalize(sceneData.sunlightDirection.xyz));
    if (albedo.a < 1e-3 || noL <= 0.0) {
        return vec3(0.0);
    }
    return albedo.rgb / albedo.a * sceneData.sunlightColor.rgb * noL *
        sunlight_visibility(p, n);
}

vec3 lumen_cached_radiance(SurfaceCacheSample surface)
{
    return surface.albedo*(surface.direct+surface.indirect)+
        (sceneData.emitterSettings.x > 0.5 ? vec3(0.0) : surface.emissive);
}

vec3 lumen_hit_radiance(vec3 p,vec3 normal, out uint rayClass)
{
    float distance=lumen_field_distance(p);
    if(dynamic_field_candidate(p,distance) && dynamic_field_distance(p)<=distance+1e-5) {
        rayClass=RayFieldUncovered;
        return dynamic_hit_radiance(p,normal,dynamicBounds.minimum.w);
    }
    SurfaceCacheSample surface; surface.weight=0.0;
    if (sceneData.giTracingSettings.y<0.5) surface=LUMEN_SURFACE_LOOKUP(p, normal);
    if (surface.weight > 0.0) {
        rayClass = RayFieldCache;
        return lumen_cached_radiance(surface);
    }
    rayClass = RayFieldUncovered;
    return lumen_hit_sun(p,normal);
}

// Light arriving along one world ray: hardware geometry when available,
// otherwise the scene field. Both read the same cache at the hit.
vec3 lumen_trace(vec3 worldPosition, vec3 worldNormal, vec3 worldDirection,
    out uint rayClass)
{
#ifdef MIRABILIS_RT_SHADOWS
    if (LUMEN_HARDWARE_WORLD) {
    // Reuse the opaque scene TLAS already bound for sunlight. The field's
    // multi-voxel origin offset moves rays almost a metre off the wall in a
    // large scene, and its voxel shells imprint false floor-contact bands.
    float magnitude = max(max(abs(worldPosition.x), abs(worldPosition.y)), abs(worldPosition.z));
    float epsilon = max(0.0002, magnitude * 0.000002);
    vec3 origin = worldPosition + worldNormal * epsilon;
    rayQueryEXT query;
    rayQueryInitializeEXT(query, sunCasters, gl_RayFlagsOpaqueEXT, 0xFF,
        origin, 0.0001, worldDirection, 1.0e6);
    while (rayQueryProceedEXT(query)) {
    }
    if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
        vec3 p = origin + worldDirection * rayQueryGetIntersectionTEXT(query, true);
        vec3 normal=ray_hit_normal(query);
        bool front=dot(normal,worldDirection)<0;
        if(!front) normal=-normal;
        RayInstance instance=rayInstances[ray_hit_index(query)];
        SurfaceCacheSample surface;surface.weight=0;
        if(instance.geometry.z!=0u && sceneData.giTracingSettings.y<0.5) surface=LUMEN_SURFACE_LOOKUP(p,normal);
        if(surface.weight>0) { rayClass=RayFieldCache; return lumen_cached_radiance(surface); }
        rayClass=RayFieldUncovered;
        if((uint(sceneData.materialDebug.z)&9u)==0u) return vec3(0);
        vec3 albedo=ray_hit_albedo(query);
        vec3 value=albedo*ray_uncached_direct(p,normal);
        if(instance.geometry.w!=0u) value=lumen_hit_sun(p,normal);
        if(front && (instance.geometry.z==0u || sceneData.emitterSettings.x<0.5)) value+=instance.emission.rgb;
        return value;
    }
    rayClass = RayFieldExit;
    return environment_radiance(worldDirection);
    }
#endif
    float voxel = dynamic_field_voxel(worldPosition,lumen_field_distance(worldPosition),fieldMin.w);
    vec3 origin = field_surface_origin(worldPosition,worldNormal,voxel);
    vec3 inverse = 1.0 / worldDirection;
    vec3 t0 = (field_bounds_min(fieldMin.xyz) - origin) * inverse;
    vec3 t1 = (field_bounds_max(fieldMax.xyz) - origin) * inverse;
    float tExit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    float t = 0.0;
    for (int step = 0; step < 192 && t < tExit; ++step) {
        float d = lumen_field_distance(origin + worldDirection * t);
        if (d < 0.25 * voxel) {
            vec3 p = origin + worldDirection * t;
            return lumen_hit_radiance(p,lumen_field_normal(p),rayClass);
        }
        t += max(d, field_minimum_step(voxel));
    }
    // Left the field: the sky.  Ran out of steps while still inside: no
    // estimate rather than the sky, which would let daylight into a sealed
    // room (the same rule the cache's radiosity follows).
    rayClass = t >= tExit ? RayFieldExit : RayFieldExhausted;
    return t >= tExit ? environment_radiance(worldDirection) : vec3(0.0);
}
