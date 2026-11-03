#ifndef MIRABILIS_RAY_SCENE_HIT
#define MIRABILIS_RAY_SCENE_HIT
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
struct RayVertex { vec4 positionU; vec4 normalV; vec4 color; vec4 tangent; };
layout(buffer_reference,std430,buffer_reference_align=16) readonly buffer RayVertices { RayVertex values[]; };
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer RayIndices { uint values[]; };
struct RayInstance {
    RayVertices vertices; RayIndices indices;
    vec4 normalRows[3]; vec4 baseColor; vec4 emission; uvec4 geometry;
};
layout(std430,set=0,binding=24) readonly buffer RayInstances { RayInstance rayInstances[]; };
uint ray_hit_index(rayQueryEXT query) {
    return rayQueryGetIntersectionInstanceCustomIndexEXT(query,true)+rayQueryGetIntersectionGeometryIndexEXT(query,true);
}
vec3 ray_hit_normal(rayQueryEXT query)
{
    uint index=ray_hit_index(query);
    RayInstance instance=rayInstances[index];
    uint first=instance.geometry.x+rayQueryGetIntersectionPrimitiveIndexEXT(query,true)*3u;
    RayVertex a=instance.vertices.values[instance.indices.values[first]];
    RayVertex b=instance.vertices.values[instance.indices.values[first+1u]];
    RayVertex c=instance.vertices.values[instance.indices.values[first+2u]];
    vec2 bary=rayQueryGetIntersectionBarycentricsEXT(query,true);
    vec3 n=a.normalV.xyz*(1-bary.x-bary.y)+b.normalV.xyz*bary.x+c.normalV.xyz*bary.y;
    if (any(isnan(n)) || any(isinf(n)) || dot(n,n)<1e-12) n=cross(b.positionU.xyz-a.positionU.xyz,c.positionU.xyz-a.positionU.xyz);
    n=vec3(dot(instance.normalRows[0].xyz,n),dot(instance.normalRows[1].xyz,n),dot(instance.normalRows[2].xyz,n));
    return normalize(n+vec3(1e-12));
}
vec3 ray_hit_albedo(rayQueryEXT query) {
    RayInstance instance=rayInstances[ray_hit_index(query)];
    if(instance.geometry.w!=0u) return vec3(0); // Uncaptured texture data is unknown.
    uint first=instance.geometry.x+rayQueryGetIntersectionPrimitiveIndexEXT(query,true)*3u;
    vec2 bary=rayQueryGetIntersectionBarycentricsEXT(query,true);
    vec3 color=instance.vertices.values[instance.indices.values[first]].color.rgb*(1-bary.x-bary.y)+
        instance.vertices.values[instance.indices.values[first+1u]].color.rgb*bary.x+
        instance.vertices.values[instance.indices.values[first+2u]].color.rgb*bary.y;
    return instance.baseColor.rgb*color*(1-clamp(instance.baseColor.a,0,1));
}

#endif
