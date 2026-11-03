#ifndef MIRABILIS_DYNAMIC_HIT_LIGHTING
#define MIRABILIS_DYNAMIC_HIT_LIGHTING
#include "punctual_lights.glsl"
float dynamic_segment_visibility(vec3 origin,vec3 direction,float span,float voxel)
{
    float t=0;
    for(int step=0;step<192 && t<span;++step) {
        float d=DYNAMIC_FIELD_DISTANCE(origin+direction*t);
        if(d<0.25*voxel) return 0;
        t+=max(d,field_minimum_step(voxel));
    }
    return t>=span ? 1 : 0;
}
vec3 dynamic_hit_radiance(vec3 p,vec3 normal,float voxel)
{
    vec3 albedo=textureLod(dynamicField,(p-dynamicBounds.minimum.xyz)/
        (dynamicBounds.maximum.xyz-dynamicBounds.minimum.xyz),0).rgb;
    vec3 origin=field_surface_origin(p,normal,voxel);
    uint sources=uint(sceneData.materialDebug.z);
    vec3 light=vec3(0);
    if((sources&1u)!=0u && any(greaterThan(sceneData.sunlightColor.rgb,vec3(0)))) {
    vec3 sun=normalize(sceneData.sunlightDirection.xyz);
#ifdef MIRABILIS_RT_SHADOWS
    float visibility=sunlight_visibility(p,normal);
#else
    vec3 t0=(field_bounds_min(fieldMin.xyz)-origin)/sun,t1=(field_bounds_max(fieldMax.xyz)-origin)/sun;
    vec3 exit=max(t0,t1);
    float visibility=sceneData.shadowSettings.w>0.5 ? dynamic_segment_visibility(origin,sun,min(min(exit.x,exit.y),exit.z),voxel) : 1;
#endif
    light=sceneData.sunlightColor.rgb*max(dot(normal,sun),0)*visibility;
    }
    if((sources&8u)==0u) return albedo*light;
    for(uint i=0u;i<uint(sceneData.graphicsSettings.x);++i) {
        PunctualLight source=PunctualLight(sceneData.localLightPosition[i],sceneData.localLightColor[i],
            sceneData.localLightDirection[i],sceneData.localLightCone[i]);
        vec3 direction;float distance;
        vec3 value=punctual_irradiance(source,p,direction,distance)*max(dot(normal,direction),0);
        vec3 delta=source.position.xyz-origin;
        float span=length(delta);
        direction=delta/max(span,1e-8);
        if(source.cone.y>=0) value*=dynamic_segment_visibility(origin,direction,span,voxel);
        light+=value;
    }
    return albedo*light;
}
#endif
