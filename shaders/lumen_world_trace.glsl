// Shared Lumen world gather for the main camera and portal cameras.
// The caller supplies field/cache samplers and the two lighting policies.
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

// Hit lighting for a field hit no card captured (R4.61): the sun it reflects,
// in the cache's convention (albedo x sunColor x N.L x visibility), with
// visibility from the forward pass's own shadow map.  Its sky, emitters and
// bounce light are unknown here and stay 0.  Half of the living room's first
// sun bounce lands on stacked window bars one card layer cannot hold (R4.60).
vec3 lumen_hit_sun(vec3 p)
{
    if (!LUMEN_HIT_LIGHTING_ENABLED) {
        return vec3(0.0);
    }
    vec4 albedo = textureLod(albedoVolume,
        (p - fieldMin.xyz) / (fieldMax.xyz - fieldMin.xyz), 0.0);
    vec3 n = lumen_field_normal(p);
    float noL = dot(n, normalize(sceneData.sunlightDirection.xyz));
    if (albedo.a < 1e-3 || noL <= 0.0) {
        return vec3(0.0);
    }
    return albedo.rgb / albedo.a * sceneData.sunlightColor.rgb * noL *
        sunlight_visibility(p, n);
}

// Light arriving along one ray the screen could not answer: the surface the
// scene field hits, read from the surface cache, or the sky for a ray that
// leaves the scene.
vec3 lumen_trace(vec3 worldPosition, vec3 worldNormal, vec3 worldDirection,
    out uint rayClass)
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
            SurfaceCacheSample surface = LUMEN_SURFACE_LOOKUP(p, lumen_field_normal(p));
            if (surface.weight > 0.0) {
                rayClass = RayFieldCache;
                // Emitted light is sampled explicitly at the receiver while
                // emitterSettings.x > 0 (R4.9), so a hit adds only what the
                // surface reflects.
                return surface.albedo * (surface.direct + surface.indirect) +
                    (sceneData.emitterSettings.x > 0.5 ? vec3(0.0) : surface.emissive);
            }
            // A surface no card saw: only its sun is known (R4.61); guessing
            // its sky once lit sealed rooms (section 4.3).
            rayClass = RayFieldUncovered;
            return lumen_hit_sun(p);
        }
        t += max(d, 0.5 * voxel);
    }
    // Left the field: the sky.  Ran out of steps while still inside: no
    // estimate rather than the sky, which would let daylight into a sealed
    // room (the same rule the cache's radiosity follows).
    rayClass = t >= tExit ? RayFieldExit : RayFieldExhausted;
    return t >= tExit ? environment_radiance(worldDirection) : vec3(0.0);
}
