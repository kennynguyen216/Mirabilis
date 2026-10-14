#pragma once
#include "scene_types.h"
#include <entt/entt.hpp>
#include <map>
#include <optional>
#include <type_traits>

using PersistentID = uint64_t;
struct EntityRef {
    uint64_t epoch{0};
    entt::entity entity{entt::null};
    PersistentID persistentId{UINT64_MAX};
    explicit operator bool() const { return epoch != 0 && entity != entt::null; }
    bool operator==(const EntityRef&) const = default;
};
struct PersistentIdentity { PersistentID id; };
struct CreationOrder { uint64_t index; };
struct Name { std::string value; };
struct Hierarchy { std::optional<PersistentID> parent; std::vector<PersistentID> children; };
struct Visibility { bool visible{true}; RenderLayer layer{RenderLayer::World}; };
struct SceneMeshAsset { SceneAssetKind assetKind{SceneAssetKind::None}; std::string modelPath; };
struct Collider {
    bool hasCollision{false};
    CollisionShape collisionShape{CollisionShape::Box};
    glm::vec3 colliderCenter{0};
    glm::vec3 colliderHalfExtents{0.5f};
};
struct PortalPlaceable {};
struct TimeTrial { TimeTrialRole role{TimeTrialRole::None}; };
struct TransformDrivenExternally {};
struct RuntimeOnly {};
struct PendingDestroy {};
struct UnknownComponents { std::map<std::string, std::string> blocks; };

template<class T> bool valid_component(const T&) { return true; }
bool valid_component(const Transform&);
bool valid_component(const Visibility&);
bool valid_component(const SceneMeshAsset&);
bool valid_component(const SceneMaterial&);
bool valid_component(const Collider&);
bool valid_component(const TimeTrial&);

// Transient authored copies for duplication and play snapshots, never live authority.
struct EntityState {
    PersistentID id;
    uint64_t creationOrder;
    Name name;
    Transform transform;
    Hierarchy hierarchy;
    Visibility visibility;
    SceneMeshAsset mesh;
    SceneMaterial material;
    Collider collider;
    TimeTrial timeTrial;
    bool portalPlaceable{false};
    bool transformDrivenExternally{false};
    UnknownComponents unknown;
};
struct SceneSnapshot {
    std::vector<EntityState> entities;
    std::vector<PersistentID> runtimeIds;
    std::vector<PersistentID> roots;
};
