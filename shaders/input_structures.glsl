#include "scene_data.glsl"

#include "sun_shadow.glsl"

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

// Image-based lighting: the panorama prefiltered for GGX, level k for
// roughness k / iblSettings.y, and the split-sum (scale, bias) table.
layout(set = 0, binding = 4) uniform sampler2D prefilteredEnvironment;
layout(set = 0, binding = 5) uniform sampler2D brdfLut;
// Emitter light at the visible surface (R4.9, R4.12): the path tracer's own
// triangles, materials and emitter list for specular sampling, and the
// surface cache with its emitter page for the shadowed diffuse term.
// Placeholders are bound while emitter sampling is off
// (sceneData.emitterSettings.x == 0), and nothing reads them then.
#include "trace_scene.glsl"
layout(std430, set = 0, binding = 7) readonly buffer Triangles { Triangle triangles[]; };
layout(std430, set = 0, binding = 8) readonly buffer Materials { Material materials[]; };
layout(std430, set = 0, binding = 9) readonly buffer Emitters { uint emitters[]; };
layout(set = 0, binding = 6) uniform sampler2D cacheAlbedo;
layout(set = 0, binding = 10) uniform sampler2D cacheEmissive;
layout(set = 0, binding = 11) uniform sampler2D cacheDepth;
// Bound to the cache's emitter page, not its direct page: the lookup's
// `direct` is then emitter light alone, and sun and sky stay the forward
// pass's own.
layout(set = 0, binding = 12) uniform sampler2D cacheDirect;
#define SURFACE_CACHE_SET 0
#define SURFACE_CACHE_BINDING_CARDS 13
#define SURFACE_CACHE_BINDING_GRID 14
#define SURFACE_CACHE_BINDING_INDICES 15
#define SURFACE_CACHE_READ_INDIRECT(texel) vec3(0.0)
#include "surface_cache_lookup.glsl"

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
// Tangent-space normal map, stored linear.  Materials without one are bound a
// 1x1 flat normal, (0.5, 0.5, 1).
layout(set = 1, binding = 3) uniform sampler2D normalTex;
