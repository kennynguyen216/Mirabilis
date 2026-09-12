#include "path_trace_scene.h"
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <cmath>
#include <fmt/core.h>

std::vector<TraceBVHNode> build_trace_bvh(std::vector<TraceTriangle>& triangles) {
    std::vector<TraceBVHNode> nodes(1);
    if(triangles.empty()) return nodes;
    auto centroid=[](const TraceTriangle& t){return glm::vec3(t.p0+t.p1+t.p2)/3.f;};
    std::function<void(uint32_t,uint32_t,uint32_t,uint32_t)> build=[&](uint32_t index,uint32_t first,uint32_t count,uint32_t depth) {
        glm::vec3 lo(1e30f),hi(-1e30f),clo(1e30f),chi(-1e30f);
        for(uint32_t i=first;i<first+count;++i) {
            const auto& t=triangles[i]; lo=glm::min(lo,glm::min(glm::vec3(t.p0),glm::min(glm::vec3(t.p1),glm::vec3(t.p2))));
            hi=glm::max(hi,glm::max(glm::vec3(t.p0),glm::max(glm::vec3(t.p1),glm::vec3(t.p2))));
            clo=glm::min(clo,centroid(t)); chi=glm::max(chi,centroid(t));
        }
        // Pad only bounds, not triangle geometry, for flat and large-coordinate scenes.
        const glm::vec3 pad=glm::max(glm::vec3(1e-5f),glm::max(glm::abs(lo),glm::abs(hi))*1e-6f);
        nodes[index]={lo-pad,first,hi+pad,count};
        if(count<=4||depth>=48) return;
        glm::vec3 size=chi-clo; int axis=size.y>size.x?1:0; if(size.z>size[axis]) axis=2;
        uint32_t middle=first+count/2;
        std::nth_element(triangles.begin()+first,triangles.begin()+middle,triangles.begin()+first+count,
            [&](const auto& a,const auto& b){return centroid(a)[axis]<centroid(b)[axis];});
        uint32_t child=uint32_t(nodes.size()); nodes.resize(nodes.size()+2);
        nodes[index].first=child; nodes[index].count=0;
        build(child,first,middle-first,depth+1); build(child+1,middle,first+count-middle,depth+1);
    };
    build(0,0,uint32_t(triangles.size()),0); return nodes;
}
namespace {
bool aabb(const TraceBVHNode& n,glm::vec3 o,glm::vec3 d,float limit) {
    float low=1e-4f, high=limit;
    for(int a=0;a<3;++a) {
        if(std::abs(d[a])<1e-20f) {if(o[a]<n.minimum[a]||o[a]>n.maximum[a]) return false;}
        else {float x=(n.minimum[a]-o[a])/d[a],y=(n.maximum[a]-o[a])/d[a]; low=std::max(low,std::min(x,y)); high=std::min(high,std::max(x,y));}
    }
    return high>=low;
}
void triangle_hit(const TraceTriangle& t,int index,glm::vec3 o,glm::vec3 d,TraceCPUHit& hit) {
    glm::vec3 e1(t.p1-t.p0),e2(t.p2-t.p0),p=glm::cross(d,e2);
    float det=glm::dot(e1,p); if(std::abs(det)<1e-8f) return;
    glm::vec3 s=o-glm::vec3(t.p0); float u=glm::dot(s,p)/det; if(u<0||u>1) return;
    auto q=glm::cross(s,e1); float v=glm::dot(d,q)/det; if(v<0||u+v>1) return;
    float distance=glm::dot(e2,q)/det;
    if(distance>1e-4f&&distance<hit.t) hit={distance,index,glm::vec2(u,v)};
}
}
TraceCPUHit trace_cpu_intersect(const std::vector<TraceTriangle>& triangles,const std::vector<TraceBVHNode>& nodes,glm::vec3 o,glm::vec3 d,bool brute) {
    TraceCPUHit hit;
    if(brute) {for(size_t i=0;i<triangles.size();++i) triangle_hit(triangles[i],int(i),o,d,hit); return hit;}
    if(triangles.empty()) return hit;
    uint32_t stack[64],top=0; stack[top++]=0;
    while(top) {
        uint32_t i=stack[--top]; if(i>=nodes.size()) throw std::runtime_error("BVH child out of range");
        const auto& n=nodes[i]; if(!aabb(n,o,d,hit.t)) continue;
        if(n.count) {if(size_t(n.first)+n.count>triangles.size()) throw std::runtime_error("BVH leaf out of range");
            for(uint32_t j=n.first;j<n.first+n.count;++j) triangle_hit(triangles[j],int(j),o,d,hit);
        } else {if(top+2>64) throw std::runtime_error("BVH stack overflow"); stack[top++]=n.first; stack[top++]=n.first+1;}
    }
    return hit;
}
void validate_trace_bvh(const std::vector<TraceTriangle>& triangles,const std::vector<TraceBVHNode>& nodes) {
    uint32_t state=12345;
    auto random=[&](){state=state*1664525u+1013904223u; return float(state>>8)*(1.f/16777216.f);};
    float maxError=0;
    for(int i=0;i<4096;++i) {
        glm::vec3 o(random()*40-20,random()*20-5,random()*40-20);
        glm::vec3 d=glm::normalize(glm::vec3(random()*2-1,random()*2-1,random()*2-1));
        if(i<6) {d=glm::vec3(0); d[i/2]=i%2?1.f:-1.f;}
        auto a=trace_cpu_intersect(triangles,nodes,o,d,true), b=trace_cpu_intersect(triangles,nodes,o,d,false);
        if((a.triangle<0)!=(b.triangle<0)) throw std::runtime_error("BVH hit/miss disagreement");
        if(a.triangle>=0) maxError=std::max(maxError,std::abs(a.t-b.t));
    }
    fmt::print("GI CPU BVH vs brute force: 4096 rays, max t error={}\n",maxError);
    if(maxError>1e-4f) throw std::runtime_error("BVH distance disagreement");
}
