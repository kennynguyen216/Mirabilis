#version 450

layout(location = 0) out vec4 outFragColor;

layout(push_constant) uniform constants
{
    vec4 data1;
    vec4 data2;
    // x = effect index, y/z = render width/height.
    vec4 settings;
} PushConstants;

float hash12(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

void main()
{
    // Use full-screen coordinates, just like the compute background.  The
    // sky is therefore independent of the portal's virtual camera and its
    // palette exactly follows the currently selected ImGui background effect.
    vec2 uv = gl_FragCoord.xy / PushConstants.settings.yz;

    if (PushConstants.settings.x < 0.5) {
        outFragColor = mix(PushConstants.data1, PushConstants.data2, uv.y);
        return;
    }

    vec3 skyColor = mix(
        PushConstants.data1.rgb * 0.2,
        PushConstants.data1.rgb,
        uv.y);
    vec2 cell = floor(gl_FragCoord.xy / 8.0);
    vec2 cellUV = fract(gl_FragCoord.xy / 8.0) - 0.5;
    float star = step(0.985, hash12(cell)) *
        smoothstep(0.5, 0.0, length(cellUV));
    outFragColor = vec4(
        skyColor + vec3(star * PushConstants.data1.a),
        1.0);
}
