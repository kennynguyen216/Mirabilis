#include "scene_data.glsl"

// The sunlight depth map. Unlike the main camera this uses conventional
// depth (near = 0, far = 1), so the comparison below is LESS_OR_EQUAL and
// the sampler's white border leaves everything outside the map lit.
layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;

// Screen-space ambient occlusion for this camera, at half resolution and
// sampled linearly.  It is bound for every camera in the frame, but only the
// one it was computed for is allowed to read it.
layout(set = 0, binding = 2) uniform sampler2D ambientOcclusionTex;

// How much of the surrounding hemisphere reaches this surface: 1 fully open,
// 0 fully enclosed.  Only the ambient term should be scaled by it.  Direct
// sunlight already has its own visibility test in the shadow map, and
// scaling it twice would darken contact points that are in full sun.
//
// Takes the fragment coordinate as an argument rather than reading
// gl_FragCoord, because this header is included by vertex shaders too.
float ambient_occlusion(vec2 fragCoord)
{
    // Zero for portal cameras: they are bound the main camera's occlusion,
    // which describes geometry that is not in front of them.  Reusing it
    // would stamp the main view's contact shadows onto the portal image.
    if (sceneData.screenSpaceSettings.x < 0.5) {
        return 1.0;
    }
    // Clamped so the bilinear tap at the last rendered pixel cannot reach a
    // quarter of a texel past the live region into whatever the previous
    // render scale left there.
    vec2 uv = min(
        fragCoord * sceneData.ambientOcclusionUV.xy,
        sceneData.ambientOcclusionUV.zw);
    return texture(ambientOcclusionTex, uv).r;
}

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
    // Scene files can tune above these values, but older maps often contain
    // biases that are too small for their long, non-uniformly scaled walls.
    // Enforce a safe engine-wide floor so switching maps cannot bring the
    // striped self-shadow pattern back.
    float normalBias = max(sceneData.shadowSettings.y, 0.15);
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
    vec3 lightDirection = normalize(sceneData.sunlightDirection.xyz);
    float grazing = 1.0 - abs(dot(normalize(normal), lightDirection));
    float depthBias = max(sceneData.shadowSettings.x, 0.0012);
    float reference = projected.z - depthBias * mix(1.0, 2.5, grazing);
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
