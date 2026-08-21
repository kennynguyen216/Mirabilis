#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <vk_loader.h>
#include <vk_types.h>
#include <world.h>

// Stable handle for a scene object.  Parent/child links use these instead of
// pointers because Scene::objects can reallocate when an object is added.
using SceneObjectID = uint32_t;
constexpr SceneObjectID InvalidSceneObject = UINT32_MAX;

struct Transform {
    glm::vec3 position{0.0f};
    // Euler angles in radians, applied Y * X * Z.  Version 1 of the scene
    // system only rotates renderables; colliders assume axis alignment.
    glm::vec3 rotation{0.0f};
    glm::vec3 scale{1.0f};
};

glm::mat4 to_matrix(const Transform& transform);

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

// Which draw context an object feeds.  The player body must not be drawn for
// the first-person camera, only for the virtual cameras behind portals.
enum class RenderLayer : uint8_t {
    World,
    PortalViewOnly,
};

// A Box has an editable local center and half extents. Ground planes are flat
// horizontal platforms, while SurfRamp is the engine's wedge-shaped slope.
enum class CollisionShape : uint8_t {
    Box,
    GroundPlane,
    SurfRamp,
};

// GPU buffers and material pointers are runtime-only.  This small identity is
// what lets a saved scene reconstruct an object's engine-owned primitive when
// it is loaded in a later session.
enum class SceneAssetKind : uint8_t {
    None,
    FloorQuad,
    UnitCube,
    SurfRamp,
    ImportedGLTF,
};

struct SceneObject {
    SceneObjectID id{InvalidSceneObject};
    std::string name;
    // IDs are used in parent links, selection, and portal host references.
    // Deleted objects therefore keep their vector slot instead of moving
    // every object after them.
    bool alive{true};

    Transform localTransform{};
    SceneObjectID parent{InvalidSceneObject};
    std::vector<SceneObjectID> children;

    bool visible{true};
    bool hasCollision{false};
    bool portalPlaceable{false};
    RenderLayer layer{RenderLayer::World};
    CollisionShape collisionShape{CollisionShape::Box};
    // Local-space box collider data.  The default describes the engine's
    // unit cube, so existing cube walls keep exactly the same collision.
    glm::vec3 colliderCenter{0.0f};
    glm::vec3 colliderHalfExtents{0.5f};
    SceneAssetKind assetKind{SceneAssetKind::None};
    // Set for objects whose transform is written every frame by another
    // system (physics, portal placement).  The inspector shows them read-only.
    bool transformDrivenExternally{false};

    MeshPrimitive primitive{};
    // Relative or absolute source path for an ImportedGLTF. The GPU resource
    // itself is reconstructed through loadGltf when a scene is opened.
    std::string modelPath;
    std::shared_ptr<LoadedGLTF> model;

    bool renderable() const { return primitive.valid() || model != nullptr; }
};

struct Scene {
    std::vector<SceneObject> objects;

    SceneObjectID create_object(
        std::string name,
        SceneObjectID parent = InvalidSceneObject);
    bool destroy_object(SceneObjectID id);
    bool set_parent(SceneObjectID child, SceneObjectID parent);

    SceneObject* get(SceneObjectID id);
    const SceneObject* get(SceneObjectID id) const;

    glm::mat4 local_matrix(SceneObjectID id) const;
    glm::mat4 world_matrix(SceneObjectID id) const;
};

// Axis-aligned broad-phase collider taken from the same world matrix used to
// render the object.  A rotated box becomes the AABB enclosing it, which is
// safe for this solver but not yet exact oriented-box collision.
AABB collider_from_object(const Scene& scene, SceneObjectID id);
