#include "scene.h"

#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

glm::mat4 to_matrix(const Transform& transform)
{
    // Yaw, then pitch, then roll: the same order the camera uses.
    const glm::mat4 rotation =
        glm::rotate(glm::mat4(1.0f), transform.rotation.y, glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::rotate(glm::mat4(1.0f), transform.rotation.x, glm::vec3(1.0f, 0.0f, 0.0f)) *
        glm::rotate(glm::mat4(1.0f), transform.rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));

    return glm::translate(glm::mat4(1.0f), transform.position) * rotation *
        glm::scale(glm::mat4(1.0f), transform.scale);
}

SceneObjectID Scene::create_object(std::string name, SceneObjectID parent)
{
    const SceneObjectID id = static_cast<SceneObjectID>(objects.size());

    SceneObject object{};
    object.id = id;
    object.name = std::move(name);
    objects.push_back(std::move(object));

    if (SceneObject* parentObject = get(parent)) {
        parentObject->children.push_back(id);
        objects[id].parent = parent;
    }
    return id;
}

bool Scene::destroy_object(SceneObjectID id)
{
    SceneObject* object = get(id);
    if (object == nullptr) {
        return false;
    }

    // Destroying a parent destroys its descendants too. Copy first because a
    // child's destruction removes itself from this parent's child list.
    const std::vector<SceneObjectID> children = object->children;
    for (const SceneObjectID child : children) {
        destroy_object(child);
    }

    object = get(id);
    if (SceneObject* parentObject = get(object->parent)) {
        std::erase(parentObject->children, id);
    }
    object->children.clear();
    object->parent = InvalidSceneObject;
    object->alive = false;
    object->visible = false;
    object->hasCollision = false;
    object->portalPlaceable = false;
    object->primitive = {};
    object->model.reset();
    return true;
}

bool Scene::set_parent(SceneObjectID child, SceneObjectID parent)
{
    SceneObject* childObject = get(child);
    if (childObject == nullptr || child == parent ||
        (parent != InvalidSceneObject && get(parent) == nullptr)) {
        return false;
    }

    // A node cannot become a child of itself or of one of its descendants.
    for (SceneObjectID ancestor = parent;
         ancestor != InvalidSceneObject;) {
        if (ancestor == child) {
            return false;
        }
        const SceneObject* ancestorObject = get(ancestor);
        ancestor = ancestorObject == nullptr
            ? InvalidSceneObject
            : ancestorObject->parent;
    }

    if (SceneObject* oldParent = get(childObject->parent)) {
        std::erase(oldParent->children, child);
    }
    childObject->parent = parent;
    if (SceneObject* newParent = get(parent)) {
        newParent->children.push_back(child);
    }
    return true;
}

SceneObject* Scene::get(SceneObjectID id)
{
    if (id >= objects.size() || !objects[id].alive) {
        return nullptr;
    }
    return &objects[id];
}

const SceneObject* Scene::get(SceneObjectID id) const
{
    if (id >= objects.size() || !objects[id].alive) {
        return nullptr;
    }
    return &objects[id];
}

glm::mat4 Scene::local_matrix(SceneObjectID id) const
{
    const SceneObject* object = get(id);
    if (object == nullptr) {
        return glm::mat4(1.0f);
    }
    return to_matrix(object->localTransform);
}

glm::mat4 Scene::world_matrix(SceneObjectID id) const
{
    // Walk up to the root and multiply back down.  The depth guard keeps a
    // malformed parent link from hanging the frame.
    constexpr int MaxHierarchyDepth = 64;
    glm::mat4 world{1.0f};
    SceneObjectID current = id;
    for (int depth = 0; depth < MaxHierarchyDepth; depth++) {
        const SceneObject* object = get(current);
        if (object == nullptr) {
            break;
        }
        world = to_matrix(object->localTransform) * world;
        current = object->parent;
    }
    return world;
}

AABB collider_from_object(const Scene& scene, SceneObjectID id)
{
    const SceneObject* object = scene.get(id);
    if (object == nullptr) {
        return {};
    }

    const glm::mat4 world = scene.world_matrix(id);
    const glm::vec3 center = glm::vec3(
        world * glm::vec4(object->colliderCenter, 1.0f));
    const glm::vec3 localHalfExtents = glm::max(
        object->colliderHalfExtents, glm::vec3(0.001f));

    // Each world basis column describes one local axis after scale/rotation.
    // Taking absolute values gives the enclosing axis-aligned half extents.
    const glm::vec3 axisX = glm::abs(glm::vec3(world[0]));
    const glm::vec3 axisY = glm::abs(glm::vec3(world[1]));
    const glm::vec3 axisZ = glm::abs(glm::vec3(world[2]));
    const glm::vec3 halfExtents{
        axisX.x * localHalfExtents.x + axisY.x * localHalfExtents.y + axisZ.x * localHalfExtents.z,
        axisX.y * localHalfExtents.x + axisY.y * localHalfExtents.y + axisZ.y * localHalfExtents.z,
        axisX.z * localHalfExtents.x + axisY.z * localHalfExtents.y + axisZ.z * localHalfExtents.z};

    return {
        .min = center - halfExtents,
        .max = center + halfExtents,
    };
}
