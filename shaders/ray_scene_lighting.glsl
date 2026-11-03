#ifndef MIRABILIS_RAY_SCENE_LIGHTING
#define MIRABILIS_RAY_SCENE_LIGHTING
#include "punctual_lights.glsl"
// Valid hit lighting for uncaptured rigid surfaces. Texture data and indirect
// lighting remain unknown here; never interpret a missing card as open sky.
vec3 ray_uncached_direct(vec3 position,vec3 normal)
{
    uint sources=uint(sceneData.materialDebug.z);
    vec3 light=(sources&1u)!=0u ? sceneData.sunlightColor.rgb*max(dot(normal,normalize(sceneData.sunlightDirection.xyz)),0)*sunlight_visibility(position,normal) : vec3(0);
    if((sources&8u)==0u) return light;
    float epsilon=max(0.0002,max(max(abs(position.x),abs(position.y)),abs(position.z))*0.000002);
    vec3 origin=position+normal*epsilon;
    for(int i=0;i<int(sceneData.graphicsSettings.x);++i) {
        PunctualLight source=PunctualLight(sceneData.localLightPosition[i],sceneData.localLightColor[i],sceneData.localLightDirection[i],sceneData.localLightCone[i]);
        vec3 direction;float distance;
        vec3 value=punctual_irradiance(source,position,direction,distance)*max(dot(normal,direction),0);
        if(!any(greaterThan(value,vec3(0)))) continue;
        if(source.cone.y>=0) {
            vec3 delta=source.position.xyz-origin;float endpoint=length(delta);
            if(endpoint<=0.0003) continue;
            rayQueryEXT shadow;
            rayQueryInitializeEXT(shadow,sunCasters,gl_RayFlagsTerminateOnFirstHitEXT|gl_RayFlagsOpaqueEXT,0xff,origin,0.0001,delta/endpoint,endpoint-0.0002);
            while(rayQueryProceedEXT(shadow)) {}
            if(rayQueryGetIntersectionTypeEXT(shadow,true)!=gl_RayQueryCommittedIntersectionNoneEXT) continue;
        }
        light+=value;
    }
    return light;
}
#endif
