#pragma once
#include <vk_loader.h>
#include <vk_types.h>

// A GPU primitive owned by the engine (the unit cube and the floor quad).
// Imported assets use SceneObject::model instead.
struct MeshPrimitive {
    uint32_t indexCount{0};
    uint32_t firstIndex{0};
    VkBuffer indexBuffer{VK_NULL_HANDLE};
    VkDeviceAddress vertexBufferAddress{0};
    Bounds bounds{};
    MaterialInstance* material{nullptr};

    bool valid() const
    {
        return indexBuffer != VK_NULL_HANDLE && material != nullptr;
    }
};


struct SceneRenderRuntime {
    MeshPrimitive primitive{};
    std::shared_ptr<LoadedGLTF> model;
};
