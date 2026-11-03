#ifndef MIRABILIS_PUNCTUAL_LIGHTS
#define MIRABILIS_PUNCTUAL_LIGHTS
struct PunctualLight { vec4 position; vec4 color; vec4 direction; vec4 cone; };
vec3 punctual_irradiance(PunctualLight light, vec3 position, out vec3 direction, out float distance)
{
    vec3 delta = light.position.xyz - position;
    distance = length(delta);
    direction = delta / max(distance, 1e-5);
    if (distance >= light.color.w) return vec3(0);
    float window = max(1.0 - pow(distance / light.color.w, 4.0), 0.0);
    vec3 value = light.color.rgb * window * window / (distance * distance + 1.0);
    if (light.position.w > 0.5)
        value *= smoothstep(light.cone.x, max(light.direction.w, light.cone.x + 1e-5),
                            dot(-direction, light.direction.xyz));
    return value;
}
#endif
