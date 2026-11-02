#ifndef MIRABILIS_REFLECTION_SETTINGS
#define MIRABILIS_REFLECTION_SETTINGS
bool reflection_roughness_owned(float roughness)
{
    // The forward decision and compute reconstruction must agree at the
    // inclusive threshold after the material target rounds to half precision.
    vec2 values=unpackHalf2x16(packHalf2x16(vec2(roughness,sceneData.reflectionSettings.y)));
    return sceneData.reflectionSettings.x>0.5 && values.x<=values.y;
}
#endif
