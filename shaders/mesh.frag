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
    vec3 normal = normalize(inNormal);
    vec3 lightDirection = normalize(sceneData.sunlightDirection.xyz);
    float diffuse = max(dot(normal, lightDirection), 0.0);
    vec3 lighting = sceneData.ambientColor.rgb +
        diffuse * sceneData.sunlightColor.rgb;

    outFragColor = vec4(baseColor.rgb * lighting, baseColor.a);
}
