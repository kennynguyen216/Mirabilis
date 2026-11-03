#version 450
#extension GL_GOOGLE_include_directive : require
#include "cel_shading.glsl"

// The one place linear HDR becomes a displayable image.  Everything before
// this writes unbounded linear radiance into an R16G16B16A16_SFLOAT target;
// the swapchain is an 8-bit UNORM buffer the display decodes as sRGB.  Without
// this pass those linear values were written straight into that buffer, so
// every midtone arrived darker than it should and anything above 1.0 was
// silently flattened by the blit.
layout(set = 0, binding = 0) uniform sampler2D hdrImage;
layout(set=0,binding=1) uniform sampler2D bloomImage;
layout(set=0,binding=2) uniform sampler2D normalImage;
layout(set=0,binding=3) uniform sampler2D albedoImage;

layout(push_constant) uniform TonemapSettings {
    // x = exposure multiplier, y = operator (0 ACES, 1 Reinhard),
    // z = 1 while the tonemap is bypassed and only the encode runs.
    vec4 settings;
    vec4 whiteBalanceGain;
    vec4 bloom;
    vec4 cel;
    vec4 extent;
} push;

layout(location = 0) out vec4 outFragColor;

// Narkowicz's curve fit to the ACES filmic response.  It is cheap enough to
// run per pixel and, unlike a plain clamp, rolls highlights off gradually
// instead of shearing them flat at 1.0.  Its output is still linear, so the
// sRGB encode below is a separate and necessary step.
vec3 tonemap_aces(vec3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Maps [0, inf) onto [0, 1) and never quite reaches white.  It desaturates
// less than ACES and is the better choice for reading absolute brightness in
// the SSGI buffers, which is why both are kept.
vec3 tonemap_reinhard(vec3 x)
{
    return x / (1.0 + x);
}

// The sRGB transfer function proper, piecewise linear near black rather than
// a plain pow(1/2.2).  The difference is only visible in the darkest few
// values, which is exactly where ambient-lit interiors sit.
vec3 linear_to_srgb(vec3 linearColor)
{
    return mix(
        linearColor * 12.92,
        1.055 * pow(linearColor, vec3(1.0 / 2.4)) - 0.055,
        greaterThan(linearColor, vec3(0.0031308)));
}

// Khronos PBR Neutral, also offered by FirstPrinciples' presentation pass.
vec3 tonemap_neutral(vec3 color)
{
    float minimum=min(color.r,min(color.g,color.b));
    color-=minimum<0.08?minimum-6.25*minimum*minimum:0.04;
    float peak=max(color.r,max(color.g,color.b));
    const float start=0.76,d=0.24;
    if(peak<start)return color;
    float compressed=1.0-d*d/(peak+d-start);
    color*=compressed/peak;
    return mix(color,vec3(compressed),1.0-1.0/(0.15*(peak-compressed)+1.0));
}
void main()
{
    // texelFetch, not texture(): this pass is strictly one output pixel per
    // input pixel, so there is nothing to interpolate and no need to bound the
    // read against the region the current render scale actually filled.
    vec4 hdr = texelFetch(hdrImage, ivec2(gl_FragCoord.xy), 0);

    vec3 color=hdr.rgb;
    bool cel=push.cel.x>0.5 || texelFetch(albedoImage,ivec2(gl_FragCoord.xy),0).a>0.5;
    if(cel) {
        vec3 normal=texelFetch(normalImage,ivec2(gl_FragCoord.xy),0).xyz;
        if(dot(normal,normal)>0.1) {
        color=cel_bands(color,push.cel.y);
        // View-space normal z approximates grazing angle; no geometry expansion or extra draw.
        color*=mix(0.15,1.0,step(push.cel.z,abs(normalize(normal).z)));
    }
        }
    if(push.bloom.x>0.0) {
        ivec2 live=max(ivec2(push.extent.xy)/2,ivec2(1));
        vec2 uv=(gl_FragCoord.xy/push.extent.xy*vec2(live))/vec2(textureSize(bloomImage,0));
        color+=push.bloom.x*texture(bloomImage,clamp(uv,0.5/vec2(textureSize(bloomImage,0)),(vec2(live)-0.5)/vec2(textureSize(bloomImage,0)))).rgb;
    }
    vec3 exposed = max(color * push.whiteBalanceGain.rgb * push.settings.x, vec3(0.0));
    vec3 mapped;
    if (push.settings.z > 0.5) {
        // Bypass: encode only.  Lets the tonemap curve be compared against the
        // raw clipped image without also changing the transfer function, so
        // what the curve alone is doing stays visible.
        mapped = clamp(exposed, 0.0, 1.0);
    } else if (push.settings.y > 1.5) {
        mapped=tonemap_neutral(exposed);
    } else if (push.settings.y > 0.5) {
        mapped = tonemap_reinhard(exposed);
    } else {
        mapped = tonemap_aces(exposed);
    }

    outFragColor = vec4(linear_to_srgb(mapped), 1.0);
}
