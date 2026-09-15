// The shared body of the main forward pass.  mesh.frag and mesh_mask.frag are
// both two lines long and differ only in whether they define
// MIRABILIS_ALPHA_MASK before including this, so the alpha-masked variant
// cannot drift away from the opaque one as the shading model grows.
#include "input_structures.glsl"
#include "alpha_mask.glsl"
#include "material_brdf.glsl"

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inWorldPosition;
layout(location = 4) in vec4 inCurrentClip;
layout(location = 5) in vec4 inPreviousClip;
layout(location = 6) in vec4 inTangent;
layout(location = 0) out vec4 outFragColor;
layout(location = 1) out vec4 outAlbedo;
layout(location = 2) out vec4 outVelocity;
layout(location = 3) out vec4 outDirectLighting;

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
    // Ambient light is deliberately left unshadowed; without it an occluded
    // surface would be pure black rather than merely out of the sun.  What it
    // does get is ambient occlusion, which asks the different question of how
    // much of the surrounding hemisphere nearby geometry blocks.
    float visibility = sunlight_visibility(inWorldPosition, geometricNormal);
    float occlusion = ambient_occlusion(gl_FragCoord.xy);
    // Once SSGI is composited, the flat ambient stand-in risks counting the
    // same indirect light a second time, because a ray that leaves the depth
    // buffer is filled from the same sky this term stands in for.  How much
    // of it survives is a policy rather than a constant: replacing ambient
    // outright is only honest while misses carry real environment radiance,
    // and an enclosed scene whose rays mostly hit unlit stone has nothing to
    // put in its place.  Portal cameras clear the SSGI flag and keep the
    // whole term until they receive their own screen-space pass.
    float ambientScale = sceneData.screenSpaceSettings.z > 0.5
        ? clamp(sceneData.indirectSettings.x, 0.0, 1.0)
        : 1.0;
    vec3 ambientLight = sceneData.ambientColor.rgb * occlusion * ambientScale;

    // The sun term is left out of occlusion: it already has its own visibility
    // test, and scaling it here would darken contact points in full sunlight.
    vec3 directDiffuse;
    vec3 directSpecular;
    material_sun(surface, visibility, directDiffuse, directSpecular);
    // SSGI supplies diffuse indirect light only, so the specular half of the
    // ambient term is never counted twice.
    vec3 ambientDiffuse;
    vec3 ambientSpecular;
    if (sceneData.iblSettings.x > 0.5) {
        material_ambient_ibl(
            surface, occlusion, ambientScale, ambientDiffuse, ambientSpecular);
    } else {
        material_ambient(surface, ambientLight, ambientDiffuse, ambientSpecular);
    }
    vec3 emission = material_emission();

    outFragColor = vec4(
        directDiffuse + directSpecular + ambientDiffuse + ambientSpecular +
            emission,
        1.0);
    vec3 debugColor;
    if (material_debug_color(
            surface, geometricNormal, inTangent,
            directDiffuse, directSpecular, emission, debugColor)) {
        outFragColor = vec4(debugColor, 1.0);
    }
    // The SSGI composite multiplies filtered incident light by this.
    outAlbedo = vec4(material_diffuse_albedo(surface), 1.0);
    vec2 currentUV = inCurrentClip.xy / max(inCurrentClip.w, 0.00001) * 0.5 + 0.5;
    vec2 previousUV = inPreviousClip.xy / max(inPreviousClip.w, 0.00001) * 0.5 + 0.5;
    // Add this displacement to a current UV to find the same point in the
    // previous frame.
    outVelocity = vec4(previousUV - currentUV, 0.0, 1.0);
    // SSGI reads this as the radiance leaving a surface towards other
    // surfaces.  Specular depends on the direction it leaves in, and only the
    // camera's was evaluated, so it stays out; emission leaves in every
    // direction and belongs here.
    outDirectLighting = vec4(directDiffuse + emission, 1.0);
}
