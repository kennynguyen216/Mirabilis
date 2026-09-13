#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <span>
#include <array>
#include <functional>
#include <deque>

#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>
#include <vk_mem_alloc.h>
#include <fmt/core.h>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

struct MaterialPipeline;
struct MaterialInstance;
struct DrawContext;



#define VK_CHECK(x)                                                        \
    do {                                                                   \
        VkResult err = x;                                                  \
        if (err) {                                                         \
            fmt::print("Detected Vulkan error: {}", string_VkResult(err)); \
            abort();                                                       \
        }                                                                  \
    } while (0)

struct TraceTextureSource {
    uint32_t width{},height{};
    std::vector<uint32_t> rgba;
};
struct AllocatedImage{
    VkImage image;
    VkImageView imageView;
    VmaAllocation allocation;
    VkExtent3D imageExtent;
    VkFormat imageFormat;
    std::shared_ptr<TraceTextureSource> traceSource;
};

struct AllocatedBuffer {
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
};

struct Vertex {
    glm::vec3 position;
    float uv_x;
    glm::vec3 normal;
    float uv_y;
    glm::vec4 color;
};

struct TraceMeshSource {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

struct GPUMeshBuffers {
    std::shared_ptr<TraceMeshSource> traceSource;
    AllocatedBuffer indexBuffer;
    AllocatedBuffer vertexBuffer;
    VkDeviceAddress vertexBufferAddress{};
};

struct GPUDrawPushConstants {
    glm::mat4 worldMatrix;
    VkDeviceAddress vertexBuffer;
    // Three rows of the previous affine object transform. Together with the
    // implicit final row (0,0,0,1), this fits the guaranteed 128-byte Vulkan
    // push-constant budget while still supporting object motion vectors.
    uint64_t alignmentPadding{};
    glm::vec4 previousWorldRow0{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 previousWorldRow1{0.0f, 1.0f, 0.0f, 0.0f};
    glm::vec4 previousWorldRow2{0.0f, 0.0f, 1.0f, 0.0f};
};
static_assert(sizeof(GPUDrawPushConstants) == 128);

enum class MaterialPass : uint8_t {
    MainColor,
    Transparent,
    // glTF alphaMode MASK: fully opaque wherever it draws at all, but the
    // base colour's alpha decides per fragment whether it draws.  It is a
    // separate pass from Transparent because it still writes depth and still
    // belongs in the prepass, which blended surfaces do not.
    Mask,
    Other
};

struct MaterialPipeline {
    VkPipeline pipeline{};
    VkPipelineLayout layout{};
};

struct MaterialInstance {
    std::shared_ptr<TraceTextureSource> traceTexture;
    glm::vec2 traceUVScale{1};
    glm::vec4 traceBaseColor{1.0f};
    glm::vec4 traceParameters{0.0f,0.8f,0.0f,1.5f};
    glm::vec4 traceEmission{0.0f};
    MaterialPipeline* pipeline{};
    VkDescriptorSet materialSet{};
    MaterialPass passType{MaterialPass::MainColor};
};

struct GPUSceneData {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 viewproj;
    glm::mat4 previousViewProjection;
    glm::vec4 ambientColor;
    glm::vec4 sunlightDirection;
    glm::vec4 sunlightColor;
    // Used only by the portal-view material pipeline.  The ordinary scene
    // leaves portalClipEnabled at zero.
    glm::vec4 portalClipPlane;
    glm::vec4 portalClipEnabled;
    // World space -> sunlight clip space.  Every camera in the frame shares
    // one shadow map, so portal views need no separate shadow pass.
    glm::mat4 sunViewProjection{1.0f};
    // x = constant depth bias, y = world-space normal offset,
    // z = one shadow-map texel in UV, w = 0 disables shadowing.
    glm::vec4 shadowSettings{0.0f};
    // Screen-space passes are given a depth buffer, not a position per
    // fragment, so they rebuild the position these undo.  Kept here rather
    // than recomputed per pass because every camera in the frame, portal
    // cameras included, already fills this block once.
    glm::mat4 inverseProjection{1.0f};
    glm::mat4 inverseViewProjection{1.0f};
    // x = 1 while the ambient-occlusion image describes this camera's view.
    //     Portal cameras are bound the same image but see different geometry,
    //     so they set 0 and shade with unoccluded ambient light instead.
    // y = 1 suppresses the sunlight term, leaving ambient only.  It exists to
    //     make occlusion visible on its own while tuning it.
    glm::vec4 screenSpaceSettings{0.0f};
    // xy = the occlusion texture coordinate that one screen pixel advances
    //      by, zw = the largest coordinate the rendered region reaches.  The
    //      occlusion image is half resolution and stays allocated at window
    //      size while the render scale moves the live region inside it, so a
    //      fragment cannot simply divide by the render extent.
    glm::vec4 ambientOcclusionUV{0.0f};
    // x = reference-compatible environment intensity, y = 1 for black.
    glm::vec4 ssgiFallbackSettings{1.0f, 0.0f, 0.0f, 0.0f};
    // x = PCF footprint radius in shadow-map texels.
    glm::vec4 shadowFilterSettings{3.0f, 0.0f, 0.0f, 0.0f};
    // How the flat ambient term and the screen-space indirect estimate divide
    // the same job, shared by the forward shader and the SSGI trace.
    // x = the fraction of ambient that survives while SSGI is enabled.  At 0
    //     SSGI replaces ambient outright, which is only honest once ray
    //     misses are filled from the real environment; at 1 SSGI adds on top
    //     of it, which double-counts sky fill but can never darken a region.
    // y = 1 fills SSGI ray misses from the environment map, 0 uses the
    //     analytic gradient the software path tracer also uses.  The gradient
    //     is kept so a reference comparison can light both sides the same.
    // z = the mip level to sample the environment at.  A cosine-weighted ray
    //     represents a wide cone, not a texel of a 4K panorama.
    // w = the radiance ceiling a miss ray may return.  A coarse mip alone is
    //     not enough: a clear-sky panorama keeps most of its energy in a sun
    //     disk thousands of times brighter than the sky, which survives every
    //     mip, and which the shadow-mapped direct term already delivers.
    glm::vec4 indirectSettings{0.0f, 1.0f, 0.0f, 1.0e4f};
};

// Sixteen visible surfaces (the player pair plus seven authored links), with
// three virtual-camera levels per surface for recursive portal views.
constexpr uint32_t MaxPortalSurfaces = 16;
constexpr uint32_t PortalRecursionDepth = 3;
constexpr uint32_t PortalViewCount = MaxPortalSurfaces * PortalRecursionDepth;
constexpr uint32_t BluePortalView = 0;
constexpr uint32_t OrangePortalView = 1;

struct Bounds {
    glm::vec3 origin{0.0f};
    float sphereRadius{0.0f};
    glm::vec3 extents{0.0f};
};

class IRenderable {
public:
    virtual ~IRenderable() = default;
    virtual void Draw(const glm::mat4& topMatrix, DrawContext& ctx) = 0;
};

struct RenderObject {
    uint32_t indexCount{};
    uint32_t firstIndex{};
    VkBuffer indexBuffer{};
    MaterialInstance* material{};
    Bounds bounds{};
    glm::mat4 transform{1.0f};
    glm::mat4 previousTransform{1.0f};
    VkDeviceAddress vertexBufferAddress{};
};

struct DrawContext {
    std::vector<RenderObject> OpaqueSurfaces;
    std::vector<RenderObject> TransparentSurfaces;
};

struct Node : public IRenderable {
    std::weak_ptr<Node> parent;
    std::vector<std::shared_ptr<Node>> children;
    glm::mat4 localTransform{1.0f};
    glm::mat4 worldTransform{1.0f};
    glm::mat4 previousDrawTransform{1.0f};
    bool hasPreviousDrawTransform{false};

    void refreshTransform(const glm::mat4& parentMatrix)
    {
        worldTransform = parentMatrix * localTransform;
        for (auto& child : children) {
            child->refreshTransform(worldTransform);
        }
    }

    void Draw(const glm::mat4& topMatrix, DrawContext& ctx) override
    {
        for (auto& child : children) {
            child->Draw(topMatrix, ctx);
        }
    }
};
