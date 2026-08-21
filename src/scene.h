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

// Version 1 supports two collider kinds, both derived from the object's own
// world transform so a moved wall moves its collision with it.
enum class CollisionShape : uint8_t {
    Box,
    GroundPlane,
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
    // Set for objects whose transform is written every frame by another
    // system (physics, portal placement).  The inspector shows them read-only.
    bool transformDrivenExternally{false};

    MeshPrimitive primitive{};
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

// Axis-aligned collider taken from the same world matrix used to render the
// object.  Rotated objects are not supported yet: the box is built from the
// world translation and the world scale, so a rotated wall would collide as
// if it were unrotated.
AABB collider_from_object(const Scene& scene, SceneObjectID id);
