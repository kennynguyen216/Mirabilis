#ifndef MIRABILIS_INDIRECT_COLOR_GLSL
#define MIRABILIS_INDIRECT_COLOR_GLSL
// Artistic control over incident GI, before receiver albedo is applied.
// Material color and direct lighting retain their original saturation.
vec3 indirect_color(vec3 light, float saturation)
{
    float luminance = dot(light, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(luminance), light, clamp(saturation, 0.0, 1.0));
}
#endif
