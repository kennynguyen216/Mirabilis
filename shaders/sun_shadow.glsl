#ifndef MIRABILIS_SUN_SHADOW_GLSL
#define MIRABILIS_SUN_SHADOW_GLSL

// The sun's shadow map and its visibility test, shared by the forward pass and
// the SSGI trace's hit lighting (R4.61), so both see one shadow.  Needs
// scene_data.glsl first.

// The sunlight depth map. Unlike the main camera this uses conventional
// depth (near = 0, far = 1), so the comparison below is LESS_OR_EQUAL and
// the sampler's white border leaves everything outside the map lit.
layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;

#ifdef MIRABILIS_RT_SHADOWS
// IQ2 (docs/lumen_lite_design.md): the *.rt.spv builds, loaded only on a
// ray-query device, trace the trace scene's opaque casters exactly.  The
// shadow map then holds only the casters that scene leaves out (alpha-masked
// draws, the player's body), so each caster is tested once.
#extension GL_EXT_ray_query : require
layout(set = 0, binding = 16) uniform accelerationStructureEXT sunCasters;

bool sun_ray_blocked(vec3 worldPosition, vec3 normal)
{
    // R4.11's endpoint rule, with the CPU sun-visibility check's epsilon.
    vec3 magnitude = abs(worldPosition);
    float epsilon = max(0.0002,
        max(max(magnitude.x, magnitude.y), magnitude.z) * 0.000002);
    rayQueryEXT query;
    rayQueryInitializeEXT(query, sunCasters,
        gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFF,
        worldPosition + normalize(normal) * epsilon, 0.0,
        normalize(sceneData.sunlightDirection.xyz), 1.0e6);
    while (rayQueryProceedEXT(query)) {
    }
    return rayQueryGetIntersectionTypeEXT(query, true) !=
        gl_RayQueryCommittedIntersectionNoneEXT;
}
#endif

// Returns how much of the sunlight reaches this surface: 1 fully lit, 0 fully
// blocked.  Only the sunlight term should be scaled by it; ambient light is
// what keeps a shadowed surface from going black.
float sunlight_visibility(vec3 worldPosition, vec3 normal)
{
    if (sceneData.shadowSettings.w < 0.5) {
        return 1.0;
    }
#ifdef MIRABILIS_RT_SHADOWS
    if (sun_ray_blocked(worldPosition, normal)) {
        return 0.0;
    }
#endif

    // Pushing the sample point along the normal before projecting removes
    // most self-shadowing acne on surfaces that face the sun edge-on, and it
    // does far less to detach contact shadows than depth bias alone.
    // One shadow texel at normal incidence, at most three for grazing faces.
    vec3 lightDirection = normalize(sceneData.sunlightDirection.xyz);
    // IQ1 diagnostic bits (0 in normal use): 1 single tap, 2 no normal
    // offset, 4 no depth bias.
    uint diagnostic = uint(sceneData.shadowFilterSettings.z + 0.5);
    float normalBias = max(sceneData.shadowSettings.y, 0.001);
    if (sceneData.shadowFilterSettings.y < 0.5) {
        normalBias /= max(abs(dot(normalize(normal), lightDirection)), 1.0 / 3.0);
    }
    if ((diagnostic & 2u) != 0u) {
        normalBias = 0.0;
    }
    vec4 lightClip = sceneData.sunViewProjection *
        vec4(worldPosition + normal * normalBias, 1.0);
    vec3 projected = lightClip.xyz / lightClip.w;
    // Beyond the shadow camera's depth range there is nothing recorded to
    // compare against, so treat the surface as lit rather than shadowed.
    if (projected.z < 0.0 || projected.z > 1.0) {
        return 1.0;
    }

    vec2 shadowUV = projected.xy * 0.5 + 0.5;
    // Surfaces facing across the light direction need more receiver bias than
    // ones facing it head-on. This slope-aware term removes the regular
    // columns caused by tiny depth changes across large grazing-angle faces.
    float grazing = 1.0 - abs(dot(normalize(normal), lightDirection));
    float depthBias = max(sceneData.shadowSettings.x, 0.00002);
    float reference = projected.z -
        ((diagnostic & 4u) != 0u ? 0.0 : depthBias * mix(1.0, 2.5, grazing));
    float texel = sceneData.shadowSettings.z;

    // A deterministic 7x7 tent kernel produces a continuous transition with
    // no per-pixel random rotation. The previous rotated Poisson pattern was
    // visible as fine stripes on large, flat surfaces. Each comparison is
    // also bilinear on depth formats that support linear compare filtering.
    float filterRadius = (diagnostic & 1u) != 0u
        ? 0.0 : max(sceneData.shadowFilterSettings.x, 0.0);
    if (filterRadius < 0.01) {
        return texture(shadowMap, vec3(shadowUV, reference));
    }
    float visibility = 0.0;
    float totalWeight = 0.0;
    float tapSpacing = filterRadius * texel / 3.0;
    for (int y = -3; y <= 3; ++y) {
        float weightY = 4.0 - abs(float(y));
        for (int x = -3; x <= 3; ++x) {
            float weightX = 4.0 - abs(float(x));
            float weight = weightX * weightY;
            vec2 offset = vec2(x, y) * tapSpacing;
            visibility += weight * texture(
                shadowMap, vec3(shadowUV + offset, reference));
            totalWeight += weight;
        }
    }
    return visibility / totalWeight;
}

#endif
