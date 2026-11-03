#include "punctual_lights.glsl"
layout(set=0,binding=22) uniform sampler2DShadow localShadowAtlas;

float local_shadow(int index,vec3 position,vec3 normal)
{
    int slot=int(sceneData.localLightCone[index].y);
    if (slot<0) return 1.0;
    if (sceneData.localLightPosition[index].w<0.5) {
        vec3 delta=position-sceneData.localLightPosition[index].xyz;
        vec3 a=abs(delta);
        slot=a.x>=a.y && a.x>=a.z ? (delta.x>=0?0:1) : a.y>=a.z ? (delta.y>=0?2:3) : (delta.z>=0?4:5);
    }
    vec4 clip=sceneData.localShadowMatrix[slot]*vec4(position+normal*0.015,1);
    if (clip.w<=0) return 1.0;
    vec3 p=clip.xyz/clip.w;p.xy=p.xy*0.5+0.5;
    if (any(lessThan(p,vec3(0))) || any(greaterThan(p,vec3(1)))) return 1.0;
    vec2 tile=vec2(slot%4,slot/4), atlas=vec2(2048,1024);
    vec2 pixel=clamp(p.xy*512.0,vec2(1.5),vec2(510.5))+tile*512.0;
    float result=0;
    for (int y=-1;y<=1;y+=2) for (int x=-1;x<=1;x+=2)
        result+=texture(localShadowAtlas,vec3((pixel+vec2(x,y)*0.5)/atlas,p.z-0.001));
    return result*0.25;
}

void material_local_lights(MaterialSurface surface,vec3 position,vec3 geometricNormal,out vec3 diffuse,out vec3 specular)
{
    diffuse=vec3(0);specular=vec3(0);
    if (sceneData.screenSpaceSettings.y>0.5) return;
    for (int i=0;i<int(sceneData.graphicsSettings.x);++i) {
        vec3 direction; float distance;
        PunctualLight light=PunctualLight(sceneData.localLightPosition[i],sceneData.localLightColor[i],
            sceneData.localLightDirection[i],sceneData.localLightCone[i]);
        vec3 radiance=punctual_irradiance(light,position,direction,distance);
        if (!any(greaterThan(radiance,vec3(0)))) continue;
        vec3 d,s;
        material_direct_light(surface,direction,radiance*local_shadow(i,position,geometricNormal),d,s);
        diffuse+=d;specular+=s;
    }
}
