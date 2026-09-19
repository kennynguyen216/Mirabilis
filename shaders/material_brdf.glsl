// The material response shared by the main forward pass and every portal
// camera, so the two views cannot disagree about a surface.  The sun term is
// the software path tracer's BRDF (path_trace_material.glsl) copied formula
// for formula, which keeps that renderer a meaningful reference.
//
// Light units: the raster sun colour behaves as irradiance, and the Lambert
// term this replaced had no 1/pi.  Each BRDF below is therefore multiplied by
// pi, so a rough dielectric keeps the brightness it had.
//
// Include after input_structures.glsl.

#include "environment.glsl"

const float MaterialPi = 3.14159265359;
// Roughness 0.045 squares to alpha 0.002025, the path tracer's floor.
const float MaterialMinimumRoughness = 0.045;
const float MaterialMinimumAlpha = 0.002025;

struct MaterialSurface {
    vec3 baseColor;
    float metallic;
    float roughness;
    // The normal used for lighting, and the view direction towards the camera.
    vec3 normal;
    vec3 view;
    float noV;
};

bool material_specular_enabled()
{
    return sceneData.materialSettings.w > 0.5;
}

// glTF packs roughness in green and metallic in blue.  With the textures
// toggled off the factors are used alone, which is all the path tracer has.
void material_factors(vec2 uv, out float metallic, out float roughness)
{
    // Sampled unconditionally: a texture read inside a branch loses the
    // screen-space derivatives its mip selection needs.
    vec4 texel = mix(
        vec4(1.0), texture(metalRoughTex, uv), sceneData.materialSettings.y);
    metallic = clamp(materialData.metal_rough_factors.x * texel.b, 0.0, 1.0);
    roughness = clamp(
        materialData.metal_rough_factors.y * texel.g,
        MaterialMinimumRoughness,
        1.0);
}

// Geometric specular anti-aliasing (Tokuyoshi and Kaplanyan): the lobe is
// widened by how fast the geometric normal turns across the pixel, so a
// highlight on curved or distant geometry cannot alias into shimmer.  It
// changes alpha in raster only, which is why "Match reference renderer" turns
// it off.
float material_antialiased_roughness(float roughness, vec3 geometricNormal)
{
    // Taken before the toggle is read, so derivatives stay well defined.
    vec3 dndu = dFdx(geometricNormal);
    vec3 dndv = dFdy(geometricNormal);
    if (sceneData.materialSettings.z < 0.5) {
        return roughness;
    }
    float variance = 0.25 * (dot(dndu, dndu) + dot(dndv, dndv));
    float kernel = min(2.0 * variance, 0.18);
    float alphaSquared = clamp(
        roughness * roughness * roughness * roughness + kernel,
        MaterialMinimumAlpha * MaterialMinimumAlpha,
        1.0);
    // alpha = roughness squared, so roughness is the fourth root of alpha^2.
    return sqrt(sqrt(alphaSquared));
}

// The normal a surface is lit with.  tangent.xyz is the interpolated
// world-space tangent and tangent.w its bitangent sign; w = 0 means the mesh
// has none.  Shadow lookups must keep using the geometric normal: a
// normal-mapped offset pushes samples inside the receiver and brings shadow
// acne back.
vec3 material_shading_normal(vec3 geometricNormal, vec4 tangent, vec2 uv)
{
    // Sampled unconditionally, like the metallic/roughness texture.
    vec3 texel = texture(normalTex, uv).xyz * 2.0 - 1.0;
    if (abs(tangent.w) < 0.001 || sceneData.materialSettings.x < 0.5) {
        return geometricNormal;
    }
    // Interpolation shortens the tangent and pulls it off the plane of the
    // interpolated normal, so it is rebuilt against that normal here.
    vec3 t = tangent.xyz - geometricNormal * dot(geometricNormal, tangent.xyz);
    if (dot(t, t) < 1e-12) {
        return geometricNormal;
    }
    t = normalize(t);
    // Sign rather than the raw w: interpolating across a mirrored UV seam
    // blends +1 and -1 toward 0.
    vec3 b = cross(geometricNormal, t) * (tangent.w < 0.0 ? -1.0 : 1.0);
    texel.xy *= materialData.materialFlags.x;
    vec3 mapped = mat3(t, b, geometricNormal) * texel;
    return dot(mapped, mapped) > 1e-12 ? normalize(mapped) : geometricNormal;
}

MaterialSurface material_surface(
    vec3 baseColor,
    vec2 uv,
    vec3 geometricNormal,
    vec3 normal,
    vec3 worldPosition)
{
    MaterialSurface surface;
    surface.baseColor = baseColor;
    material_factors(uv, surface.metallic, surface.roughness);
    surface.roughness = material_antialiased_roughness(
        surface.roughness, geometricNormal);
    surface.normal = normal;
    surface.view = normalize(sceneData.cameraPosition.xyz - worldPosition);
    // Clamped rather than rejected: a shading normal can face away from the
    // camera on a silhouette, and the specular term divides by this.
    surface.noV = max(dot(normal, surface.view), 1e-4);
    return surface;
}

float material_ggx_d(float noH, float alpha)
{
    float a2 = alpha * alpha;
    float denominator = noH * noH * (a2 - 1.0) + 1.0;
    return a2 / (MaterialPi * denominator * denominator);
}

// Separable Smith, as in the path tracer.  The height-correlated form is a
// little more accurate, but matching the reference is worth more.
float material_smith_g1(float noX, float alpha)
{
    return 2.0 * noX /
        (noX + sqrt(alpha * alpha + (1.0 - alpha * alpha) * noX * noX));
}

vec3 material_fresnel_schlick(vec3 f0, float voH)
{
    return f0 + (1.0 - f0) * pow(clamp(1.0 - voH, 0.0, 1.0), 5.0);
}

// Direct sunlight, split into the part that leaves a surface in every
// direction and the part that depends on where it is seen from.
// visibility is the shadow-map result.
void material_sun(
    MaterialSurface surface,
    float visibility,
    out vec3 diffuse,
    out vec3 specular)
{
    diffuse = vec3(0.0);
    specular = vec3(0.0);
    // y = 1 is the ambient-only tuning view.
    if (sceneData.screenSpaceSettings.y > 0.5) {
        return;
    }
    vec3 lightDirection = normalize(sceneData.sunlightDirection.xyz);
    float noL = dot(surface.normal, lightDirection);
    if (noL <= 0.0) {
        return;
    }
    vec3 sun = MaterialPi * sceneData.sunlightColor.rgb * noL * visibility;
    if (!material_specular_enabled()) {
        // Lambert alone: exactly the term this model replaced.
        diffuse = surface.baseColor / MaterialPi * sun;
        return;
    }

    vec3 halfSum = surface.view + lightDirection;
    vec3 halfVector = dot(halfSum, halfSum) > 1e-12
        ? normalize(halfSum)
        : surface.normal;
    float voH = max(dot(surface.view, halfVector), 0.0);
    float noH = max(dot(surface.normal, halfVector), 0.0);
    float alpha = max(surface.roughness * surface.roughness, MaterialMinimumAlpha);
    vec3 fresnel = material_fresnel_schlick(
        mix(vec3(0.04), surface.baseColor, surface.metallic), voH);
    diffuse = (1.0 - fresnel) * (1.0 - surface.metallic) *
        surface.baseColor / MaterialPi * sun;
    float distribution = material_ggx_d(noH, alpha) *
        material_smith_g1(surface.noV, alpha) *
        material_smith_g1(noL, alpha);
    specular = fresnel * distribution /
        max(4.0 * surface.noV * noL, 1e-4) * sun;
}

// Karis, "Physically Based Shading on Mobile" (2014): an analytic fit of the
// split-sum environment BRDF.  It lights a uniform surround, which is exactly
// what the flat ambient colour claims to be.
vec3 material_environment_brdf(vec3 f0, float roughness, float noV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, pow(2.0, -9.28 * noV)) * r.x + r.y;
    vec2 scaleBias = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * scaleBias.x + scaleBias.y;
}

// Splits ambient irradiance - already scaled by occlusion and by whatever
// SSGI policy the caller applies - into diffuse and specular reflection.
// Without the specular half a metal out of the sun would render black.
void material_ambient(
    MaterialSurface surface,
    vec3 irradiance,
    out vec3 diffuse,
    out vec3 specular)
{
    if (!material_specular_enabled()) {
        diffuse = irradiance * surface.baseColor;
        specular = vec3(0.0);
        return;
    }
    diffuse = irradiance * surface.baseColor * (1.0 - surface.metallic);
    specular = irradiance * material_environment_brdf(
        mix(vec3(0.04), surface.baseColor, surface.metallic),
        surface.roughness,
        surface.noV);
}

// Image-based lighting (docs/image_based_lighting_design.md).  Both lights
// below are radiance, the unit the flat ambient colour is in: a uniform
// surround of radiance L lights a white Lambert surface to L, so irradiance
// enters as E / pi.

// The environment policy's intensity and black override.  The sun ceiling was
// already applied when the harmonics and the prefiltered chain were built.
vec3 material_environment_scale(vec3 radiance)
{
    return sceneData.ssgiFallbackSettings.y > 0.5
        ? vec3(0.0)
        : sceneData.ssgiFallbackSettings.x * radiance;
}

vec3 material_ibl_diffuse_light(vec3 n)
{
    // The analytic gradient keeps its closed form, so SSGI misses, portal
    // substitutes and this term all see one sky.
    if (sceneData.indirectSettings.y < 0.5) {
        return environment_irradiance(n);
    }
    // The same basis, in the same order, as sh_basis() in vk_engine_ibl.cpp.
    vec3 irradiance =
        sceneData.environmentSH[0].rgb * 0.282095 +
        sceneData.environmentSH[1].rgb * (0.488603 * n.y) +
        sceneData.environmentSH[2].rgb * (0.488603 * n.z) +
        sceneData.environmentSH[3].rgb * (0.488603 * n.x) +
        sceneData.environmentSH[4].rgb * (1.092548 * n.x * n.y) +
        sceneData.environmentSH[5].rgb * (1.092548 * n.y * n.z) +
        sceneData.environmentSH[6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0)) +
        sceneData.environmentSH[7].rgb * (1.092548 * n.x * n.z) +
        sceneData.environmentSH[8].rgb * (0.546274 * (n.x * n.x - n.y * n.y));
    // Band-2 ringing can dip below zero opposite a very bright region.
    return material_environment_scale(max(irradiance, vec3(0.0)) / MaterialPi);
}

vec3 material_ibl_specular_light(vec3 direction, float roughness)
{
    if (sceneData.indirectSettings.y < 0.5) {
        return environment_radiance(direction);
    }
    return material_environment_scale(textureLod(
        prefilteredEnvironment,
        equirectangular_uv(direction),
        roughness * sceneData.iblSettings.y).rgb);
}

// Ambient light from the skybox.  diffuseScale carries the SSGI retention
// policy, which applies to diffuse only: SSGI supplies no specular, so the
// reflection is never scaled by it.
void material_ambient_ibl(
    MaterialSurface surface,
    float occlusion,
    float diffuseScale,
    out vec3 diffuse,
    out vec3 specular)
{
    vec3 diffuseLight =
        material_ibl_diffuse_light(surface.normal) * occlusion * diffuseScale;
    if (!material_specular_enabled()) {
        diffuse = diffuseLight * surface.baseColor;
        specular = vec3(0.0);
        return;
    }
    diffuse = diffuseLight * surface.baseColor * (1.0 - surface.metallic);
    vec3 reflected = reflect(-surface.view, surface.normal);
    vec2 scaleBias = texture(brdfLut, vec2(surface.noV, surface.roughness)).rg;
    vec3 f0 = mix(vec3(0.04), surface.baseColor, surface.metallic);
    specular = material_ibl_specular_light(reflected, surface.roughness) *
        (f0 * scaleBias.x + scaleBias.y) * occlusion;
}

// What diffuse indirect light is multiplied by.  A metal has no diffuse lobe,
// here or in the path tracer, so it must not receive any.
vec3 material_diffuse_albedo(MaterialSurface surface)
{
    return material_specular_enabled()
        ? surface.baseColor * (1.0 - surface.metallic)
        : surface.baseColor;
}

// Emitted radiance: added once, unaffected by lighting, shadow or occlusion,
// and leaving only the side the authored geometric normal points to.  Every
// pass rasterises with culling disabled -- walls and floors are single-sided
// geometry that must not vanish when seen from behind -- so the back face of
// an emissive surface still shades, and without this test it would glow as
// brightly as the front.  The geometric normal is used rather than the
// shading normal because sidedness is a property of the surface, not of its
// normal map, and rather than gl_FrontFacing because winding and authored
// normal can disagree while the path tracer follows the authored one.
vec3 material_emission(vec3 geometricNormal, vec3 worldPosition)
{
    vec3 towardViewer = sceneData.cameraPosition.xyz - worldPosition;
    return dot(geometricNormal, towardViewer) > 0.0
        ? materialData.emission.rgb
        : vec3(0.0);
}

// The forward-written debug views.  Values match RenderDebugView in
// src/vk_engine.h, which is what the C++ side writes into
// sceneData.materialDebug.x -- they were two apart, so "material roughness"
// and "material metallic" showed nothing and the other seven views each
// showed the one two places along.  tests/test_debug_view_modes.py fails if
// the two lists drift again.
const int MaterialDebugRoughness = 19;
const int MaterialDebugMetallic = 20;
const int MaterialDebugDirectDiffuse = 21;
const int MaterialDebugDirectSpecular = 22;
const int MaterialDebugEmission = 23;
const int MaterialDebugShadingNormal = 24;
const int MaterialDebugGeometricNormal = 25;
const int MaterialDebugTangent = 26;
const int MaterialDebugTangentHandedness = 27;

// True while one of those views is selected, with the colour it shows.  The
// lighting views stay in linear radiance, so a capture of "direct diffuse"
// plus "direct specular" is comparable with the path tracer's direct image.
bool material_debug_color(
    MaterialSurface surface,
    vec3 geometricNormal,
    vec4 tangent,
    vec3 directDiffuse,
    vec3 directSpecular,
    vec3 emission,
    out vec3 color)
{
    int mode = int(sceneData.materialDebug.x + 0.5);
    color = vec3(0.0);
    if (mode == MaterialDebugRoughness) {
        color = vec3(surface.roughness);
    } else if (mode == MaterialDebugMetallic) {
        color = vec3(surface.metallic);
    } else if (mode == MaterialDebugDirectDiffuse) {
        color = directDiffuse;
    } else if (mode == MaterialDebugDirectSpecular) {
        color = directSpecular;
    } else if (mode == MaterialDebugEmission) {
        color = emission;
    } else if (mode == MaterialDebugShadingNormal) {
        color = surface.normal * 0.5 + 0.5;
    } else if (mode == MaterialDebugGeometricNormal) {
        color = geometricNormal * 0.5 + 0.5;
    } else if (mode == MaterialDebugTangent) {
        color = dot(tangent.xyz, tangent.xyz) > 1e-12
            ? normalize(tangent.xyz) * 0.5 + 0.5
            : vec3(0.0);
    } else if (mode == MaterialDebugTangentHandedness) {
        // Green +1, red -1, grey where the mesh has no tangent.  A mirrored UV
        // island or a negatively scaled object should read red.
        color = abs(tangent.w) < 0.001
            ? vec3(0.25)
            : (tangent.w > 0.0 ? vec3(0.1, 0.9, 0.1) : vec3(0.9, 0.1, 0.1));
    } else {
        return false;
    }
    return true;
}
