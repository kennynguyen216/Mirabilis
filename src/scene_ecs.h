#pragma once
#include "scene_components.h"
#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace mirabilis {
class Scene {
public:
    using DestroyCallback = std::function<void(EntityRef)>;
    using CommandAction = std::function<void(Scene&, EntityRef)>;
    void set_script_cleanup_handler(DestroyCallback handler) { _scriptCleanup = std::move(handler); }
    void cleanup_script(EntityRef);
    Scene() = default;
    Scene(Scene&&) noexcept = default;
    Scene& operator=(Scene&&) noexcept = default;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    EntityRef create_object(std::string name, std::optional<EntityRef> parent = std::nullopt, bool owner = false);
    EntityRef import_object(std::string name, PersistentID id);
    EntityRef get(PersistentID id) const;
    EntityRef reference(entt::entity entity) const;
    bool valid(EntityRef ref) const;
    bool pending(EntityRef ref) const;
    bool mutable_ref(EntityRef ref, bool owner = false) const;
    bool runtime_only(EntityRef ref) const;
    EntityRef dynamic_ancestor(EntityRef ref) const;
    bool validate_physics_edit(EntityRef, const RigidBody*, const Collider*, const Transform*) const;
    void set_authored_portal_hosts(std::vector<PersistentID> hosts);
    void set_physics_play(bool play) { _physicsPlay = play; }
    using TeleportHandler = std::function<bool(EntityRef, const Transform&)>;
    void set_teleport_handler(TeleportHandler handler) { _teleportHandler = std::move(handler); }
    uint64_t epoch() const { return _epoch; }
    PersistentID next_persistent_id() const { return _nextPersistentId; }
    bool reserve_persistent_ids(PersistentID next);
    const std::string& last_error() const { return _lastError; }
    const entt::registry& registry() const { return _registry; }

    template<class T> const T* read(EntityRef ref) const {
        if constexpr (std::is_empty_v<T>) {
            static const T tag{};
            return has<T>(ref) ? &tag : nullptr;
        } else return valid(ref) ? _registry.try_get<T>(ref.entity) : nullptr;
    }
    template<class T> bool has(EntityRef ref) const { return valid(ref) && _registry.all_of<T>(ref.entity); }
    template<class T> T* edit(EntityRef ref, bool owner = false) {
        static_assert(!std::is_empty_v<T> && !std::is_same_v<T, Transform> && !std::is_same_v<T, Hierarchy> && !std::is_same_v<T, PersistentIdentity> && !std::is_same_v<T, CreationOrder>, "Use validated setters; identity and order are immutable");
        if (!mutable_ref(ref, owner) || _inDestroyCallback) return nullptr;
        return _registry.try_get<T>(ref.entity);
    }
    template<class T> bool set(EntityRef ref, const T& value, bool owner = false) {
        static_assert(!std::is_same_v<T, Hierarchy> && !std::is_same_v<T, PersistentIdentity> && !std::is_same_v<T, CreationOrder> && !std::is_same_v<T, PendingDestroy>);
        if (!mutable_ref(ref, owner) || !valid_component(value)) return fail("Invalid component mutation");
        if constexpr (std::is_same_v<T, RigidBody>) {
            if (!validate_physics_edit(ref, &value, read<Collider>(ref), read<Transform>(ref))) return false;
        } else if constexpr (std::is_same_v<T, Collider>) {
            if (!validate_physics_edit(ref, read<RigidBody>(ref), &value, read<Transform>(ref))) return false;
        } else if constexpr (std::is_same_v<T, Transform>) {
            if (!validate_physics_edit(ref, read<RigidBody>(ref), read<Collider>(ref), &value)) return false;
            const auto* body = read<RigidBody>(ref);
            if (!owner && _physicsPlay && body && body->motion == BodyMotion::Dynamic) {
                return _teleportHandler ? _teleportHandler(ref, value) : fail("Physics teleport handler unavailable");
            }
        }
        if (_inDestroyCallback) {
            return enqueue_command(ref, [value, owner](Scene& scene, EntityRef target) { scene.set<T>(target, value, owner); });
        }
        if constexpr (std::is_same_v<T, Script>) {
            if (has<Script>(ref)) cleanup_script(ref);
            if (!mutable_ref(ref, owner)) return false;
        }
        _registry.emplace_or_replace<T>(ref.entity, value);
        if constexpr (std::is_same_v<T, Transform> || std::is_same_v<T, Collider> || std::is_same_v<T, RigidBody>) mark_transform_dirty(ref);
        return true;
    }
    template<class T> bool remove(EntityRef ref, bool owner = false) {
        static_assert(!std::is_same_v<T, Transform> && !std::is_same_v<T, Hierarchy> && !std::is_same_v<T, Name> && !std::is_same_v<T, PersistentIdentity> && !std::is_same_v<T, CreationOrder> && !std::is_same_v<T, PendingDestroy>);
        if (!mutable_ref(ref, owner)) return false;
        if constexpr (std::is_same_v<T, Visibility> || std::is_same_v<T, SceneMeshAsset> ||
            std::is_same_v<T, SceneMaterial> || std::is_same_v<T, Collider> ||
            std::is_same_v<T, TimeTrial> || std::is_same_v<T, UnknownComponents>) {
            return fail("Required core components cannot be detached; use their disabled/empty values");
        }
        if (_inDestroyCallback) return enqueue_command(ref, [owner](Scene& scene, EntityRef target) { scene.remove<T>(target, owner); });
        if constexpr (std::is_same_v<T, Script>) {
            if (has<Script>(ref)) cleanup_script(ref);
            if (!mutable_ref(ref, owner)) return false;
        }
        _registry.remove<T>(ref.entity);
        if constexpr (std::is_same_v<T, Collider> || std::is_same_v<T, RigidBody>) mark_transform_dirty(ref);
        return true;
    }

    bool set_parent(EntityRef child, std::optional<EntityRef> parent, bool owner = false);
    bool set_sibling_index(EntityRef ref, size_t index, bool owner = false);
    bool set_local_transform(EntityRef ref, const Transform& transform, bool owner = false);
    bool set_world_transform(EntityRef ref, const glm::mat4& world, bool owner = false);
    glm::mat4 local_matrix(EntityRef ref) const;
    glm::mat4 world_matrix(EntityRef ref) const;
    glm::mat4 world_matrix(PersistentID id) const { return world_matrix(get(id)); }
    const std::vector<PersistentID>& roots() const { return _roots; }
    std::vector<EntityRef> hierarchy_order() const;
    std::vector<EntityRef> creation_order() const;
    EntityRef duplicate_subtree(EntityRef root);

    bool destroy_object(EntityRef ref, bool owner = false);
    size_t drain_destruction(const DestroyCallback& beforeDestroy = {}, size_t cap = 4096);
    bool enqueue_command(EntityRef target, CommandAction action);
    size_t apply_commands();
    size_t queued_command_count() const { return _commands.size(); }
    void clear_queues();
    void mark_transform_dirty(EntityRef ref);
    std::vector<EntityRef> consume_transform_dirty();

    SceneSnapshot take_snapshot() const;
    bool restore_snapshot(const SceneSnapshot&, const DestroyCallback& beforeDestroy = {});
    void replace(Scene&& staging, const DestroyCallback& beforeDestroy = {});
    void clear(const DestroyCallback& beforeDestroy = {});
private:
    entt::registry _registry;
    std::unordered_map<PersistentID, entt::entity> _entities;
    std::vector<PersistentID> _roots;
    uint64_t _epoch{1};
    PersistentID _nextPersistentId{0};
    uint64_t _nextCreationOrder{0};
    std::deque<EntityRef> _destruction;
    struct Command { EntityRef target; CommandAction action; };
    std::deque<Command> _commands;
    std::unordered_set<PersistentID> _dirty;
    bool _inDestroyCallback{false};
    bool _physicsPlay{false};
    TeleportHandler _teleportHandler;
    DestroyCallback _scriptCleanup;
    std::vector<PersistentID> _authoredPortalHosts;
    mutable std::string _lastError;
    bool fail(std::string message) const;
    EntityRef create_with_id(std::string name, PersistentID id);
    EntityState capture_entity(EntityRef ref) const;
    void restore_entity(const EntityState& state);
};
} // namespace mirabilis
AABB collider_from_object(const mirabilis::Scene&, EntityRef);
inline AABB collider_from_object(const mirabilis::Scene& scene, PersistentID id) {
    return collider_from_object(scene, scene.get(id));
}
