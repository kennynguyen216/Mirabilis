#include "scene_data.glsl"

// The sunlight depth map. Unlike the main camera this uses conventional
// depth (near = 0, far = 1), so the comparison below is LESS_OR_EQUAL and
// the sampler's white border leaves everything outside the map lit.
layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;

// Screen-space ambient occlusion for this camera, at half resolution and
// sampled linearly.  It is bound for every camera in the frame, but only the
// one it was computed for is allowed to read it.
layout(set = 0, binding = 2) uniform sampler2D ambientOcclusionTex;

// The equirectangular environment panorama, at every mip.  It sits beside the
// scene data rather than in a pass-specific set because more than the SSGI
// trace needs it now: a portal camera has no screen-space pass to fall back
// from, so its forward shader reads the same sky directly.  Declared with the
// name environment.glsl expects.
layout(set = 0, binding = 3) uniform sampler2D environmentTexture;

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

// The same slots as GLTFMetallic_Roughness::MaterialConstants in vk_engine.h.
layout(set = 1, binding = 0) uniform GLTFMaterialData {
    vec4 colorFactors;
    // x metallic, y roughness.  z and w are always 0.
    vec4 metal_rough_factors;
    // xy = UV tiling, zw = UV offset. Legacy materials leave this zero and
    // the vertex shaders interpret that as a 1x1 scale.
    vec4 uvTransform;
    // x = glTF alphaCutoff.  Left at zero on every material that is not
    // alpha-masked, which makes the test a no-op there instead of something
    // the shared shader body has to branch around.
    vec4 alphaMask;
    // rgb = emissive factor times strength, w reserved.
    vec4 emission;
    // x = normal-map scale, y = debug checkerboard, zw reserved.
    vec4 materialFlags;
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
    // Respect the value shown in the UI. A tiny nonzero floor avoids exact
    // coplanar comparisons without silently replacing the authored bias.
    float normalBias = max(sceneData.shadowSettings.y, 0.001);
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
    float depthBias = max(sceneData.shadowSettings.x, 0.00002);
    float reference = projected.z - depthBias * mix(1.0, 2.5, grazing);
    float texel = sceneData.shadowSettings.z;

    // A deterministic 7x7 tent kernel produces a continuous transition with
    // no per-pixel random rotation. The previous rotated Poisson pattern was
    // visible as fine stripes on large, flat surfaces. Each comparison is
    // also bilinear on depth formats that support linear compare filtering.
    float filterRadius = max(sceneData.shadowFilterSettings.x, 0.0);
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
