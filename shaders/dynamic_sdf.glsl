#ifndef MIRABILIS_DYNAMIC_SDF
#define MIRABILIS_DYNAMIC_SDF
#ifndef MIRABILIS_DYNAMIC_FIELD_ENABLED
#define MIRABILIS_DYNAMIC_FIELD_ENABLED true
#endif
layout(set=0,binding=26) uniform sampler3D dynamicField;
layout(std140,set=0,binding=27) uniform DynamicFieldBounds { vec4 minimum; vec4 maximum; } dynamicBounds;

float dynamic_field_distance(vec3 world)
{
    if (!MIRABILIS_DYNAMIC_FIELD_ENABLED || dynamicBounds.maximum.w<0.5) return 1e20;
    vec3 p=clamp(world,dynamicBounds.minimum.xyz,dynamicBounds.maximum.xyz);
    float outside=length(world-p);
    float stored=textureLod(dynamicField,(p-dynamicBounds.minimum.xyz)/
        (dynamicBounds.maximum.xyz-dynamicBounds.minimum.xyz),0).a;
    return outside>0 ? length(vec2(outside,max(stored,0))) : stored;
}

bool dynamic_field_candidate(vec3 world,float distance)
{
    if(!MIRABILIS_DYNAMIC_FIELD_ENABLED || dynamicBounds.maximum.w<0.5) return false;
    vec3 outside=max(max(dynamicBounds.minimum.xyz-world,world-dynamicBounds.maximum.xyz),vec3(0));
    float squared=dot(outside,outside);
    return squared==0 || (distance>0 && squared<=distance*distance);
}

float dynamic_field_union(vec3 world,float distance)
{
    // The box is a lower bound too; avoid another 3D fetch when it cannot win.
    if(!dynamic_field_candidate(world,distance)) return distance;
    return min(distance,dynamic_field_distance(world));
}

float bounded_field_distance(sampler3D field,vec3 world,vec3 lo,vec3 hi)
{
    vec3 p=clamp(world,lo,hi);
    float outside=length(world-p);
    float stored=textureLod(field,(p-lo)/(hi-lo),0).r;
    return outside>0 ? length(vec2(outside,max(stored,0))) : stored;
}

vec3 field_bounds_min(vec3 lo) { return MIRABILIS_DYNAMIC_FIELD_ENABLED && dynamicBounds.maximum.w>0.5 ? min(lo,dynamicBounds.minimum.xyz) : lo; }
vec3 field_bounds_max(vec3 hi) { return MIRABILIS_DYNAMIC_FIELD_ENABLED && dynamicBounds.maximum.w>0.5 ? max(hi,dynamicBounds.maximum.xyz) : hi; }

float dynamic_field_voxel(vec3 world,float distance,float voxel)
{
    if(!dynamic_field_candidate(world,distance)) return voxel;
    return dynamic_field_distance(world)<=distance ? min(voxel,dynamicBounds.minimum.w) : voxel;
}

float field_minimum_step(float voxel)
{
    return 0.5*(MIRABILIS_DYNAMIC_FIELD_ENABLED && dynamicBounds.maximum.w>0.5 ? min(voxel,dynamicBounds.minimum.w) : voxel);
}

#endif
