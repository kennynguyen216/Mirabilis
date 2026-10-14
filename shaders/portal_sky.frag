#version 450
#extension GL_GOOGLE_include_directive : require
#include "environment_gradient.glsl"

layout(location = 0) out vec4 outFragColor;

layout(set = 0, binding = 0) uniform sampler2D skyboxTexture;

layout(push_constant) uniform constants
{
    vec4 data1;
    vec4 data2;
    // x = effect index, y/z = render width/height.
    vec4 settings;
    vec4 cameraRight;
    vec4 cameraUp;
    vec4 cameraForward;
    vec4 environmentTint;
} PushConstants;

void main()
{
    // Match the main camera's world-direction sky and environment policy.
    vec2 uv = gl_FragCoord.xy / PushConstants.settings.yz;
    if (PushConstants.settings.w < 0.0 && PushConstants.settings.x < 0.5) {
        outFragColor = mix(PushConstants.data1, PushConstants.data2, uv.y);
        return;
    }

    vec2 screen = uv * 2.0 - 1.0;
    screen.y = -screen.y;
    float aspect = PushConstants.cameraRight.w;
    const float tanHalfFov = 0.70020754; // tan(70 degrees / 2)
    vec3 direction = normalize(
        PushConstants.cameraForward.xyz +
        screen.x * aspect * tanHalfFov * PushConstants.cameraRight.xyz +
        screen.y * tanHalfFov * PushConstants.cameraUp.xyz);
    vec3 radiance = PushConstants.settings.x > 0.5
        ? (PushConstants.settings.w < 0.0
            ? texture(skyboxTexture, equirectangular_uv(direction)).rgb
            : textureLod(skyboxTexture, equirectangular_uv(direction), 0.0).rgb)
        : gradient_radiance(direction);
    outFragColor = vec4(PushConstants.settings.w < 0.0 ? radiance
        : (PushConstants.data1.x > 0.5 ? vec3(0.0)
            : radiance * PushConstants.settings.w * PushConstants.environmentTint.rgb), 1.0);
}
