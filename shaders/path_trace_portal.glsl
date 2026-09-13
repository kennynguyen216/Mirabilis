struct TracePortal { vec4 center; vec4 rightWidth; vec4 upHeight; vec4 normal; mat4 transfer; mat4 inverseTransfer; };
layout(std430,set=0,binding=11) readonly buffer Portals { TracePortal portals[]; };
int closestPortal(vec3 origin,vec3 direction,float limit,out float distance) {
    int result=-1; distance=limit;
    for(uint i=0;i<lighting.counts.y;++i) {
        TracePortal portal=portals[i]; float denominator=dot(direction,portal.normal.xyz);
        if(abs(denominator)<1e-8) continue;
        float t=dot(portal.center.xyz-origin,portal.normal.xyz)/denominator;
        if(t<=0.0001||t>distance+0.0001) continue;
        vec3 local=origin+direction*t-portal.center.xyz;
        if(abs(dot(local,portal.rightWidth.xyz))>portal.rightWidth.w||abs(dot(local,portal.upHeight.xyz))>portal.upHeight.w) continue;
        result=int(i);distance=t;
    }
    return result;
}
void transferRay(uint index,inout vec3 origin,inout vec3 direction,float distance) {
    origin=(portals[index].transfer*vec4(origin+direction*distance,1)).xyz;
    direction=normalize((portals[index].transfer*vec4(direction,0)).xyz);
    origin+=direction*0.0004;
}
Hit intersectPortalScene(inout vec3 origin,inout vec3 direction,inout uint count,out bool limited,out float travelled) {
    limited=false; travelled=0;
    uint visits=0,tests=0;
    for(uint step=0;step<=4u;++step) {
        Hit hit=intersectScene(origin,direction); visits+=hit.visits;tests+=hit.tests;
        float distance; int portal=closestPortal(origin,direction,hit.t,distance);
        if(portal<0||hit.invalid) {hit.visits=visits;hit.tests=tests;return hit;}
        if(portals[portal].center.w==0) {hit.invalid=true;return hit;}
        if(count>=min(pc.sampling.w,4u)) {limited=true;return hit;}
        ++count; travelled+=distance;
        transferRay(uint(portal),origin,direction,distance);
    }
    limited=true;return Hit(1e30,-1,vec2(0),visits,tests,false);
}
// Tests a sampled direct-light connection with a prescribed portal chain.
// Chains partition light paths. The caller divides by their discrete PDF.
bool connectionVisible(vec3 origin,vec3 direction,uint chain[4],uint length,float remaining,bool infinite) {
    for(uint k=0;k<length;++k) {
        Hit hit=intersectScene(origin,direction);
        float distance; int portal=closestPortal(origin,direction,min(hit.t,remaining),distance);
        if(hit.invalid||portal<0||uint(portal)!=chain[k]||portals[portal].center.w==0) return false;
        if(!infinite) remaining-=distance+0.0004;
        transferRay(uint(portal),origin,direction,distance);
    }
    Hit hit=intersectScene(origin,direction);
    float distance;
    if(hit.invalid||closestPortal(origin,direction,min(hit.t,remaining),distance)>=0) return false;
    return infinite?hit.triangle<0:hit.t>=remaining-max(0.0005,remaining*1e-5);
}
