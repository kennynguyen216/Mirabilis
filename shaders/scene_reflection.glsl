#ifndef MIRABILIS_SCENE_REFLECTION
#define MIRABILIS_SCENE_REFLECTION
#include "reflection_settings.glsl"
struct ReflectionHit { vec3 position; float identity; };
#ifdef MIRABILIS_RT_SHADOWS
#include "ray_scene_hit.glsl"
#include "ray_scene_lighting.glsl"
uint reflection_random(inout uint state) { state=state*747796405u+2891336453u; uint word=((state>>((state>>28u)+4u))^state)*277803737u; return (word>>22u)^word; }
float reflection_uniform(inout uint state) { return float(reflection_random(state)>>8u)/16777216.0; }
float reflection_smith(float cosine,float alpha) { return 2*cosine/(cosine+sqrt(alpha*alpha+(1-alpha*alpha)*cosine*cosine)); }
vec3 scene_reflected_radiance(vec3 origin,vec3 direction,out ReflectionHit hit)
{
    if(sceneData.reflectionSettings.w<0) { hit=ReflectionHit(vec3(0),-1); return vec3(1); }
    rayQueryEXT query;
    rayQueryInitializeEXT(query,sunCasters,gl_RayFlagsOpaqueEXT,0xff,origin,0.0001,direction,sceneData.reflectionSettings.w);
    while (rayQueryProceedEXT(query)) {}
    if (rayQueryGetIntersectionTypeEXT(query,true)==gl_RayQueryCommittedIntersectionNoneEXT) {
        hit=ReflectionHit(origin+direction*sceneData.reflectionSettings.w,-1);
        return apply_environment_policy(sceneData.indirectSettings.y>0.5 ? textureLod(environmentTexture,equirectangular_uv(direction),0).rgb : gradient_radiance(direction));
    }
    hit.position=origin+direction*rayQueryGetIntersectionTEXT(query,true);
    hit.identity=float(rayQueryGetIntersectionInstanceCustomIndexEXT(query,true)+1u);
    vec3 normal=ray_hit_normal(query);
    if (dot(normal,direction)>0) normal=-normal;
    RayInstance instance=rayInstances[rayQueryGetIntersectionInstanceCustomIndexEXT(query,true)];
    SurfaceCacheSample surface;surface.weight=0;
    if(instance.geometry.z!=0u) surface=REFLECTION_CACHE_LOOKUP(hit.position,normal);
    // Specular paths see emission even when diffuse emitter sampling owns it.
    if (surface.weight>0) return surface.emissive+surface.albedo*(surface.direct+surface.indirect);
    // A real hit without cache coverage is not an environment escape.
    return ray_hit_albedo(query)*ray_uncached_direct(hit.position,normal)+
        (dot(ray_hit_normal(query),direction)<0 ? instance.emission.rgb : vec3(0));
}
vec3 scene_reflection(vec3 position,vec3 normal,vec3 geometric,vec3 view,vec3 f0,float roughness,uint seed,out ReflectionHit firstHit)
{
    vec3 sum=vec3(0);
    firstHit=ReflectionHit(vec3(0),-2);
    float noV=max(dot(normal,view),1e-5);
    float alpha=max(roughness*roughness,0.002025);
    int samples=roughness<0.05?1:int(sceneData.reflectionSettings.z);
    vec3 helper=abs(normal.z)<0.999?vec3(0,0,1):vec3(1,0,0);
    vec3 tangent=normalize(cross(helper,normal)),bitangent=cross(normal,tangent);
    float epsilon=max(0.0002,max(max(abs(position.x),abs(position.y)),abs(position.z))*0.000002);
    for (int i=0;i<samples;++i) {
        // Heitz 2018: sample visible GGX normals in stretched view space.
        // The separable Smith BRDF/PDF weight becomes Fresnel * G1(light).
        vec3 localView=vec3(dot(view,tangent),dot(view,bitangent),noV);
        vec3 stretched=normalize(vec3(alpha*localView.xy,localView.z));
        float length2=dot(stretched.xy,stretched.xy);
        vec3 t1=length2>0?vec3(-stretched.y,stretched.x,0)/sqrt(length2):vec3(1,0,0);
        vec3 t2=cross(stretched,t1);
        float radius=sqrt(reflection_uniform(seed)),phi=6.28318530718*reflection_uniform(seed);
        float diskX=radius*cos(phi),diskY=radius*sin(phi),blend=.5*(1+stretched.z);
        diskY=(1-blend)*sqrt(max(0,1-diskX*diskX))+blend*diskY;
        vec3 projected=diskX*t1+diskY*t2+sqrt(max(0,1-diskX*diskX-diskY*diskY))*stretched;
        vec3 localHalf=normalize(vec3(alpha*projected.xy,max(0,projected.z)));
        vec3 halfVector=normalize(tangent*localHalf.x+bitangent*localHalf.y+normal*localHalf.z);
        vec3 outgoing=reflect(-view,halfVector);
        float noL=dot(normal,outgoing),voH=dot(view,halfVector);
        if (noL<=0 || voH<=0 || dot(geometric,outgoing)<=0) continue;
        ReflectionHit hit;
        vec3 radiance=scene_reflected_radiance(position+geometric*epsilon,outgoing,hit);
        if (i==0) firstHit=hit;
        vec3 fresnel=f0+(1-f0)*pow(clamp(1-voH,0,1),5);
        vec3 weight=fresnel*reflection_smith(noL,alpha);
        sum+=radiance*weight;
    }
    return sum/float(samples);
}
#else
vec3 scene_reflection(vec3 position,vec3 normal,vec3 geometric,vec3 view,vec3 f0,float roughness,uint seed,out ReflectionHit hit) { hit=ReflectionHit(vec3(0),-2); return vec3(0); }
#endif
#endif
