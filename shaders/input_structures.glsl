layout(set = 0, binding = 0) uniform SceneData {
    mat4 view;
    mat4 proj;
    mat4 viewproj;
    vec4 ambientColor;
    vec4 sunlightDirection;
    vec4 sunlightColor;
    vec4 portalClipPlane;
    vec4 portalClipEnabled;
} sceneData;

layout(set = 1, binding = 0) uniform GLTFMaterialData {
    vec4 colorFactors;
    vec4 metal_rough_factors;
    // xy = UV tiling, zw = UV offset. Legacy materials leave this zero and
    // the vertex shaders interpret that as a 1x1 scale.
    vec4 uvTransform;
} materialData;

layout(set = 1, binding = 1) uniform sampler2D colorTex;
layout(set = 1, binding = 2) uniform sampler2D metalRoughTex;
