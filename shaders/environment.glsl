// The one environment convention, shared by every camera that has to invent
// light for a direction no geometry occupies.  SSGI fills its ray misses from
// it; a portal camera, which has no screen-space buffers of its own to trace,
// reads it across the whole hemisphere instead.  The two have to agree: one
// room lit by two different skies is exactly the broad colour shift that
// appears the moment the player crosses a portal.
//
// Include this after a header that declares sceneData, and declare the
// panorama as `environmentTexture` before including it.  The image sits at a
// different set and binding in every pass that reads it, but the lookups
// below have to name it once.

const float EnvironmentPi = 3.14159265359;

// The analytic stand-in sky.  It is kept rather than deleted because the
// software path tracer still lights its misses with it, and a reference
// comparison only means something when both sides see the same environment.
const vec3 EnvironmentDownColor = vec3(0.7, 0.8, 1.0);
const vec3 EnvironmentUpColor = vec3(0.12, 0.3, 0.65);

vec2 equirectangular_uv(vec3 direction)
{
    float longitude =
        atan(direction.z, direction.x) / (2.0 * EnvironmentPi) + 0.5;
    float latitude = acos(clamp(direction.y, -1.0, 1.0)) / EnvironmentPi;
    return vec2(longitude, latitude);
}

// Everything about an environment sample except where the radiance came from.
// The panorama and the gradient are two sources under one policy, and it is
// the policy -- intensity, black override, sun ceiling -- that has to be
// identical between cameras.
vec3 apply_environment_policy(vec3 radiance)
{
    if (sceneData.ssgiFallbackSettings.y > 0.5) {
        return vec3(0.0);
    }
    // The sun disk is already delivered by the direct term, with a shadow map
    // deciding who receives it.  A sample that happens to point at it would
    // add the sun a second time and unshadowed, at a radiance four orders of
    // magnitude above the sky beside it -- which on screen is a white speckle
    // that survives both filters and that a high history weight then makes
    // permanent.  Clipping to where sky ends and sun begins removes that
    // double count.  Scaling by luminance rather than clipping each channel
    // separately keeps a bright sky from shifting hue as it is clipped.
    float luminance = dot(radiance, vec3(0.2126, 0.7152, 0.0722));
    if (luminance > sceneData.indirectSettings.w) {
        radiance *= sceneData.indirectSettings.w / luminance;
    }
    return sceneData.ssgiFallbackSettings.x * radiance;
}

// Radiance arriving from one direction outside the scene.  The equirectangular
// mapping is the one sky.comp uses, so what a ray that leaves the screen is
// filled with is the same sky the camera would have seen looking that way.
//
// The lookup is deliberately taken from a coarse mip.  One cosine-weighted
// sample stands for a wide cone, not for a texel of a 4K panorama, and a noon
// sky with a sun disk in it produces fireflies at level 0 that no amount of
// bilateral filtering removes.
vec3 environment_radiance(vec3 worldDirection)
{
    vec3 radiance = sceneData.indirectSettings.y > 0.5
        ? textureLod(
            environmentTexture,
            equirectangular_uv(worldDirection),
            sceneData.indirectSettings.z).rgb
        : mix(
            EnvironmentDownColor,
            EnvironmentUpColor,
            clamp(worldDirection.y * 0.5 + 0.5, 0.0, 1.0));
    return apply_environment_policy(radiance);
}

// What a surface whose rays all left the scene converges to: the whole
// hemisphere around its normal rather than one direction in it.  A camera with
// no screen-space buffers has no rays to average, so the average is evaluated
// directly.
//
// The gradient is linear in the direction's y, and the cosine-weighted mean of
// y over the hemisphere around n is two thirds of n.y, so its closed form is
// the same gradient read a third of the way along.  The panorama has no closed
// form and is instead read several mips coarser than a single ray would be,
// because the cone one sample stands for here is the entire hemisphere.
vec3 environment_irradiance(vec3 worldNormal)
{
    const float HemisphereLodBias = 3.0;
    vec3 radiance = sceneData.indirectSettings.y > 0.5
        ? textureLod(
            environmentTexture,
            equirectangular_uv(worldNormal),
            sceneData.indirectSettings.z + HemisphereLodBias).rgb
        : mix(
            EnvironmentDownColor,
            EnvironmentUpColor,
            clamp(worldNormal.y / 3.0 + 0.5, 0.0, 1.0));
    return apply_environment_policy(radiance);
}
