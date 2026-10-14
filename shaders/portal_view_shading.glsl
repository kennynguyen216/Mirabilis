// The shared body of the portal camera's forward pass, split for the same
// reason as mesh_shading.glsl: portal_view.frag and portal_view_mask.frag
// differ only by MIRABILIS_ALPHA_MASK.  Its material response comes from the
// same material_brdf.glsl as the main camera's, so the two views differ only
// in their outputs and ambient policy, never in the BRDF.
#define MIRABILIS_PORTAL_GI 1
#include "input_structures.glsl"
#include "alpha_mask.glsl"
#include "environment.glsl"
#include "material_brdf.glsl"
#include "portal_indirect.glsl"
#include "indirect_color.glsl"

#ifndef MIRABILIS_ALPHA_MASK
layout(early_fragment_tests) in;
#endif

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inWorldPosition;
layout(location = 4) in vec4 inTangent;
layout(location = 0) out vec4 outFragColor;

void main()
{
    vec4 baseColor = inColor * texture(colorTex, inUV);
#ifdef MIRABILIS_ALPHA_MASK
    // Before any shading work: a discarded fragment must not have cost a
    // lighting evaluation, and must not reach the G-buffer writes below.
    apply_alpha_mask(baseColor.a);
#endif
    if (materialData.materialFlags.y > 0.5) {
        const float cellsAcross = 25.0;
        float checker = mod(
            floor(inUV.x * cellsAcross) + floor(inUV.y * cellsAcross),
            2.0);
        vec3 checkerColor = mix(vec3(0.28), vec3(0.72), checker);

        vec2 cell = abs(fract(inUV * cellsAcross) - 0.5);
        float gridLine = step(0.46, max(cell.x, cell.y));
        baseColor.rgb *= mix(checkerColor, vec3(0.04, 0.10, 0.16), gridLine);
    }
    vec3 geometricNormal = normalize(inNormal);
    vec3 normal = material_shading_normal(geometricNormal, inTangent, inUV);
    MaterialSurface surface = material_surface(
        baseColor.rgb, inUV, geometricNormal, normal, inWorldPosition);
    float visibility = sunlight_visibility(inWorldPosition, geometricNormal);
    // Lumen portals gather scene lighting; plain SSGI keeps its legacy stand-in.
    bool lumen = sceneData.portalIndirectSettings.y == 1.0;
    // The main camera's AO coordinates do not describe this virtual camera.
    float occlusion = lumen ? 1.0 : ambient_occlusion(gl_FragCoord.xy);
    float substitute = lumen ? 0.0 : sceneData.portalIndirectSettings.x;
    float ambientScale = lumen ? 0.0 : 1.0 - substitute;
    vec3 ambientLight = sceneData.ambientColor.rgb * occlusion * ambientScale +
        substitute * environment_irradiance(normal) * occlusion;

    // The sun term is left out of occlusion: it already has its own visibility
    // test, and scaling it here would darken contact points in full sunlight.
    vec3 directDiffuse;
    vec3 directSpecular;
    material_sun(surface, visibility, directDiffuse, directSpecular);
    vec3 ambientDiffuse;
    vec3 ambientSpecular;
    if (sceneData.iblSettings.x > 0.5) {
        // The SSGI stand-in is the same irradiance, so it folds into the
        // diffuse scale and both cameras keep dividing the work alike.
        material_ambient_ibl(
            surface, occlusion, ambientScale + substitute,
            ambientDiffuse, ambientSpecular);
    } else {
        material_ambient(surface, ambientLight, ambientDiffuse, ambientSpecular);
    }
    if (!lumen && substitute > 0.0 && sceneData.environmentTint.w < 1.0) {
        vec3 standIn = (sceneData.iblSettings.x > 0.5
            ? material_ibl_diffuse_light(surface.normal) : environment_irradiance(normal)) * occlusion * substitute;
        ambientDiffuse += material_diffuse_albedo(surface) *
            (indirect_color(standIn, sceneData.environmentTint.w) - standIn);
    }

    vec3 emission = material_emission(geometricNormal, inWorldPosition);
    vec3 emitterDiffuse = vec3(0.0);
    vec3 emitterSpecular = vec3(0.0);
    if (lumen)
        material_emitters(surface, inWorldPosition, geometricNormal, gl_FragCoord.xy,
            emitterDiffuse, emitterSpecular);
    vec3 indirect = lumen ? material_diffuse_albedo(surface) *
        indirect_color(portal_indirect(inWorldPosition, geometricNormal), sceneData.environmentTint.w) * sceneData.portalIndirectSettings.z
        : vec3(0.0);

    outFragColor = vec4(
        directDiffuse + directSpecular + ambientDiffuse + ambientSpecular +
            emission + emitterDiffuse + emitterSpecular + indirect,
        1.0);
    vec3 debugColor;
    if (material_debug_color(
            surface, geometricNormal, inTangent,
            directDiffuse, directSpecular, emission, debugColor)) {
        outFragColor = vec4(debugColor, 1.0);
    }
}
