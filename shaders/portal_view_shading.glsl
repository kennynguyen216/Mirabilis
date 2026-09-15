// The shared body of the portal camera's forward pass, split for the same
// reason as mesh_shading.glsl: portal_view.frag and portal_view_mask.frag
// differ only by MIRABILIS_ALPHA_MASK.  Its material response comes from the
// same material_brdf.glsl as the main camera's, so the two views differ only
// in their outputs and ambient policy, never in the BRDF.
#include "input_structures.glsl"
#include "alpha_mask.glsl"
#include "environment.glsl"
#include "material_brdf.glsl"

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inWorldPosition;
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
    vec3 normal = normalize(inNormal);
    MaterialSurface surface = material_surface(
        baseColor.rgb, inUV, normal, inWorldPosition);
    // Ambient light is deliberately left unshadowed; without it an occluded
    // surface would be pure black rather than merely out of the sun.  What it
    // does get is ambient occlusion, which asks the different question of how
    // much of the surrounding hemisphere nearby geometry blocks.
    float visibility = sunlight_visibility(inWorldPosition, normal);
    float occlusion = ambient_occlusion(gl_FragCoord.xy);
    // A portal camera cannot run SSGI: every screen-space buffer in the frame
    // describes the room in front of the player, not the one through the
    // opening.  What it must not do is carry on spending the whole flat
    // ambient term while the main camera has handed most of that job to SSGI,
    // because then the same wall is lit two ways and changes colour the
    // instant the player steps through.  So this camera divides the work the
    // same way the main one does, and stands the missing screen-space
    // estimate up with the environment SSGI itself falls back to whenever a
    // ray leaves the depth buffer -- evaluated over the whole hemisphere at
    // once, since there are no rays here to average.
    //
    // It is an approximation and not portal SSGI: it carries no colour
    // bleeding, no local indirect shadowing, and no light from surfaces the
    // portal camera cannot see.  It only stops the two cameras disagreeing
    // about how much indirect light there is.
    float substitute = sceneData.portalIndirectSettings.x;
    float ambientScale = mix(
        1.0, clamp(sceneData.indirectSettings.x, 0.0, 1.0), substitute);
    vec3 ambientLight = sceneData.ambientColor.rgb * occlusion * ambientScale +
        substitute * environment_irradiance(normal) * occlusion;

    // The sun term is left out of occlusion: it already has its own visibility
    // test, and scaling it here would darken contact points in full sunlight.
    vec3 directDiffuse;
    vec3 directSpecular;
    material_sun(surface, visibility, directDiffuse, directSpecular);
    vec3 ambientDiffuse;
    vec3 ambientSpecular;
    material_ambient(surface, ambientLight, ambientDiffuse, ambientSpecular);

    outFragColor = vec4(
        directDiffuse + directSpecular + ambientDiffuse + ambientSpecular +
            material_emission(),
        1.0);
}
