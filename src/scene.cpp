#include "scene.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <glm/gtc/matrix_inverse.hpp>

namespace {
template<class V> bool finite_vector(const V& value) {
    for (glm::length_t i = 0; i < value.length(); ++i) if (!std::isfinite(value[i])) return false;
    return true;
}
bool finite_matrix(const glm::mat4& value) {
    for (int i = 0; i < 4; ++i) if (!finite_vector(value[i])) return false;
    return true;
}
bool collider_subtree_valid(const mirabilis::Scene& scene, EntityRef root,
    std::optional<EntityRef> parent, const Transform* transform, const Collider* collider) {
    bool nonzero = true;
    for (auto ancestor = parent; ancestor;) {
        nonzero &= glm::all(glm::notEqual(scene.read<Transform>(*ancestor)->scale,glm::vec3(0)));
        const auto id = scene.read<Hierarchy>(*ancestor)->parent;
        ancestor = id ? std::optional(scene.get(*id)) : std::nullopt;
    }
    struct Visit { EntityRef ref; glm::mat4 parentWorld; bool nonzero; };
    std::vector<Visit> stack{{root,parent ? scene.world_matrix(*parent) : glm::mat4(1),nonzero}};
    while (!stack.empty()) {
        const auto visit = stack.back(); stack.pop_back();
        const auto& local = visit.ref == root && transform ? *transform : *scene.read<Transform>(visit.ref);
        const auto world = visit.parentWorld * to_matrix(local);
        const bool validScale = visit.nonzero && glm::all(glm::notEqual(local.scale,glm::vec3(0)));
        const auto* shape = visit.ref == root ? collider : scene.read<Collider>(visit.ref);
        if (shape && shape->hasCollision) {
            if (!validScale || !finite_matrix(world)) return false;
            for (int column=0; column<3; ++column)
                if (glm::length(glm::dvec3(world[column])) == 0) return false;
        }
        for (const auto id : scene.read<Hierarchy>(visit.ref)->children)
            stack.push_back({scene.get(id),world,validScale});
    }
    return true;
}
}
bool valid_component(const Transform& value) {
    return finite_vector(value.position) && finite_vector(value.rotation) && finite_vector(value.scale);
}
bool valid_component(const Visibility& value) { return value.layer <= RenderLayer::PortalViewOnly; }
bool valid_component(const SceneMeshAsset& value) {
    return value.assetKind <= SceneAssetKind::ImportedGLTF &&
        (value.assetKind != SceneAssetKind::ImportedGLTF || !value.modelPath.empty());
}
bool valid_component(const SceneMaterial& value) {
    return finite_vector(value.colorTint) && finite_vector(value.uvScale) &&
        value.uvScale.x > 0 && value.uvScale.y > 0 &&
        finite_vector(value.emissionColor) && glm::all(glm::greaterThanEqual(value.emissionColor, glm::vec3(0))) &&
        std::isfinite(value.emissionStrength) && value.emissionStrength >= 0 &&
        std::isfinite(value.metallic) && value.metallic >= 0 && value.metallic <= 1 &&
        std::isfinite(value.roughness) && value.roughness >= 0 && value.roughness <= 1 &&
        std::isfinite(value.transmission) && value.transmission >= 0 && value.transmission <= 1 &&
        std::isfinite(value.ior) && value.ior >= 1 && value.ior <= 3;
}
bool valid_component(const Collider& value) {
    return value.collisionShape <= CollisionShape::SurfRamp && finite_vector(value.colliderCenter) &&
        finite_vector(value.colliderHalfExtents) && glm::all(glm::greaterThan(value.colliderHalfExtents, glm::vec3(0)));
}
bool valid_component(const TimeTrial& value) { return value.role <= TimeTrialRole::FinishTrigger; }

bool valid_component(const RigidBody& value) {
    return value.motion <= BodyMotion::Dynamic && std::isfinite(value.mass) &&
        (value.motion != BodyMotion::Dynamic || value.mass > 0) &&
        std::isfinite(value.friction) && value.friction >= 0 &&
        std::isfinite(value.restitution) && value.restitution >= 0 && value.restitution <= 1 &&
        std::isfinite(value.gravityFactor);
}

namespace mirabilis {
bool Scene::fail(std::string message) const {
    _lastError = std::move(message);
    std::cerr << "Scene: " << _lastError << '\n';
    return false;
}
bool Scene::valid(EntityRef ref) const {
    if (ref.epoch != _epoch || !_registry.valid(ref.entity)) return false;
    const auto* identity = _registry.try_get<PersistentIdentity>(ref.entity);
    return identity && identity->id == ref.persistentId;
}
EntityRef Scene::get(PersistentID id) const {
    const auto it = _entities.find(id);
    return it == _entities.end() ? EntityRef{} : EntityRef{_epoch, it->second, id};
}
EntityRef Scene::reference(entt::entity entity) const {
    if (!_registry.valid(entity)) return {};
    const auto* identity = _registry.try_get<PersistentIdentity>(entity);
    return identity ? EntityRef{_epoch, entity, identity->id} : EntityRef{};
}
bool Scene::pending(EntityRef ref) const { return has<PendingDestroy>(ref); }
bool Scene::runtime_only(EntityRef ref) const {
    while (valid(ref)) {
        if (has<RuntimeOnly>(ref)) return true;
        const auto* hierarchy = read<Hierarchy>(ref);
        if (!hierarchy || !hierarchy->parent) break;
        ref = get(*hierarchy->parent);
    }
    return false;
}
EntityRef Scene::dynamic_ancestor(EntityRef ref) const {
    while (valid(ref)) {
        if (const auto* body = read<RigidBody>(ref); body && body->motion == BodyMotion::Dynamic) return ref;
        const auto* hierarchy = read<Hierarchy>(ref);
        if (!hierarchy || !hierarchy->parent) break;
        ref = get(*hierarchy->parent);
    }
    return {};
}
void Scene::set_authored_portal_hosts(std::vector<PersistentID> hosts) { _authoredPortalHosts = std::move(hosts); }
bool Scene::validate_physics_edit(EntityRef ref, const RigidBody* body, const Collider* collider, const Transform* transform) const {
    if (body && !valid_component(*body)) return fail("Invalid rigid-body settings");
    const bool dynamic = body && body->motion == BodyMotion::Dynamic;
    if (dynamic) {
        if (read<Hierarchy>(ref)->parent) return fail("Dynamic bodies must be root-level");
        if (!collider || !collider->hasCollision || collider->collisionShape != CollisionShape::Box)
            return fail("Dynamic bodies require an enabled Box collider");
        for (const auto hostId : _authoredPortalHosts) {
            auto host = get(hostId);
            while (valid(host)) {
                if (host.persistentId == ref.persistentId) return fail("Authored portal hosts cannot enter a dynamic subtree");
                const auto& hierarchy = *read<Hierarchy>(host);
                if (!hierarchy.parent) break;
                host = get(*hierarchy.parent);
            }
        }
    }
    const auto parentId = read<Hierarchy>(ref)->parent;
    if (!collider_subtree_valid(*this,ref,parentId ? std::optional(get(*parentId)) : std::nullopt,transform,collider))
        return fail("Collider subtree requires finite world transforms and nonzero ancestor scale");
    return true;
}
bool Scene::mutable_ref(EntityRef ref, bool owner) const {
    if (!valid(ref)) return fail("Stale entity reference");
    if (pending(ref)) return fail("Entity is pending destruction");
    if (!owner && runtime_only(ref)) return fail("Entity is owned by a native system");
    return true;
}
bool Scene::reserve_persistent_ids(PersistentID next) {
    if (next == UINT64_MAX) return fail("Persistent ID allocator exhausted");
    _nextPersistentId = std::max(_nextPersistentId, next);
    return true;
}
EntityRef Scene::create_with_id(std::string name, PersistentID id) {
    if (id == UINT64_MAX || _entities.contains(id)) {
        fail("Duplicate or exhausted persistent ID"); return {};
    }
    const auto entity = _registry.create();
    _registry.emplace<PersistentIdentity>(entity, id);
    _registry.emplace<CreationOrder>(entity, _nextCreationOrder++);
    _registry.emplace<Name>(entity, std::move(name));
    _registry.emplace<Transform>(entity);
    _registry.emplace<Hierarchy>(entity);
    _registry.emplace<Visibility>(entity);
    _registry.emplace<SceneMeshAsset>(entity);
    _registry.emplace<SceneMaterial>(entity);
    _registry.emplace<Collider>(entity);
    _registry.emplace<TimeTrial>(entity);
    _registry.emplace<UnknownComponents>(entity);
    _entities.emplace(id, entity);
    _roots.push_back(id);
    _nextPersistentId = std::max(_nextPersistentId, id + 1);
    const auto ref = get(id);
    mark_transform_dirty(ref);
    return ref;
}
EntityRef Scene::create_object(std::string name, std::optional<EntityRef> parent, bool owner) {
    if (parent && !mutable_ref(*parent, owner)) return {};
    if (_nextPersistentId == UINT64_MAX) { fail("Persistent ID allocator exhausted"); return {}; }
    const auto ref = create_with_id(std::move(name), _nextPersistentId);
    if (ref && parent) set_parent(ref, parent, owner);
    return ref;
}
EntityRef Scene::import_object(std::string name, PersistentID id) { return create_with_id(std::move(name), id); }
std::vector<EntityRef> Scene::creation_order() const {
    std::vector<EntityRef> result;
    result.reserve(_entities.size());
    for (const auto entity : _registry.view<PersistentIdentity, CreationOrder>()) result.push_back(reference(entity));
    std::sort(result.begin(), result.end(), [&](EntityRef a, EntityRef b) {
        return read<CreationOrder>(a)->index < read<CreationOrder>(b)->index;
    });
    return result;
}
std::vector<EntityRef> Scene::hierarchy_order() const {
    std::vector<EntityRef> result;
    result.reserve(_entities.size());
    std::vector<PersistentID> stack(_roots.rbegin(), _roots.rend());
    while (!stack.empty()) {
        const auto ref = get(stack.back()); stack.pop_back();
        if (!valid(ref)) continue;
        result.push_back(ref);
        const auto& children = read<Hierarchy>(ref)->children;
        stack.insert(stack.end(), children.rbegin(), children.rend());
    }
    return result;
}
bool Scene::set_parent(EntityRef child, std::optional<EntityRef> parent, bool owner) {
    if (!mutable_ref(child, owner) || (parent && !mutable_ref(*parent, owner))) return false;
    if (parent && read<RigidBody>(child) && read<RigidBody>(child)->motion == BodyMotion::Dynamic)
        return fail("Dynamic bodies must be root-level");
    if (parent && dynamic_ancestor(*parent)) {
        for (const auto hostId : _authoredPortalHosts) {
            auto host = get(hostId);
            while (valid(host)) {
                if (host.persistentId == child.persistentId) return fail("Authored portal hosts cannot enter a dynamic subtree");
                const auto& hierarchy = *read<Hierarchy>(host);
                if (!hierarchy.parent) break;
                host = get(*hierarchy.parent);
            }
        }
    }
    for (auto ancestor = parent; ancestor;) {
        if (ancestor->persistentId == child.persistentId) return fail("Hierarchy cycle rejected");
        const auto& hierarchy = *read<Hierarchy>(*ancestor);
        ancestor = hierarchy.parent ? std::optional(get(*hierarchy.parent)) : std::nullopt;
    }
    if (!collider_subtree_valid(*this,child,parent,read<Transform>(child),read<Collider>(child)))
        return fail("Collider subtree requires finite world transforms and nonzero ancestor scale");
    if (_inDestroyCallback) return enqueue_command(child, [parent, owner](Scene& scene, EntityRef target) { scene.set_parent(target, parent, owner); });
    auto& hierarchy = _registry.get<Hierarchy>(child.entity);
    if (hierarchy.parent) {
        if (const auto old = get(*hierarchy.parent); valid(old)) std::erase(_registry.get<Hierarchy>(old.entity).children, child.persistentId);
    } else std::erase(_roots, child.persistentId);
    hierarchy.parent = parent ? std::optional(parent->persistentId) : std::nullopt;
    if (parent) _registry.get<Hierarchy>(parent->entity).children.push_back(child.persistentId);
    else _roots.push_back(child.persistentId);
    mark_transform_dirty(child);
    return true;
}
bool Scene::set_sibling_index(EntityRef ref, size_t index, bool owner) {
    if (!mutable_ref(ref, owner)) return false;
    if (_inDestroyCallback) return enqueue_command(ref, [index, owner](Scene& scene, EntityRef target) { scene.set_sibling_index(target, index, owner); });
    const auto& hierarchy = *read<Hierarchy>(ref);
    auto& siblings = hierarchy.parent ? _registry.get<Hierarchy>(get(*hierarchy.parent).entity).children : _roots;
    std::erase(siblings, ref.persistentId);
    siblings.insert(siblings.begin() + std::min(index, siblings.size()), ref.persistentId);
    return true;
}
bool Scene::set_local_transform(EntityRef ref, const Transform& transform, bool owner) { return set<Transform>(ref, transform, owner); }
bool Scene::set_world_transform(EntityRef ref, const glm::mat4& world, bool owner) {
    if (!mutable_ref(ref, owner) || !finite_matrix(world)) return fail("Invalid world transform");
    if (_inDestroyCallback) return enqueue_command(ref, [world, owner](Scene& scene, EntityRef target) {
        scene.set_world_transform(target, world, owner);
    });
    const auto& hierarchy = *read<Hierarchy>(ref);
    const auto parentWorld = hierarchy.parent ? world_matrix(*hierarchy.parent) : glm::mat4(1);
    const auto local = glm::inverse(parentWorld) * world;
    if (!finite_matrix(local)) return fail("Parent transform is not invertible");
    return set_local_transform(ref, transform_from_matrix(local), owner);
}
glm::mat4 Scene::local_matrix(EntityRef ref) const {
    const auto* transform = read<Transform>(ref);
    if (!transform) { fail("Stale transform reference"); return glm::mat4(1); }
    return to_matrix(*transform);
}
glm::mat4 Scene::world_matrix(EntityRef ref) const {
    if (!valid(ref)) { fail("Stale transform reference"); return glm::mat4(1); }
    glm::mat4 world(1);
    while (valid(ref)) {
        world = local_matrix(ref) * world;
        const auto& hierarchy = *read<Hierarchy>(ref);
        if (!hierarchy.parent) break;
        ref = get(*hierarchy.parent);
    }
    return world;
}
void Scene::mark_transform_dirty(EntityRef ref) {
    if (!valid(ref)) return;
    std::vector<PersistentID> stack{ref.persistentId};
    while (!stack.empty()) {
        const auto target = get(stack.back()); stack.pop_back();
        if (!valid(target)) continue;
        _dirty.insert(target.persistentId);
        const auto& children = read<Hierarchy>(target)->children;
        stack.insert(stack.end(), children.begin(), children.end());
    }
}
std::vector<EntityRef> Scene::consume_transform_dirty() {
    std::vector<EntityRef> result;
    for (const auto ref : hierarchy_order()) if (_dirty.contains(ref.persistentId)) result.push_back(ref);
    _dirty.clear();
    return result;
}
bool Scene::destroy_object(EntityRef ref, bool owner) {
    if (!valid(ref)) return fail("Stale entity reference");
    if (pending(ref)) return true;
    std::vector<EntityRef> subtree;
    std::vector<PersistentID> stack{ref.persistentId};
    while (!stack.empty()) {
        const auto target = get(stack.back()); stack.pop_back();
        if (!valid(target)) continue;
        if (!owner && runtime_only(target)) return fail("Cannot destroy a native-owned subtree");
        subtree.push_back(target);
        const auto& children = read<Hierarchy>(target)->children;
        stack.insert(stack.end(), children.begin(), children.end());
    }
    for (auto it = subtree.rbegin(); it != subtree.rend(); ++it) {
        if (!pending(*it)) {
            _registry.emplace<PendingDestroy>(it->entity);
            _destruction.push_back(*it);
        }
    }
    std::erase_if(_commands, [&](const Command& command) { return !valid(command.target) || pending(command.target); });
    return true;
}
size_t Scene::drain_destruction(const DestroyCallback& beforeDestroy, size_t cap) {
    size_t count = 0;
    while (!_destruction.empty() && count < cap) {
        const auto ref = _destruction.front(); _destruction.pop_front();
        if (!valid(ref) || !pending(ref)) continue;
        if (beforeDestroy) {
            const bool previous = _inDestroyCallback; _inDestroyCallback = true;
            try { beforeDestroy(ref); }
            catch (const std::exception& error) { fail(std::string("Destruction callback: ") + error.what()); }
            catch (...) { fail("Destruction callback failed"); }
            _inDestroyCallback = previous;
        }
        const auto hierarchy = _registry.get<Hierarchy>(ref.entity);
        if (hierarchy.parent) {
            if (const auto parent = get(*hierarchy.parent); valid(parent)) std::erase(_registry.get<Hierarchy>(parent.entity).children, ref.persistentId);
        } else std::erase(_roots, ref.persistentId);
        _dirty.erase(ref.persistentId);
        _entities.erase(ref.persistentId);
        _registry.destroy(ref.entity);
        ++count;
    }
    if (!_destruction.empty()) fail("Destruction drain cap reached; pending work retained");
    return count;
}
bool Scene::enqueue_command(EntityRef target, CommandAction action) {
    if (!valid(target) || pending(target) || !action) return fail("Cannot queue a stale or pending target");
    _commands.push_back({target, std::move(action)});
    return true;
}
size_t Scene::apply_commands() {
    auto batch = std::move(_commands);
    _commands.clear();
    size_t applied = 0;
    for (auto& command : batch) {
        if (valid(command.target) && !pending(command.target)) {
            command.action(*this, command.target); ++applied;
        }
    }
    return applied;
}
void Scene::clear_queues() {
    _commands.clear(); _destruction.clear();
    _registry.clear<PendingDestroy>();
}
EntityState Scene::capture_entity(EntityRef ref) const {
    return {ref.persistentId, read<CreationOrder>(ref)->index, *read<Name>(ref), *read<Transform>(ref),
        *read<Hierarchy>(ref), read<Visibility>(ref) ? *read<Visibility>(ref) : Visibility{},
        read<SceneMeshAsset>(ref) ? *read<SceneMeshAsset>(ref) : SceneMeshAsset{},
        read<SceneMaterial>(ref) ? *read<SceneMaterial>(ref) : SceneMaterial{},
        read<Collider>(ref) ? *read<Collider>(ref) : Collider{},
        read<TimeTrial>(ref) ? *read<TimeTrial>(ref) : TimeTrial{},
        has<PortalPlaceable>(ref), has<TransformDrivenExternally>(ref),
        read<UnknownComponents>(ref) ? *read<UnknownComponents>(ref) : UnknownComponents{},
        read<RigidBody>(ref) ? std::optional(*read<RigidBody>(ref)) : std::nullopt};
}
void Scene::restore_entity(const EntityState& state) {
    auto ref = get(state.id);
    if (!valid(ref)) ref = import_object(state.name.value, state.id);
    _registry.emplace_or_replace<CreationOrder>(ref.entity, state.creationOrder);
    _registry.emplace_or_replace<Name>(ref.entity, state.name);
    _registry.emplace_or_replace<Transform>(ref.entity, state.transform);
    _registry.emplace_or_replace<Hierarchy>(ref.entity, state.hierarchy);
    _registry.emplace_or_replace<Visibility>(ref.entity, state.visibility);
    _registry.emplace_or_replace<SceneMeshAsset>(ref.entity, state.mesh);
    _registry.emplace_or_replace<SceneMaterial>(ref.entity, state.material);
    _registry.emplace_or_replace<Collider>(ref.entity, state.collider);
    _registry.emplace_or_replace<TimeTrial>(ref.entity, state.timeTrial);
    _registry.emplace_or_replace<UnknownComponents>(ref.entity, state.unknown);
    if (state.rigidBody) _registry.emplace_or_replace<RigidBody>(ref.entity, *state.rigidBody);
    else _registry.remove<RigidBody>(ref.entity);
    _registry.remove<PhysicsState>(ref.entity);
    if (state.portalPlaceable) _registry.emplace_or_replace<PortalPlaceable>(ref.entity);
    else _registry.remove<PortalPlaceable>(ref.entity);
    if (state.transformDrivenExternally) _registry.emplace_or_replace<TransformDrivenExternally>(ref.entity);
    else _registry.remove<TransformDrivenExternally>(ref.entity);
    _nextCreationOrder = std::max(_nextCreationOrder, state.creationOrder + 1);
    _nextPersistentId = std::max(_nextPersistentId, state.id + 1);
}
SceneSnapshot Scene::take_snapshot() const {
    SceneSnapshot snapshot;
    snapshot.roots = _roots;
    for (const auto ref : creation_order()) {
        if (runtime_only(ref)) snapshot.runtimeIds.push_back(ref.persistentId);
        else snapshot.entities.push_back(capture_entity(ref));
    }
    return snapshot;
}
EntityRef Scene::duplicate_subtree(EntityRef root) {
    if (!mutable_ref(root) || runtime_only(root)) return {};
    std::vector<EntityState> states;
    std::vector<PersistentID> stack{root.persistentId};
    while (!stack.empty()) {
        const auto ref = get(stack.back()); stack.pop_back();
        if (!valid(ref) || runtime_only(ref) || pending(ref)) continue;
        states.push_back(capture_entity(ref));
        const auto& children = read<Hierarchy>(ref)->children;
        stack.insert(stack.end(), children.rbegin(), children.rend());
    }
    if (states.empty() || states.size() > UINT64_MAX - _nextPersistentId) { fail("Persistent ID allocator exhausted"); return {}; }
    std::unordered_map<PersistentID, PersistentID> copies;
    for (const auto& state : states) {
        const auto ref = create_object(state.name.value);
        if (!ref) return {};
        copies.emplace(state.id, ref.persistentId);
    }
    for (auto state : states) {
        const auto original = state.id;
        state.id = copies.at(original);
        state.creationOrder = read<CreationOrder>(get(state.id))->index;
        if (state.hierarchy.parent && copies.contains(*state.hierarchy.parent)) state.hierarchy.parent = copies.at(*state.hierarchy.parent);
        state.hierarchy.children.clear();
        for (const auto child : read<Hierarchy>(get(original))->children) if (copies.contains(child)) state.hierarchy.children.push_back(copies.at(child));
        restore_entity(state);
        std::erase(_roots, state.id);
    }
    const auto duplicate = get(copies.at(root.persistentId));
    const auto& hierarchy = *read<Hierarchy>(duplicate);
    if (hierarchy.parent) _registry.get<Hierarchy>(get(*hierarchy.parent).entity).children.push_back(duplicate.persistentId);
    else _roots.push_back(duplicate.persistentId);
    mark_transform_dirty(duplicate);
    return duplicate;
}
bool Scene::restore_snapshot(const SceneSnapshot& snapshot, const DestroyCallback& beforeDestroy) {
    std::unordered_set<PersistentID> saved;
    for (const auto& state : snapshot.entities) {
        if (state.id == UINT64_MAX || !saved.insert(state.id).second ||
            !valid_component(state.transform) || !valid_component(state.mesh) || !valid_component(state.material) || !valid_component(state.collider) || (state.rigidBody && !valid_component(*state.rigidBody))) {
            return fail("Invalid scene snapshot");
        }
    }
    std::unordered_set<PersistentID> keep = saved;
    keep.insert(snapshot.runtimeIds.begin(), snapshot.runtimeIds.end());
    // Synchronous restoration must finish the finite scene, independent of the per-frame cap.
    drain_destruction(beforeDestroy, std::max<size_t>(4096, _entities.size()));
    // Remove play-created entities individually: a surviving authored child may have moved under one.
    const auto removalOrder = hierarchy_order();
    for (auto it = removalOrder.rbegin(); it != removalOrder.rend(); ++it) if (!keep.contains(it->persistentId)) {
        const auto ref = *it;
        _registry.emplace_or_replace<PendingDestroy>(ref.entity);
        _destruction.push_back(ref);
    }
    drain_destruction(beforeDestroy, std::max<size_t>(4096, _entities.size()));
    if (!_destruction.empty()) return fail("Snapshot teardown still has pending work");
    clear_queues();
    if (_epoch == UINT64_MAX) return fail("Scene epoch exhausted");
    ++_epoch;
    for (const auto& state : snapshot.entities) restore_entity(state);
    _roots.clear();
    for (const auto id : snapshot.roots) if (valid(get(id))) _roots.push_back(id);
    // Remove absent runtime links in scratch scenes and rebind surviving runtime children.
    for (const auto ref : creation_order()) {
        auto& hierarchy = _registry.get<Hierarchy>(ref.entity);
        std::erase_if(hierarchy.children, [&](PersistentID id) { return !valid(get(id)); });
        if (hierarchy.parent && !valid(get(*hierarchy.parent))) hierarchy.parent.reset();
        if (!hierarchy.parent && std::find(_roots.begin(), _roots.end(), ref.persistentId) == _roots.end()) _roots.push_back(ref.persistentId);
    }
    for (const auto ref : hierarchy_order()) mark_transform_dirty(ref);
    return true;
}
void Scene::clear(const DestroyCallback& beforeDestroy) {
    const auto removalOrder = hierarchy_order();
    for (auto it = removalOrder.rbegin(); it != removalOrder.rend(); ++it) if (!pending(*it)) {
        const auto ref = *it;
        _registry.emplace<PendingDestroy>(ref.entity); _destruction.push_back(ref);
    }
    // Budget every existing entity while retaining a bound on callback-generated work.
    drain_destruction(beforeDestroy, std::max<size_t>(4096, _entities.size()));
    if (!_destruction.empty()) throw std::runtime_error("Scene teardown cap reached");
    clear_queues(); _dirty.clear(); _roots.clear();
    _entities.clear(); _registry.clear();
}
void Scene::replace(Scene&& staging, const DestroyCallback& beforeDestroy) {
    if (_epoch == UINT64_MAX) throw std::runtime_error("Scene epoch exhausted");
    const auto nextEpoch = _epoch + 1;
    clear(beforeDestroy);
    *this = std::move(staging);
    _epoch = nextEpoch;
    clear_queues();
    for (const auto ref : hierarchy_order()) mark_transform_dirty(ref);
}
} // namespace mirabilis

AABB collider_from_object(const mirabilis::Scene& scene, EntityRef ref) {
    const auto* collider = scene.read<Collider>(ref);
    if (!collider) return {};
    const auto world = scene.world_matrix(ref);
    const auto center = glm::vec3(world * glm::vec4(collider->colliderCenter, 1));
    const auto localHalf = glm::max(collider->colliderHalfExtents, glm::vec3(0.001f));
    const auto half = glm::abs(glm::vec3(world[0])) * localHalf.x +
        glm::abs(glm::vec3(world[1])) * localHalf.y + glm::abs(glm::vec3(world[2])) * localHalf.z;
    return {center - half, center + half};
}
