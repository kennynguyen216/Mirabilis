// The shared body of the portal camera's forward pass, split for the same
// reason as mesh_shading.glsl: portal_view.frag and portal_view_mask.frag
// differ only by MIRABILIS_ALPHA_MASK.
#include "input_structures.glsl"
#include "alpha_mask.glsl"

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
    vec3 ambient = sceneData.ambientColor.rgb * occlusion;
    // The sun term is left alone: it already has its own visibility test, and
    // scaling it here would darken contact points standing in full sunlight.
    vec3 direct = visibility * diffuse * sceneData.sunlightColor.rgb;
    if (sceneData.screenSpaceSettings.y > 0.5) {
        direct = vec3(0.0);
    }
    vec3 lighting = ambient + direct;

    outFragColor = vec4(baseColor.rgb * lighting, 1.0);
}
