// The coordinate convention shared by the occlusion sampling pass, the
// bilateral blur, and the composition in input_structures.glsl.
//
// Three different pixel grids are in play and mixing them up is the easiest
// way to get this effect subtly wrong:
//
//   * the occlusion images, allocated at half the window size;
//   * the prepass depth/normal images, allocated at the window size;
//   * the region of both that the current render scale actually filled.
//
// Neither set of images is reallocated when the render scale changes, so a
// UV of 1.0 is the edge of the allocation, not the edge of the live image.
// Every lookup below therefore converts through an explicit fraction rather
// than assuming the two coincide.

layout(push_constant) uniform constants {
    // xy = the occlusion extent dispatched this frame; invocations past it
    //      return without writing.  zw = the blur tap direction in occlusion
    //      texels, (1,0) or (0,1); unused by the sampling pass.
    ivec4 extents;
    // xy = one occlusion texel in UV, zw = one prepass texel in UV, both
    // taken from the allocated sizes.
    vec4 texelSize;
    // xy = the fraction of the occlusion image the active region fills,
    // zw = the same fraction of the prepass images.  A screen-space [0,1]
    // coordinate is scaled by these to reach the live texels.
    vec4 activeFraction;
    // xy = one full-resolution pixel in screen UV.
    vec4 screenTexel;
    // Sampling pass: x = radius, y = bias, z = power, w = intensity.
    // Blur pass:     x = depth falloff, y = normal falloff exponent.
    vec4 settings;
} PushConstants;

// The main camera uses reversed depth, so the cleared far value is 0 and a
// pixel still holding it was never covered by geometry.
const float BackgroundDepth = 1e-6;

// The centre of the full-resolution 2x2 block this occlusion pixel stands
// for, in screen space where 0..1 spans the rendered region.
vec2 screen_uv_for(ivec2 aoPixel)
{
    return (vec2(aoPixel) * 2.0 + 1.0) * PushConstants.screenTexel.xy;
}

// Screen space -> a texture coordinate in the prepass allocation.
vec2 prepass_uv(vec2 screenUV)
{
    vec2 limit = PushConstants.activeFraction.zw -
        PushConstants.texelSize.zw * 0.5;
    return clamp(screenUV * PushConstants.activeFraction.zw, vec2(0.0), limit);
}

// Screen space -> a texture coordinate in the occlusion allocation.
vec2 ao_uv(vec2 screenUV)
{
    vec2 limit = PushConstants.activeFraction.xy -
        PushConstants.texelSize.xy * 0.5;
    return clamp(screenUV * PushConstants.activeFraction.xy, vec2(0.0), limit);
}

// Vulkan clip space already runs z in [0,1] and the stored projection carries
// the Y flip, so its inverse undoes both without a correction here.  Reversed
// depth needs no manual un-reversing either: it is baked into the projection
// this matrix inverts.
vec3 view_position_from_depth(vec2 screenUV, float depth)
{
    vec4 clipPosition = vec4(screenUV * 2.0 - 1.0, depth, 1.0);
    vec4 viewPosition = sceneData.inverseProjection * clipPosition;
    return viewPosition.xyz / viewPosition.w;
}
