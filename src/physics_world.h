#pragma once
#include "scene_ecs.h"
#include <memory>
#include <string>
namespace mirabilis {
// No Vulkan or public Jolt types: the core owns native body IDs and shape resources.
class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    bool synchronize(Scene&, bool editorMode);
    void step(Scene&, float dt);
    void remove(EntityRef);
    void clear();
    size_t body_count() const;
    bool has_body(EntityRef) const;
    bool sleeping(EntityRef) const;
    bool queue_teleport(Scene&, EntityRef, const Transform&);
    bool queue_velocity(Scene&, EntityRef, glm::vec3);
    bool queue_impulse(Scene&, EntityRef, glm::vec3);
    bool queue_force(Scene&, EntityRef, glm::vec3);
    glm::vec3 velocity(EntityRef) const;
    glm::mat4 render_world(const Scene&, EntityRef, float alpha) const;
    std::vector<PersistentID> consume_history_resets();
    const std::vector<std::string>& warnings() const;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
bool world_transform_has_shear(const glm::mat4&);
bool exact_world_trs(const glm::mat4&, Transform&);
}
