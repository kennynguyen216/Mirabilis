struct BVHNode { vec3 minimum; uint first; vec3 maximum; uint count; };
layout(std430,set=0,binding=4) readonly buffer Nodes { BVHNode nodes[]; };
struct Hit { float t; int triangle; vec2 bary; uint visits; uint tests; bool invalid; };
bool boxHit(BVHNode n,vec3 o,vec3 d,float limit) {
    float low=0.0001,high=limit;
    for(int a=0;a<3;++a) {
        if(abs(d[a])<1e-20) {if(o[a]<n.minimum[a]||o[a]>n.maximum[a]) return false;}
        else {float x=(n.minimum[a]-o[a])/d[a],y=(n.maximum[a]-o[a])/d[a]; low=max(low,min(x,y)); high=min(high,max(x,y));}
    }
    return high>=low;
}
void triangleHit(uint index,vec3 o,vec3 d,inout Hit hit) {
    ++hit.tests;
    Triangle t=triangles[index]; vec3 e1=t.p1.xyz-t.p0.xyz,e2=t.p2.xyz-t.p0.xyz,p=cross(d,e2);
    float det=dot(e1,p); if(abs(det)<1e-8) return;
    vec3 s=o-t.p0.xyz; float u=dot(s,p)/det; if(u<0||u>1) return;
    vec3 q=cross(s,e1); float v=dot(d,q)/det; if(v<0||u+v>1) return;
    float distance=dot(e2,q)/det;
    if(distance>0.0001&&distance<hit.t) {hit.t=distance; hit.triangle=int(index); hit.bary=vec2(u,v);}
}
Hit intersectScene(vec3 o,vec3 d) {
    Hit hit=Hit(1e30,-1,vec2(0),0u,0u,false);
    if(pc.settings.y==0) return hit;
    uint stack[64]; uint top=0; stack[top++]=0;
    while(top>0) {
        uint index=stack[--top];
        if(index>=nodes.length()) {hit.invalid=true; break;}
        BVHNode n=nodes[index]; ++hit.visits;
        if(!boxHit(n,o,d,hit.t)) continue;
        if(n.count>0) {
            if(n.first>=triangles.length()||n.count>triangles.length()-n.first) {hit.invalid=true; break;}
            for(uint j=n.first;j<n.first+n.count;++j) triangleHit(j,o,d,hit);
        } else {
            if(top+2>64||n.first+1>=nodes.length()) {hit.invalid=true; break;}
            stack[top++]=n.first; stack[top++]=n.first+1;
        }
    }
    return hit;
}
vec3 geometricNormal(Triangle t) {return normalize(cross(t.p1.xyz-t.p0.xyz,t.p2.xyz-t.p0.xyz));}
vec3 shadingNormal(Triangle t,vec2 b,vec3 geometric) {
    vec3 n=t.n0.xyz*(1-b.x-b.y)+t.n1.xyz*b.x+t.n2.xyz*b.y;
    n=dot(n,n)>1e-16?normalize(n):geometric;
    return dot(n,geometric)<0?-n:n;
}
