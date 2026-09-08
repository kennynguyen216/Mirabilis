#include "scene_data.glsl"

// The sunlight depth map. Unlike the main camera this uses conventional
// depth (near = 0, far = 1), so the comparison below is LESS_OR_EQUAL and
// the sampler's white border leaves everything outside the map lit.
layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;

layout(set = 1, binding = 0) uniform GLTFMaterialData {
    vec4 colorFactors;
    vec4 metal_rough_factors;
    // xy = UV tiling, zw = UV offset. Legacy materials leave this zero and
    // the vertex shaders interpret that as a 1x1 scale.
    vec4 uvTransform;
} materialData;

layout(set = 1, binding = 1) uniform sampler2D colorTex;
layout(set = 1, binding = 2) uniform sampler2D metalRoughTex;

// Returns how much of the sunlight reaches this surface: 1 fully lit, 0 fully
// blocked.  Only the sunlight term should be scaled by it; ambient light is
// what keeps a shadowed surface from going black.
float sunlight_visibility(vec3 worldPosition, vec3 normal)
{
    if (sceneData.shadowSettings.w < 0.5) {
        return 1.0;
    }

    // Pushing the sample point along the normal before projecting removes
    // most self-shadowing acne on surfaces that face the sun edge-on, and it
    // does far less to detach contact shadows than depth bias alone.
    vec4 lightClip = sceneData.sunViewProjection *
        vec4(worldPosition + normal * sceneData.shadowSettings.y, 1.0);
    vec3 projected = lightClip.xyz / lightClip.w;
    // Beyond the shadow camera's depth range there is nothing recorded to
    // compare against, so treat the surface as lit rather than shadowed.
    if (projected.z < 0.0 || projected.z > 1.0) {
        return 1.0;
    }

    vec2 shadowUV = projected.xy * 0.5 + 0.5;
    float reference = projected.z - sceneData.shadowSettings.x;
    float texel = sceneData.shadowSettings.z;

    // Percentage-closer filtering: nine comparisons instead of one turn the
    // hard per-texel edge into a short gradient. On formats that support it,
    // each tap is also bilinear because the sampler compares before filtering.
    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            visibility += texture(
                shadowMap,
                vec3(shadowUV + vec2(x, y) * texel, reference));
        }
    }
    return visibility * (1.0 / 9.0);
}
