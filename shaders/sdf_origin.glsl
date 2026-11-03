vec3 field_surface_origin(vec3 world,vec3 normal,float voxel)
{
    float offset=2.5*voxel;
    // Trilinear shell minima can shift almost one voxel beyond the captured surface.
    float previous=ORIGIN_FIELD_DISTANCE(world+normal*(offset*3.0*0.125));
    for(int i=4;i<=8;++i) {
        float distance=ORIGIN_FIELD_DISTANCE(world+normal*(offset*float(i)*0.125));
        // A falling distance enters another shell; never relocate a ray through it.
        if(distance<previous-0.05*voxel) return world+normal*(offset*float(i-1)*0.125);
        previous=distance;
    }
    return world+normal*offset;
}
