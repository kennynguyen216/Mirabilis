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
};

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
