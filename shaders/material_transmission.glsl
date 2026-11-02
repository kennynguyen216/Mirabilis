// Screen-space approximation: one opaque background, not layered volume transport.
vec4 material_transmission(MaterialSurface surface,vec4 baseColor,vec3 shaded,vec3 reflection)
{
    float transmission=clamp(materialData.surfaceFactors.w,0.0,1.0);
    if(transmission<=0.0)return vec4(shaded,baseColor.a);
    float ior=max(materialData.transmissionFactors.x,1.0);
    vec3 bent=refract(-surface.view,surface.normal,1.0/ior)+surface.view;
    vec2 bentView=(sceneData.view*vec4(bent,0)).xy;
    vec2 offset=vec2(bentView.x,-bentView.y)*materialData.transmissionFactors.y*0.1;
    vec2 allocation=vec2(textureSize(opaqueSceneColor,0)),live=sceneData.graphicsSettings.zw;
    vec2 uv=gl_FragCoord.xy/allocation+offset*live/allocation,limit=(live-0.5)/allocation;
    vec3 behind=texture(opaqueSceneColor,clamp(uv,0.5/allocation,limit)).rgb;
    float blur=surface.roughness*surface.roughness*0.015;
    const vec2 taps[4]=vec2[](vec2(1,1),vec2(-1,1),vec2(1,-1),vec2(-1,-1));
    for(int i=0;i<4;++i)behind+=texture(opaqueSceneColor,clamp(uv+taps[i]*blur*live/allocation,0.5/allocation,limit)).rgb;
    behind*=0.2;
    vec3 fresnel=material_fresnel_schlick(vec3(pow((ior-1.0)/(ior+1.0),2.0)),surface.noV);
    vec3 glass=behind*mix(vec3(1),baseColor.rgb,baseColor.a)*(1.0-fresnel)+reflection;
    return vec4(mix(shaded,glass,transmission),mix(baseColor.a,1.0,transmission));
}
