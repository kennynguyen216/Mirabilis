#version 450
#extension GL_GOOGLE_include_directive : require

#include "input_structures.glsl"

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

void main()
{
    vec4 baseColor = inColor * texture(colorTex, inUV);
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
    vec3 lighting = sceneData.ambientColor.rgb +
        diffuse * sceneData.sunlightColor.rgb;

    outFragColor = vec4(baseColor.rgb * lighting, 1.0);
}
