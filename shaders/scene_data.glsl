// The per-camera uniform block, shared by every pass that needs the camera
// or the sun.  It lives in its own file because passes such as the
// depth/normal prepass need the camera but none of the material bindings in
// input_structures.glsl, and the std140 layout here must stay byte-identical
// to GPUSceneData in vk_types.h.
layout(set = 0, binding = 0) uniform SceneData {
    mat4 view;
    mat4 proj;
    mat4 viewproj;
    mat4 previousViewProjection;
    vec4 ambientColor;
    vec4 sunlightDirection;
    vec4 sunlightColor;
    vec4 portalClipPlane;
    vec4 portalClipEnabled;
    // World space -> sunlight clip space for the directional shadow map.
    // It is built from world-space positions only, so portal views sample
    // exactly the same shadows as the main camera.
    mat4 sunViewProjection;
    // x = constant depth bias, y = world-space normal offset,
    // z = one shadow-map texel in UV, w = 0 disables shadowing.
    vec4 shadowSettings;
    // Screen-space effects reconstruct a position from the depth buffer
    // instead of receiving it per fragment.  Both inverses are stored
    // because view-space work needs only the projection, while world-space
    // work needs the whole transform.
    mat4 inverseProjection;
    mat4 inverseViewProjection;
    // x = 1 while the ambient-occlusion image describes this camera's view.
    //     Portal cameras are bound the same image but see different geometry,
    //     so they set 0 and shade with unoccluded ambient light instead.
    // y = 1 suppresses the sunlight term, leaving ambient only.
    vec4 screenSpaceSettings;
    // xy = the occlusion texture coordinate that one screen pixel advances
    //      by, zw = the largest coordinate the rendered region reaches.
    vec4 ambientOcclusionUV;
    // x = environment intensity, y = 1 for an intentionally black fallback.
    vec4 ssgiFallbackSettings;
    // x = PCF footprint radius in shadow-map texels.
    vec4 shadowFilterSettings;
    // How the flat ambient term and the screen-space indirect estimate divide
    // the same job.  x = the fraction of ambient that survives while SSGI is
    // enabled, y = 1 to fill SSGI ray misses from the environment map rather
    // than the analytic gradient, z = the mip level to sample it at,
    // w = the radiance ceiling a miss ray may return, which separates the
    // sky from a sun disk the direct term already delivers.
    vec4 indirectSettings;
    // x = 1 while this camera substitutes an environment lookup for a
    //     screen-space indirect pass it cannot run.  Only portal cameras set
    //     it, and only while the main camera is running SSGI: both views have
    //     to divide the work between flat ambient and indirect light the same
    //     way, or the destination room changes colour as the player crosses.
    vec4 portalIndirectSettings;
    // xyz = world-space camera position; a portal camera's virtual position.
    vec4 cameraPosition;
    // 1 = on: x normal maps, y metallic/roughness textures, z specular
    // anti-aliasing, w specular.
    vec4 materialSettings;
} sceneData;
