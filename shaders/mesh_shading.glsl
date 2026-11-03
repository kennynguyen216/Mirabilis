// The shared body of the main forward pass.  mesh.frag and mesh_mask.frag are
// both two lines long and differ only in whether they define
// MIRABILIS_ALPHA_MASK before including this, so the alpha-masked variant
// cannot drift away from the opaque one as the shading model grows.
#include "input_structures.glsl"
#include "alpha_mask.glsl"
#include "material_brdf.glsl"
#include "local_lights.glsl"
#include "material_transmission.glsl"
#include "cel_shading.glsl"
#include "reflection_settings.glsl"

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inWorldPosition;
layout(location = 4) in vec4 inCurrentClip;
layout(location = 5) in vec4 inPreviousClip;
layout(location = 6) in vec4 inTangent;
layout(location = 0) out vec4 outFragColor;
// The transparent pass draws into the lit image alone -- additive blending
// must not sum a pane's albedo, motion and direct light into the G-buffer
// behind it -- so the body that pass compiles declares none of these.  A
// fragment shader that writes an output its pass has no attachment for is a
// validation error, not merely a wasted store.
#ifndef MIRABILIS_COLOR_ONLY
layout(location = 1) out vec4 outAlbedo;
layout(location = 2) out vec4 outVelocity;
layout(location = 3) out vec4 outDirectLighting;
layout(location = 4) out vec4 outSpecular;
#endif

void main()
{
    vec4 baseColor = inColor * texture(colorTex, inUV);
#ifdef MIRABILIS_ALPHA_MASK
    // Before any shading work: a discarded fragment must not have cost a
    // lighting evaluation, and must not reach the G-buffer writes below.
    apply_alpha_mask(baseColor.a);
#endif
    if (materialData.materialFlags.y > 0.5) {
        const float cellsAcross = 25.0;
        float checker = mod(
            floor(inUV.x * cellsAcross) + floor(inUV.y * cellsAcross),
            2.0);
        vec3 checkerColor = mix(vec3(0.28), vec3(0.72), checker);

        vec2 cell = abs(fract(inUV * cellsAcross) - 0.5);
        float gridLine = step(0.46, max(cell.x, cell.y));
        baseColor.rgb *= mix(checkerColor, vec3(0.04, 0.10, 0.16), gridLine);
    }
    vec3 geometricNormal = normalize(inNormal);
    vec3 normal = material_shading_normal(geometricNormal, inTangent, inUV);
    vec3 lightingGeometricNormal=geometricNormal;
    if (materialData.materialFlags.z>0.5 && dot(geometricNormal,sceneData.cameraPosition.xyz-inWorldPosition)<0.0) {
        lightingGeometricNormal=-geometricNormal; normal=-normal;
    }
    MaterialSurface surface = material_surface(
        baseColor.rgb, inUV, lightingGeometricNormal, normal, inWorldPosition);
    // Ambient light is deliberately left unshadowed; without it an occluded
    // surface would be pure black rather than merely out of the sun.  What it
    // does get is ambient occlusion, which asks the different question of how
    // much of the surrounding hemisphere nearby geometry blocks.
    float visibility = material_sun_visibility(surface, inWorldPosition, lightingGeometricNormal);
    float occlusion = ambient_occlusion(gl_FragCoord.xy);
    // Diffuse environment has one owner (docs/lumen_lite_design.md 4.2).
    // While SSGI runs it is the trace, which lights a ray from the sky only
    // when the ray demonstrably left the scene, so the diffuse ambient term
    // here is zero. Specular is not SSGI's and is never scaled. Lumen portal
    // cameras use the shared world gather instead of this screen-space pass.
    occlusion*=mix(1.0,texture(occlusionTex,inUV).r,materialData.surfaceFactors.x);
    float ambientScale = sceneData.screenSpaceSettings.z > 0.5 ? 0.0 : 1.0;
    vec3 ambientLight = sceneData.ambientColor.rgb * occlusion * ambientScale;

    // The sun term is left out of occlusion: it already has its own visibility
    // test, and scaling it here would darken contact points in full sunlight.
    vec3 directDiffuse;
    vec3 directSpecular;
    material_sun(surface, visibility, directDiffuse, directSpecular);
    vec3 localDiffuse,localSpecular;
    material_local_lights(surface,inWorldPosition,lightingGeometricNormal,localDiffuse,localSpecular);
    directDiffuse+=localDiffuse;directSpecular+=localSpecular;
    // SSGI supplies diffuse indirect light only, so the specular half of the
    // ambient term is never counted twice.
    vec3 ambientDiffuse;
    vec3 ambientSpecular;
    if (sceneData.iblSettings.x > 0.5) {
        material_ambient_ibl(
            surface, occlusion, ambientScale, ambientDiffuse, ambientSpecular);
    } else {
        material_ambient(surface, ambientLight, ambientDiffuse, ambientSpecular);
    }
    vec3 emission = material_emission(geometricNormal, inWorldPosition);
    vec3 emitterDiffuse;
    vec3 emitterSpecular;
    material_emitters(surface, inWorldPosition, lightingGeometricNormal, gl_FragCoord.xy,
        emitterDiffuse, emitterSpecular);
    #ifndef MIRABILIS_COLOR_ONLY
    if (reflection_roughness_owned(surface.roughness)) {
        ambientSpecular=vec3(0); emitterSpecular=vec3(0);
    }
    #endif
    // While emitters are sampled explicitly, their first bounce is owned here,
    // so emission must not also reach SSGI through the transport target: the
    // path tracer likewise drops emission a BSDF-sampled ray finds after a
    // diffuse scatter (R4.9).
    bool emittersSampled = sceneData.emitterSettings.x > 0.5;

    // R4 ownership instrumentation (src/r4_contributions.h): an owner left
    // out of materialDebug.y adds nothing to the image.  Only this sum is
    // masked; the direct-lighting target below is what SSGI transports, and
    // it has to stay the full frame's.
    uint keep = uint(sceneData.materialDebug.y + 0.5);
    outFragColor = vec4(
        ((keep & 1u) != 0u ? directDiffuse : vec3(0.0)) +
            ((keep & 2u) != 0u ? directSpecular : vec3(0.0)) +
            ((keep & 4u) != 0u ? ambientDiffuse : vec3(0.0)) +
            ((keep & 8u) != 0u ? ambientSpecular : vec3(0.0)) +
            ((keep & 16u) != 0u ? emission : vec3(0.0)) +
            ((keep & 64u) != 0u ? emitterDiffuse : vec3(0.0)) +
            ((keep & 128u) != 0u ? emitterSpecular : vec3(0.0)),
        1.0);
    // R4.15 diagnostic: in coverage mode with only sun_diffuse kept, the
    // shadow-map visibility this fragment was lit with, where it faces the sun.
    // Blended panes add nothing then, so they cannot overwrite the reading of
    // the opaque surface behind them.
    if (sceneData.emitterSettings.w > 0.5 && keep == 1u) {
#ifdef MIRABILIS_COLOR_ONLY
        outFragColor = vec4(0.0);
#else
        outFragColor = vec4(vec3(
            dot(lightingGeometricNormal, normalize(sceneData.sunlightDirection.xyz)) > 0.0
                ? visibility : 0.0), 1.0);
#endif
    }
#ifdef MIRABILIS_COLOR_ONLY
    if(sceneData.emitterSettings.w>0.5 && keep==1u)outFragColor=vec4(0);
    else {
        vec3 reflection=((keep&2u)!=0u?directSpecular:vec3(0))+
            ((keep&8u)!=0u?ambientSpecular:vec3(0))+((keep&128u)!=0u?emitterSpecular:vec3(0))+
            ((keep&16u)!=0u?emission:vec3(0));
        outFragColor=material_transmission(surface,baseColor,outFragColor.rgb,reflection);
        if(materialData.materialFlags.w>0.5 && sceneData.graphicsSettings.y<0.5)
            outFragColor.rgb=cel_bands(outFragColor.rgb,4.0)*mix(0.15,1.0,step(0.18,surface.noV));
    }
#endif
    vec3 debugColor;
    if (material_debug_color(
            surface, lightingGeometricNormal, inTangent,
            directDiffuse, directSpecular, emission, debugColor)) {
        outFragColor = vec4(debugColor, 1.0);
    }
#ifndef MIRABILIS_COLOR_ONLY
    // The SSGI composite multiplies filtered incident light by this.
    outAlbedo = vec4(material_diffuse_albedo(surface)*mix(1.0,texture(occlusionTex,inUV).r,materialData.surfaceFactors.x), materialData.materialFlags.w);
    vec2 currentUV = inCurrentClip.xy / max(inCurrentClip.w, 0.00001) * 0.5 + 0.5;
    vec2 previousUV = inPreviousClip.xy / max(inPreviousClip.w, 0.00001) * 0.5 + 0.5;
    // Add this displacement to a current UV to find the same point in the
    // previous frame.
    vec3 oct=surface.normal/(abs(surface.normal.x)+abs(surface.normal.y)+abs(surface.normal.z));
    vec2 packedNormal=oct.z>=0?oct.xy:(1-abs(oct.yx))*mix(vec2(-1),vec2(1),greaterThanEqual(oct.xy,vec2(0)));
    outVelocity = vec4(previousUV-currentUV,packedNormal);
    outSpecular = vec4(surface.f0,surface.roughness);
    // SSGI reads this as the radiance leaving a surface towards other
    // surfaces.  Specular depends on the direction it leaves in, and only the
    // camera's was evaluated, so it stays out.  Emission leaves in every
    // direction and belongs here unless emitters are sampled explicitly.
    outDirectLighting = vec4(directDiffuse + emitterDiffuse +
        (emittersSampled ? vec3(0.0) : emission), 1.0);
#endif
}
