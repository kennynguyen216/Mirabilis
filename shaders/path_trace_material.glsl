vec3 decodeSRGB(vec3 c) {return mix(c/12.92,pow((c+0.055)/1.055,vec3(2.4)),greaterThan(c,vec3(0.04045)));}
vec3 textureTexel(uvec4 info,ivec2 p) {
    ivec2 size=ivec2(info.yz); p=(p%size+size)%size;
    uint index=info.x+uint(p.y)*info.y+uint(p.x);
    if(index>=texels.length()) return vec3(1,0,1);
    uint rgba=texels[index];
    return decodeSRGB(vec3(rgba&255u,(rgba>>8u)&255u,(rgba>>16u)&255u)/255.0);
}
vec3 baseColorAt(Material m,Triangle t,vec2 bary) {
    vec2 uv=(vec2(t.n0.w,t.n1.w)*(1-bary.x-bary.y)+t.uv12.xy*bary.x+t.uv12.zw*bary.y)*m.uvScale.xy;
    vec3 color=m.baseColor.rgb*(t.c0.rgb*(1-bary.x-bary.y)+t.c1.rgb*bary.x+t.c2.rgb*bary.y);
    if(m.textureInfo.y>0u&&m.textureInfo.z>0u) {
        vec2 p=fract(uv)*vec2(m.textureInfo.yz)-0.5;
        ivec2 low=ivec2(floor(p)); vec2 f=fract(p);
        // Decode each texel before filtering: base color is sRGB even when
        // the legacy raster glTF upload used UNORM. Raster stays unchanged.
        color*=mix(mix(textureTexel(m.textureInfo,low),textureTexel(m.textureInfo,low+ivec2(1,0)),f.x),
            mix(textureTexel(m.textureInfo,low+ivec2(0,1)),textureTexel(m.textureInfo,low+ivec2(1,1)),f.x),f.y);
    }
    return clamp(color,0,1);
}
float ggxD(float noH,float alpha) {
    float a2=alpha*alpha,denom=noH*noH*(a2-1)+1;
    return a2/(3.14159265359*denom*denom);
}
float smithG1(float noV,float alpha) {return 2*noV/(noV+sqrt(alpha*alpha+(1-alpha*alpha)*noV*noV));}
vec3 fresnelSchlick(vec3 f0,float voH) {return f0+(1-f0)*pow(clamp(1-voH,0,1),5);}
float specularProbability(Material m) {return m.parameters.x>=0.999?1.0:0.5;}
vec3 evaluateBRDF(Material m,vec3 albedo,vec3 n,vec3 view,vec3 outgoing) {
    float noV=dot(n,view),noL=dot(n,outgoing);
    if(noV<=0||noL<=0) return vec3(0);
    if(pc.sampling.z==0u) return albedo/3.14159265359;
    vec3 sum=view+outgoing; if(dot(sum,sum)<1e-12) return vec3(0);
    vec3 h=normalize(sum); float voH=max(dot(view,h),0),noH=max(dot(n,h),0);
    float alpha=max(m.parameters.y*m.parameters.y,0.002025);
    vec3 f=fresnelSchlick(mix(vec3(0.04),albedo,clamp(m.parameters.x,0,1)),voH);
    vec3 diffuse=(1-f)*(1-clamp(m.parameters.x,0,1))*albedo/3.14159265359;
    vec3 specular=f*(ggxD(noH,alpha)*smithG1(noV,alpha)*smithG1(noL,alpha)/(4*noV*noL));
    return diffuse+specular;
}
float brdfPdf(Material m,vec3 n,vec3 view,vec3 outgoing) {
    float noL=max(dot(n,outgoing),0);
    if(noL<=0) return 0;
    if(pc.sampling.z==0u) return noL/3.14159265359;
    vec3 sum=view+outgoing; if(dot(sum,sum)<1e-12) return 0;
    vec3 h=normalize(sum); float voH=abs(dot(view,h)); if(voH<1e-8) return 0;
    float p=specularProbability(m),alpha=max(m.parameters.y*m.parameters.y,0.002025);
    return (1-p)*noL/3.14159265359+p*ggxD(max(dot(n,h),0),alpha)*max(dot(n,h),0)/(4*voH);
}
vec3 sampleBRDF(Material m,vec3 n,vec3 view,inout uint state) {
    if(pc.sampling.z==0u||randomFloat(state)>=specularProbability(m)) return cosineDirection(n,state);
    float u=randomFloat(state),phi=6.28318530718*randomFloat(state),alpha=max(m.parameters.y*m.parameters.y,0.002025);
    float cosine=sqrt((1-u)/(1+(alpha*alpha-1)*u)),sine=sqrt(max(0,1-cosine*cosine));
    vec3 tangent=normalize(cross(abs(n.z)<0.999?vec3(0,0,1):vec3(1,0,0),n));
    vec3 h=normalize(tangent*(sine*cos(phi))+cross(n,tangent)*(sine*sin(phi))+n*cosine);
    return reflect(-view,h);
}
float dielectricFresnel(float cosine,float etaIncident,float etaTransmitted) {
    cosine=clamp(cosine,0,1);
    float sineTransmitted=etaIncident/etaTransmitted*sqrt(max(0,1-cosine*cosine));
    if(sineTransmitted>=1) return 1;
    float ct=sqrt(max(0,1-sineTransmitted*sineTransmitted));
    float parallel=(etaTransmitted*cosine-etaIncident*ct)/(etaTransmitted*cosine+etaIncident*ct);
    float perpendicular=(etaIncident*cosine-etaTransmitted*ct)/(etaIncident*cosine+etaTransmitted*ct);
    return 0.5*(parallel*parallel+perpendicular*perpendicular);
}
