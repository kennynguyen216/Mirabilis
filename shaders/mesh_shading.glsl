// The shared body of the main forward pass.  mesh.frag and mesh_mask.frag are
// both two lines long and differ only in whether they define
// MIRABILIS_ALPHA_MASK before including this, so the alpha-masked variant
// cannot drift away from the opaque one as the shading model grows.
#include "input_structures.glsl"
#include "alpha_mask.glsl"

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inWorldPosition;
layout(location = 4) in vec4 inCurrentClip;
layout(location = 5) in vec4 inPreviousClip;
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
    if (materialData.metal_rough_factors.z > 0.5) {
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
    vec3 lightDirection = normalize(sceneData.sunlightDirection.xyz);
    float diffuse = max(dot(normal, lightDirection), 0.0);
    // Ambient light is deliberately left unshadowed; without it an occluded
    // surface would be pure black rather than merely out of the sun.  What it
    // does get is ambient occlusion, which asks the different question of how
    // much of the surrounding hemisphere nearby geometry blocks.
    float visibility = sunlight_visibility(inWorldPosition, normal);
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
    vec3 ambient = sceneData.ambientColor.rgb * occlusion * ambientScale;
    // The sun term is left alone: it already has its own visibility test, and
    // scaling it here would darken contact points standing in full sunlight.
    vec3 direct = visibility * diffuse * sceneData.sunlightColor.rgb;
    if (sceneData.screenSpaceSettings.y > 0.5) {
        direct = vec3(0.0);
    }
    vec3 lighting = ambient + direct;

    outFragColor = vec4(baseColor.rgb * lighting, 1.0);
    outAlbedo = vec4(baseColor.rgb, 1.0);
    vec2 currentUV = inCurrentClip.xy / max(inCurrentClip.w, 0.00001) * 0.5 + 0.5;
    vec2 previousUV = inPreviousClip.xy / max(inPreviousClip.w, 0.00001) * 0.5 + 0.5;
    // Add this displacement to a current UV to find the same point in the
    // previous frame.
    outVelocity = vec4(previousUV - currentUV, 0.0, 1.0);
    outDirectLighting = vec4(baseColor.rgb * direct, 1.0);
}
