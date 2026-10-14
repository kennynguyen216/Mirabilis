#pragma once
#include "scene.h"
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
