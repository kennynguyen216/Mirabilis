#pragma once
#include "scene_types.h"

using SceneObjectID = uint32_t;
constexpr SceneObjectID InvalidSceneObject = UINT32_MAX;

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
    TimeTrialRole timeTrialRole{TimeTrialRole::None};
    SceneMaterial material{};
    // Set for objects whose transform is written every frame by another
    // system (physics, portal placement).  The inspector shows them read-only.
    bool transformDrivenExternally{false};

    // Relative or absolute source path for an ImportedGLTF. The GPU resource
    // itself is reconstructed through loadGltf when a scene is opened.
    std::string modelPath;

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
