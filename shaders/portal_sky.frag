#version 450

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
} PushConstants;

float hash12(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

vec2 equirectangular_uv(vec3 direction)
{
    const float pi = 3.14159265359;
    float longitude = atan(direction.z, direction.x) / (2.0 * pi) + 0.5;
    float latitude = acos(clamp(direction.y, -1.0, 1.0)) / pi;
    return vec2(longitude, latitude);
}

void main()
{
    // Gradient remains a screen-space teaching background.  The skybox path
    // below instead reconstructs a ray from this portal view's virtual
    // camera, so it matches the main camera sky across the opening.
    vec2 uv = gl_FragCoord.xy / PushConstants.settings.yz;

    if (PushConstants.settings.x < 0.5) {
        outFragColor = mix(PushConstants.data1, PushConstants.data2, uv.y);
        return;
    }

    vec2 screen = uv * 2.0 - 1.0;
    screen.y = -screen.y;
    float aspect = PushConstants.settings.y / PushConstants.settings.z;
    const float tanHalfFov = 0.70020754; // tan(70 degrees / 2)
    vec3 direction = normalize(
        PushConstants.cameraForward.xyz +
        screen.x * aspect * tanHalfFov * PushConstants.cameraRight.xyz +
        screen.y * tanHalfFov * PushConstants.cameraUp.xyz);
    outFragColor = vec4(texture(skyboxTexture, equirectangular_uv(direction)).rgb, 1.0);
}
