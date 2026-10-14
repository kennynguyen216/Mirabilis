#include "scene_queries.h"
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

void add_collider_if_nonempty(std::vector<AABB>& colliders, const AABB& collider)
{
    constexpr float MinimumThickness = 0.001f;
    const glm::vec3 size = collider.max - collider.min;
    if (size.x > MinimumThickness &&
        size.y > MinimumThickness &&
        size.z > MinimumThickness) {
        colliders.push_back(collider);
    }
}

void carve_portal_opening(
    std::vector<AABB>& wallPieces,
    const Portal& portal)
{
    const bool wallUsesX = std::abs(portal.normal.z) > 0.5f;
    const bool wallUsesZ = std::abs(portal.normal.x) > 0.5f;
    if (!wallUsesX && !wallUsesZ) {
        return;
    }

    const float openingMinU = (wallUsesX ? portal.position.x : portal.position.z) -
        portal.halfWidth;
    const float openingMaxU = (wallUsesX ? portal.position.x : portal.position.z) +
        portal.halfWidth;
    const float openingMinY = portal.position.y - portal.halfHeight;
    const float openingMaxY = portal.position.y + portal.halfHeight;

    std::vector<AABB> carvedPieces;
    carvedPieces.reserve(wallPieces.size() * 4);
    for (const AABB& piece : wallPieces) {
        const float pieceMinU = wallUsesX ? piece.min.x : piece.min.z;
        const float pieceMaxU = wallUsesX ? piece.max.x : piece.max.z;
        const float overlapMinU = std::max(pieceMinU, openingMinU);
        const float overlapMaxU = std::min(pieceMaxU, openingMaxU);
        const float overlapMinY = std::max(piece.min.y, openingMinY);
        const float overlapMaxY = std::min(piece.max.y, openingMaxY);

        if (overlapMinU >= overlapMaxU || overlapMinY >= overlapMaxY) {
            carvedPieces.push_back(piece);
            continue;
        }

        const auto addPiece = [&](float minU, float maxU, float minY, float maxY) {
            AABB remaining = piece;
            if (wallUsesX) {
                remaining.min.x = minU;
                remaining.max.x = maxU;
            } else {
                remaining.min.z = minU;
                remaining.max.z = maxU;
            }
            remaining.min.y = minY;
            remaining.max.y = maxY;
            add_collider_if_nonempty(carvedPieces, remaining);
        };

        // Left/right strips keep their full height.  The two middle strips
        // fill above and below the opening without overlapping each other.
        addPiece(pieceMinU, overlapMinU, piece.min.y, piece.max.y);
        addPiece(overlapMaxU, pieceMaxU, piece.min.y, piece.max.y);
        addPiece(overlapMinU, overlapMaxU, piece.min.y, overlapMinY);
        addPiece(overlapMinU, overlapMaxU, overlapMaxY, piece.max.y);
    }

    wallPieces = std::move(carvedPieces);
}

void collect_player_collision(const mirabilis::Scene& scene, std::span<const Portal> portals, PlayerCollisionSet& result)
{
    // Colliders are rebuilt from the scene every tick, so a wall edited in
    // the inspector moves its collision in the same frame it moves visually.
    const auto objects = scene.creation_order();
    result.walls.clear();
    result.walls.reserve(objects.size() + 12);
    result.groundPlanes.clear();
    result.groundPlanes.reserve(objects.size());
    result.surfRamps.clear();
    result.surfRamps.reserve(objects.size());

    for (const EntityRef object : objects) {
        const Collider* collider = scene.read<Collider>(object);
        if (collider == nullptr || !collider->hasCollision) {
            continue;
        }

        if (collider->collisionShape == CollisionShape::GroundPlane) {
            // A floor is a horizontal walkable surface rather than a box the
            // player gets pushed sideways from. Multiple floor actors make
            // independent platforms for course blockout.
            const glm::mat4 world = scene.world_matrix(object);
            result.groundPlanes.push_back(GroundPlane{
                .center = glm::vec2(world[3].x, world[3].z),
                .halfExtents = glm::vec2(
                    glm::length(glm::vec3(world[0])) * 0.5f,
                    glm::length(glm::vec3(world[2])) * 0.5f),
                .height = world[3].y});
            continue;
        }

        if (collider->collisionShape == CollisionShape::SurfRamp) {
            const glm::mat4 world = scene.world_matrix(object);
            result.surfRamps.push_back(SurfRamp{
                .worldToLocal = glm::inverse(world),
                .lowLeft = glm::vec3(world * glm::vec4(-0.5f, 0.0f, -0.5f, 1.0f)),
                .lowRight = glm::vec3(world * glm::vec4(0.5f, 0.0f, -0.5f, 1.0f)),
                .highLeft = glm::vec3(world * glm::vec4(-0.5f, 1.0f, 0.5f, 1.0f))});
            continue;
        }

        std::vector<AABB> wallPieces{collider_from_object(scene, object)};
        for (const auto& portal : portals) {
            if (portal.placed && portal.hostWallObject == object.persistentId) carve_portal_opening(wallPieces, portal);
        }
        for (const AABB& piece : wallPieces) {
            add_collider_if_nonempty(result.walls, piece);
        }
    }
}

std::optional<glm::vec3> find_scene_spawn_point(const mirabilis::Scene& scene)
{
    for (const EntityRef ref : scene.creation_order()) {
        const TimeTrial* role = scene.read<TimeTrial>(ref);
        if (role != nullptr && role->role == TimeTrialRole::SpawnPoint) {
            return glm::vec3(scene.world_matrix(ref)[3]);
        }
    }
    return std::nullopt;
}

TimeTrialOverlap collect_time_trial_overlaps(const mirabilis::Scene& scene, const AABB& player)
{
    TimeTrialOverlap result;
    for (const EntityRef ref : scene.creation_order()) {
        const TimeTrial* role = scene.read<TimeTrial>(ref);
        if (role == nullptr || (role->role != TimeTrialRole::StartTrigger &&
            role->role != TimeTrialRole::FinishTrigger)) continue;
        const AABB volume = collider_from_object(scene, ref);
        const bool overlaps = player.min.x <= volume.max.x && player.max.x >= volume.min.x &&
            player.min.y <= volume.max.y && player.max.y >= volume.min.y &&
            player.min.z <= volume.max.z && player.max.z >= volume.min.z;
        if (overlaps && role->role == TimeTrialRole::StartTrigger) result.start = true;
        if (overlaps && role->role == TimeTrialRole::FinishTrigger) result.finish = true;
    }
    return result;
}

bool contributes_to_static_caches(const mirabilis::Scene& scene, EntityRef ref, bool)
{
    return scene.valid(ref);
}

std::vector<EntityRef> scene_emission_order(const mirabilis::Scene& scene)
{
    return scene.hierarchy_order();
}
