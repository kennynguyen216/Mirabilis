#ifndef MIRABILIS_ENVIRONMENT_GRADIENT_GLSL
#define MIRABILIS_ENVIRONMENT_GRADIENT_GLSL

// The analytic stand-in sky.  It is kept rather than deleted because the
// software path tracer still lights its misses with it, and a reference
// comparison only means something when both sides see the same environment.
// Shared by environment.glsl and the surface cache's direct lighting, which
// has no scene uniform block to include that file with.
const vec3 EnvironmentDownColor = vec3(0.7, 0.8, 1.0);
const vec3 EnvironmentUpColor = vec3(0.12, 0.3, 0.65);

// The panorama's mapping, the one sky.comp draws the background with.
vec2 equirectangular_uv(vec3 direction)
{
    const float Pi = 3.14159265359;
    float longitude = atan(direction.z, direction.x) / (2.0 * Pi) + 0.5;
    float latitude = acos(clamp(direction.y, -1.0, 1.0)) / Pi;
    return vec2(longitude, latitude);
}

vec3 gradient_radiance(vec3 direction)
{
    return mix(
        EnvironmentDownColor,
        EnvironmentUpColor,
        clamp(direction.y * 0.5 + 0.5, 0.0, 1.0));
}

// The cosine-weighted mean over the hemisphere around n (irradiance / pi):
// the gradient is linear in y and the cosine-weighted mean of y is two thirds
// of n.y, so it is the same gradient read a third of the way along.
vec3 gradient_irradiance(vec3 normal)
{
    return mix(
        EnvironmentDownColor,
        EnvironmentUpColor,
        clamp(normal.y / 3.0 + 0.5, 0.0, 1.0));
}

#endif
