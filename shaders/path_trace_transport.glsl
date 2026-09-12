uint randomUint(inout uint state) {
    state=state*747796405u+2891336453u;
    uint word=((state>>((state>>28u)+4u))^state)*277803737u;
    return (word>>22u)^word;
}
float randomFloat(inout uint state) {return float(randomUint(state)>>8u)*(1.0/16777216.0);}
vec3 cosineDirection(vec3 n,inout uint state) {
    float u=randomFloat(state),phi=6.28318530718*randomFloat(state);
    vec3 tangent=normalize(cross(abs(n.z)<0.999?vec3(0,0,1):vec3(1,0,0),n));
    return normalize(tangent*(sqrt(u)*cos(phi))+cross(n,tangent)*(sqrt(u)*sin(phi))+n*sqrt(1-u));
}
#include "path_trace_material.glsl"
vec3 environmentRadiance(vec3 d) {return lighting.environment.y>0.5?vec3(0):lighting.environment.x*mix(vec3(0.7,0.8,1),vec3(0.12,0.3,0.65),clamp(d.y*0.5+0.5,0,1));}
vec3 offsetOrigin(vec3 p,vec3 geometric,vec3 outgoing) {
    float epsilon=max(0.0002,max(abs(p.x),max(abs(p.y),abs(p.z)))*0.000002);
    return p+geometric*(dot(geometric,outgoing)>0?epsilon:-epsilon);
}
struct TransportResult {vec3 direct; vec3 indirect; bool invalid;};
void addContribution(inout TransportResult r,vec3 value,bool direct) {
    if(any(isnan(value))||any(isinf(value))) {r.invalid=true;return;}
    if(direct) r.direct+=value; else r.indirect+=value;
}
void sampleLights(inout TransportResult result,Material material,vec3 albedo,vec3 point,vec3 geometric,vec3 normal,vec3 view,
    vec3 throughput,uint bounce,uint portalCount,inout uint state) {
    uint remaining=min(pc.sampling.w,4u)-min(portalCount,min(pc.sampling.w,4u));
    for(uint kind=0;kind<2u;++kind) {
        if(kind==1u&&(remaining==0u||lighting.counts.y==0u)) continue;
        uint chain[4]=uint[4](0u,0u,0u,0u); uint chainLength=0; float inverseChainPdf=1;
        if(kind==1u) {
            chainLength=min(uint(randomFloat(state)*float(remaining)),remaining-1u)+1u;
            inverseChainPdf=float(remaining);
            for(uint k=0;k<chainLength;++k) {
                chain[k]=min(uint(randomFloat(state)*float(lighting.counts.y)),lighting.counts.y-1u);
                inverseChainPdf*=float(lighting.counts.y);
            }
        }
        if(any(greaterThan(lighting.sunRadiance.rgb,vec3(0)))) {
            vec3 outgoing=lighting.sunDirection.xyz;
            for(uint k=chainLength;k>0u;--k) outgoing=(portals[chain[k-1u]].inverseTransfer*vec4(outgoing,0)).xyz;
            outgoing=normalize(outgoing);
            if(dot(outgoing,geometric)>0&&dot(outgoing,normal)>0&&
                connectionVisible(offsetOrigin(point,geometric,outgoing),outgoing,chain,chainLength,1e30,true))
                addContribution(result,throughput*evaluateBRDF(material,albedo,normal,view,outgoing)*dot(normal,outgoing)*lighting.sunRadiance.rgb*inverseChainPdf,bounce==0);
        }
        if(lighting.counts.x>0u) {
            uint lightIndex=min(uint(randomFloat(state)*float(lighting.counts.x)),lighting.counts.x-1u);
            Triangle emitter=triangles[emitters[lightIndex]];
            float u=sqrt(randomFloat(state)),v=randomFloat(state);
            vec3 lightPoint=emitter.p0.xyz*(1-u)+emitter.p1.xyz*(u*(1-v))+emitter.p2.xyz*(u*v);
            vec3 virtualPoint=lightPoint;
            for(uint k=chainLength;k>0u;--k) virtualPoint=(portals[chain[k-1u]].inverseTransfer*vec4(virtualPoint,1)).xyz;
            vec3 delta=virtualPoint-point; float distanceSquared=dot(delta,delta);
            if(distanceSquared<=1e-10) continue;
            vec3 outgoing=delta*inversesqrt(distanceSquared),finalDirection=outgoing;
            for(uint k=0;k<chainLength;++k) finalDirection=(portals[chain[k]].transfer*vec4(finalDirection,0)).xyz;
            vec3 crossEdges=cross(emitter.p1.xyz-emitter.p0.xyz,emitter.p2.xyz-emitter.p0.xyz);
            float twiceArea=length(crossEdges),cosineLight=abs(dot(crossEdges/twiceArea,-normalize(finalDirection)));
            if(dot(outgoing,geometric)<=0||dot(outgoing,normal)<=0||cosineLight<=1e-8) continue;
            vec3 shadowOrigin=offsetOrigin(point,geometric,outgoing),shadowDelta=virtualPoint-shadowOrigin;
            float shadowDistance=length(shadowDelta);
            if(connectionVisible(shadowOrigin,shadowDelta/shadowDistance,chain,chainLength,shadowDistance,false)) {
                float inversePdf=cosineLight*(0.5*twiceArea)*float(lighting.counts.x)/distanceSquared*inverseChainPdf;
                addContribution(result,throughput*evaluateBRDF(material,albedo,normal,view,outgoing)*dot(normal,outgoing)*materials[emitter.meta.x].emission.rgb*inversePdf,bounce==0);
            }
        }
    }
}
TransportResult transport(vec3 origin,vec3 direction,inout uint state) {
    TransportResult result=TransportResult(vec3(0),vec3(0),false);
    vec3 throughput=vec3(1); bool previousDelta=false; uint portalCount=0;
    for(uint bounce=0;bounce<=min(pc.sampling.x,4u);++bounce) {
        bool limited; float travelled;
        Hit hit=intersectPortalScene(origin,direction,portalCount,limited,travelled);
        if(limited) break;
        if(hit.invalid) {result.invalid=true;break;}
        if(hit.triangle<0) {addContribution(result,throughput*environmentRadiance(direction),bounce<=1);break;}
        Triangle t=triangles[hit.triangle];
        if(t.meta.x>=materials.length()) {result.invalid=true;break;}
        Material material=materials[t.meta.x];
        // Area emitters are sampled explicitly at every diffuse vertex.
        // Count camera-visible emission, suppress BSDF-hit emission after a
        // diffuse scatter to avoid counting the same light path twice.
        if(bounce==0||previousDelta) addContribution(result,throughput*material.emission.rgb,bounce<=1);
        if(bounce==pc.sampling.x) break;
        vec3 geometric=geometricNormal(t),normal=shadingNormal(t,hit.bary,geometric);
        bool front=dot(direction,geometric)<0; geometric=front?geometric:-geometric; normal=front?normal:-normal;
        vec3 point=origin+direction*hit.t,albedo=baseColorAt(material,t,hit.bary);
        float transmission=pc.sampling.z==0u?0:clamp(material.parameters.z,0,1);
        if(randomFloat(state)<transmission) {
            float ior=clamp(material.parameters.w,1.0,3.0),etaIncident=front?1:ior,etaTransmitted=front?ior:1;
            // Perfect dielectric interfaces use the geometric normal. The
            // radiance-mode eta^2 factors cancel on entry/exit through glass.
            float f=dielectricFresnel(max(dot(-direction,geometric),0),etaIncident,etaTransmitted);
            vec3 next;
            if(randomFloat(state)<f) next=reflect(direction,geometric);
            else {next=refract(direction,geometric,etaIncident/etaTransmitted);throughput*=pow(etaIncident/etaTransmitted,2);}
            if(dot(next,next)<1e-12) {result.invalid=true;break;}
            direction=normalize(next);origin=offsetOrigin(point,geometric,direction); previousDelta=true;
            continue;
        }
        previousDelta=false;
        sampleLights(result,material,albedo,point,geometric,normal,-direction,throughput,bounce,portalCount,state);
        vec3 next=sampleBRDF(material,normal,-direction,state);
        if(dot(next,geometric)<=0) break;
        float pdf=brdfPdf(material,normal,-direction,next);
        if(pdf<=1e-12||isnan(pdf)||isinf(pdf)) break;
        throughput*=evaluateBRDF(material,albedo,normal,-direction,next)*max(dot(normal,next),0)/pdf;
        if(any(isnan(throughput))||any(isinf(throughput))) {result.invalid=true;break;}
        if(bounce>=2) {
            float survival=clamp(max(throughput.x,max(throughput.y,throughput.z)),0.05,0.95);
            if(randomFloat(state)>survival) break;
            throughput/=survival;
        }
        origin=offsetOrigin(point,geometric,next); direction=next;
    }
    return result;
}
