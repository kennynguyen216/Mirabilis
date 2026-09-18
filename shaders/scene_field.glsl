// The scene distance field as cascades, finest first (see update_scene_sdf).
// The fine cascades follow the camera; the last covers the whole scene and
// never moves.  All of them are stacked along z in one R8 volume, each
// storing distance / (maxDistanceVoxels x its own voxel), so a single sampler
// binding serves every cascade.
//
// Before including, the shader declares the stacked volume and defines:
//   SCENE_FIELD_TEXTURE           its sampler3D (default sceneField)
//   SCENE_FIELD_SET               set of the cascade table
//   SCENE_FIELD_BINDING_CASCADES  binding of the cascade table

#ifndef SCENE_FIELD_TEXTURE
#define SCENE_FIELD_TEXTURE sceneField
#endif

const int SceneFieldMaxCascades = 4;

struct SceneFieldCascade {
    // xyz = world position of the minimum corner, w = voxel size.
    vec4 origin;
    // xyz = voxel counts, w = first z slice in the stacked volume.
    vec4 size;
};

layout(std140, set = SCENE_FIELD_SET, binding = SCENE_FIELD_BINDING_CASCADES)
uniform SceneFieldCascades {
    // x = cascade count, y = stored distance in voxels.
    vec4 sceneFieldInfo;
    SceneFieldCascade sceneFieldCascade[SceneFieldMaxCascades];
};

// The finest cascade holding p with a voxel of margin for filtering; the
// whole-scene cascade otherwise.
int scene_field_cascade(vec3 p)
{
    int last = int(sceneFieldInfo.x) - 1;
    for (int i = 0; i < last; ++i) {
        vec3 v = (p - sceneFieldCascade[i].origin.xyz) / sceneFieldCascade[i].origin.w;
        if (all(greaterThan(v, vec3(1.0))) &&
            all(lessThan(v, sceneFieldCascade[i].size.xyz - 1.0))) {
            return i;
        }
    }
    return last;
}

float scene_field_sample(int cascade, vec3 p)
{
    SceneFieldCascade c = sceneFieldCascade[cascade];
    // Clamped half a voxel inside the cascade's own slab, so filtering never
    // reaches into its neighbour in the stack.
    vec3 v = clamp((p - c.origin.xyz) / c.origin.w, vec3(0.5), c.size.xyz - 0.5);
    v.z += c.size.w;
    return textureLod(SCENE_FIELD_TEXTURE, v / vec3(textureSize(SCENE_FIELD_TEXTURE, 0)), 0.0).r *
        sceneFieldInfo.y * c.origin.w;
}

// Distance to the nearest surface in world units, and the voxel size of the
// cascade that answered, which is what step and hit thresholds scale with.
float scene_field_distance(vec3 p, out float voxel)
{
    int cascade = scene_field_cascade(p);
    voxel = sceneFieldCascade[cascade].origin.w;
    return scene_field_sample(cascade, p);
}

float scene_field_distance(vec3 p)
{
    return scene_field_sample(scene_field_cascade(p), p);
}

float scene_field_voxel(vec3 p)
{
    return sceneFieldCascade[scene_field_cascade(p)].origin.w;
}

vec3 scene_field_normal(vec3 p)
{
    int cascade = scene_field_cascade(p);
    float h = sceneFieldCascade[cascade].origin.w;
    vec3 gradient = vec3(
        scene_field_sample(cascade, p + vec3(h, 0, 0)) - scene_field_sample(cascade, p - vec3(h, 0, 0)),
        scene_field_sample(cascade, p + vec3(0, h, 0)) - scene_field_sample(cascade, p - vec3(0, h, 0)),
        scene_field_sample(cascade, p + vec3(0, 0, h)) - scene_field_sample(cascade, p - vec3(0, 0, h)));
    return normalize(gradient + vec3(1e-8));
}
