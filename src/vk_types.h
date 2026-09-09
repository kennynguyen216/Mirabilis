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

struct AllocatedImage{
    VkImage image;
    VkImageView imageView;
    VmaAllocation allocation;
    VkExtent3D imageExtent;
    VkFormat imageFormat;
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

struct GPUMeshBuffers {
    AllocatedBuffer indexBuffer;
    AllocatedBuffer vertexBuffer;
    VkDeviceAddress vertexBufferAddress{};
};

struct GPUDrawPushConstants {
    glm::mat4 worldMatrix;
    VkDeviceAddress vertexBuffer;
};

enum class MaterialPass : uint8_t {
    MainColor,
    Transparent,
    Other
};

struct MaterialPipeline {
    VkPipeline pipeline{};
    VkPipelineLayout layout{};
};

struct MaterialInstance {
    MaterialPipeline* pipeline{};
    VkDescriptorSet materialSet{};
    MaterialPass passType{MaterialPass::MainColor};
};

struct GPUSceneData {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 viewproj;
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
