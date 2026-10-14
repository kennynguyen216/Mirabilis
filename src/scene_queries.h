#pragma once
#include "scene.h"
#include "scene_ecs.h"
#include "portal.h"
#include "player_movement.h"
#include <span>
struct PlayerCollisionSet {
    std::vector<AABB> walls;
    std::vector<GroundPlane> groundPlanes;
    std::vector<SurfRamp> surfRamps;
};
void collect_player_collision(const Scene&, std::span<const Portal>, PlayerCollisionSet&);
inline PlayerCollisionSet collect_player_collision(const Scene& scene, std::span<const Portal> portals) {
    PlayerCollisionSet result;
    collect_player_collision(scene, portals, result);
    return result;
}

std::optional<glm::vec3> find_scene_spawn_point(const mirabilis::Scene&);
struct TimeTrialOverlap { bool start{false}; bool finish{false}; };
TimeTrialOverlap collect_time_trial_overlaps(const mirabilis::Scene&, const AABB& player);
bool contributes_to_static_caches(const mirabilis::Scene&, EntityRef, bool editorMode);
std::vector<EntityRef> scene_emission_order(const mirabilis::Scene&);
