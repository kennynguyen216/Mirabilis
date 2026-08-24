#include "vk_engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

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

void VulkanEngine::update_physics(float deltaTime)
{
    // Keep the respawn target scene-authored. This happens before integrate()
    // because integrate() owns the fall-reset check.
    apply_scene_spawn_point();

    const glm::vec3 previousPosition = _playerMovement.position;
    _playerMovement.integrate(_playerInput, deltaTime);

    _portalTraversalCooldown = std::max(
        0.0f, _portalTraversalCooldown - deltaTime);
    if (_portalTraversalCooldown <= 0.0f &&
        _bluePortal.placed && _orangePortal.placed) {
        if (!try_traverse_portal(_bluePortal, _orangePortal, previousPosition)) {
            try_traverse_portal(_orangePortal, _bluePortal, previousPosition);
        }
    }

    rebuild_collision_from_scene();
    _playerMovement.resolve_world_collision(
        _activeWallColliders, _activeGroundPlanes, _activeSurfRamps);
    update_time_trial(deltaTime);
    _playerInput.jumpPressed = false;
}

bool VulkanEngine::apply_scene_spawn_point()
{
    for (const SceneObject& object : _scene.objects) {
        if (object.alive && object.timeTrialRole == TimeTrialRole::SpawnPoint) {
            _playerMovement.settings.spawnPosition = glm::vec3(
                _scene.world_matrix(object.id)[3]);
            return true;
        }
    }
    return false;
}

void VulkanEngine::reset_time_trial()
{
    _timeTrialSeconds = 0.0f;
    _timeTrialRunning = false;
    _timeTrialFinished = false;
    // Treat the next overlap as a fresh entry. This makes the editor's Reset
    // button useful even when the player is currently standing in the start
    // volume.
    _playerInsideStartTrigger = false;
    _playerInsideFinishTrigger = false;
}

void VulkanEngine::update_time_trial(float deltaTime)
{
    const float playerHalfWidth = _playerMovement.settings.playerHalfWidth;
    const glm::vec3 playerMin = _playerMovement.position - glm::vec3(
        playerHalfWidth, 0.0f, playerHalfWidth);
    const glm::vec3 playerMax = _playerMovement.position + glm::vec3(
        playerHalfWidth,
        _playerMovement.settings.playerHeight,
        playerHalfWidth);
    const auto overlaps_player = [&](const AABB& volume) {
        return playerMin.x <= volume.max.x && playerMax.x >= volume.min.x &&
            playerMin.y <= volume.max.y && playerMax.y >= volume.min.y &&
            playerMin.z <= volume.max.z && playerMax.z >= volume.min.z;
    };

    bool insideStart = false;
    bool insideFinish = false;
    for (const SceneObject& object : _scene.objects) {
        if (!object.alive) {
            continue;
        }
        if (object.timeTrialRole == TimeTrialRole::StartTrigger &&
            overlaps_player(collider_from_object(_scene, object.id))) {
            insideStart = true;
        }
        if (object.timeTrialRole == TimeTrialRole::FinishTrigger &&
            overlaps_player(collider_from_object(_scene, object.id))) {
            insideFinish = true;
        }
    }

    if (insideStart && !_playerInsideStartTrigger) {
        _timeTrialSeconds = 0.0f;
        _timeTrialRunning = true;
        _timeTrialFinished = false;
    }

    if (_timeTrialRunning) {
        _timeTrialSeconds += deltaTime;
    }

    if (insideFinish && !_playerInsideFinishTrigger && _timeTrialRunning) {
        _timeTrialRunning = false;
        _timeTrialFinished = true;
        if (_timeTrialBestSeconds < 0.0f ||
            _timeTrialSeconds < _timeTrialBestSeconds) {
            _timeTrialBestSeconds = _timeTrialSeconds;
        }
    }

    _playerInsideStartTrigger = insideStart;
    _playerInsideFinishTrigger = insideFinish;
}

bool VulkanEngine::try_traverse_portal(
    const Portal& source,
    const Portal& destination,
    const glm::vec3& previousPosition)
{
    const float playerHalfWidth = _playerMovement.settings.playerHalfWidth;
    // position is the feet point, but it is also the collider's X/Z center.
    // A rendered portal cannot safely occupy the main camera's near plane.
    // Traverse just before that happens, while preserving the matching offset
    // behind the exit portal.  The real camera then lands at the exact virtual
    // camera location that was visible through the portal on the prior frame.
    const float previousDistance = portal_signed_distance(source, previousPosition);
    const float currentDistance = portal_signed_distance(
        source, _playerMovement.position);
    constexpr float MinimumEntrySpeed = 0.01f;
    constexpr float PortalTraversalDistance = 0.12f;
    const float entrySpeed = glm::dot(_playerMovement.velocity, source.normal);
    const bool movingThroughPlane = entrySpeed < -MinimumEntrySpeed;
    const bool reachedPortalNearPlane =
        previousDistance > PortalTraversalDistance &&
        currentDistance <= PortalTraversalDistance &&
        previousDistance - currentDistance > 0.000001f;

    if (!movingThroughPlane || !reachedPortalNearPlane) {
        return false;
    }

    const float distanceDelta = previousDistance - currentDistance;
    const float crossingFraction = distanceDelta > 0.000001f
        ? std::clamp(
            (previousDistance - PortalTraversalDistance) / distanceDelta,
            0.0f,
            1.0f)
        : 1.0f;
    const glm::vec3 crossingCenter = glm::mix(
        previousPosition,
        _playerMovement.position,
        crossingFraction);
    if (!portal_overlaps_upright_player(
            source,
            crossingCenter,
            playerHalfWidth,
            _playerMovement.settings.playerHeight)) {
        return false;
    }

    _playerMovement.position = transform_position_through_portal(
        source, destination, _playerMovement.position);
    // Collision tests compare the previous and current feet positions. Keep
    // both endpoints in the same (destination) space; otherwise an elevated
    // exit platform appears to have no collider because the old source-space
    // Y value cannot prove that the player crossed its top from above.
    _playerMovement.previousPosition = transform_position_through_portal(
        source, destination, _playerMovement.previousPosition);
    _playerMovement.velocity = transform_direction_through_portal(
        source, destination, _playerMovement.velocity);

    // Our physics collider has a portal-shaped gap, but the visible host wall
    // is still one solid cube. The early near-plane transfer maps the player
    // just behind the exit plane, so move them to the room-facing side before
    // the main camera renders and cannot end up inside that opaque cube.
    constexpr float ExitEpsilon = 0.02f;
    const float exitDistance = portal_signed_distance(
        destination, _playerMovement.position);
    if (exitDistance < ExitEpsilon) {
        _playerMovement.position += destination.normal *
            (ExitEpsilon - exitDistance);
    }

    const glm::vec3 transformedForward = glm::normalize(
        transform_direction_through_portal(
            source,
            destination,
            glm::vec3(mainCamera.getRotationMatrix() *
                glm::vec4(0.0f, 0.0f, -1.0f, 0.0f))));
    mainCamera.yaw = std::atan2(transformedForward.x, -transformedForward.z);
    mainCamera.pitch = std::asin(std::clamp(transformedForward.y, -1.0f, 1.0f));
    _playerInput.yaw = mainCamera.yaw;

    // Prevent the next fixed tick from immediately re-entering the exit.
    _portalTraversalCooldown = 0.15f;
    return true;
}

void VulkanEngine::rebuild_collision_from_scene()
{
    // Colliders are rebuilt from the scene every tick, so a wall edited in
    // the inspector moves its collision in the same frame it moves visually.
    _activeWallColliders.clear();
    _activeWallColliders.reserve(_scene.objects.size() + 12);
    _activeGroundPlanes.clear();
    _activeGroundPlanes.reserve(_scene.objects.size());
    _activeSurfRamps.clear();
    _activeSurfRamps.reserve(_scene.objects.size());

    for (const SceneObject& object : _scene.objects) {
        if (!object.alive || !object.hasCollision) {
            continue;
        }

        if (object.collisionShape == CollisionShape::GroundPlane) {
            // A floor is a horizontal walkable surface rather than a box the
            // player gets pushed sideways from. Multiple floor actors make
            // independent platforms for course blockout.
            const glm::mat4 world = _scene.world_matrix(object.id);
            _activeGroundPlanes.push_back(GroundPlane{
                .center = glm::vec2(world[3].x, world[3].z),
                .halfExtents = glm::vec2(
                    glm::length(glm::vec3(world[0])) * 0.5f,
                    glm::length(glm::vec3(world[2])) * 0.5f),
                .height = world[3].y});
            continue;
        }

        if (object.collisionShape == CollisionShape::SurfRamp) {
            const glm::mat4 world = _scene.world_matrix(object.id);
            _activeSurfRamps.push_back(SurfRamp{
                .worldToLocal = glm::inverse(world),
                .lowLeft = glm::vec3(world * glm::vec4(-0.5f, 0.0f, -0.5f, 1.0f)),
                .lowRight = glm::vec3(world * glm::vec4(0.5f, 0.0f, -0.5f, 1.0f)),
                .highLeft = glm::vec3(world * glm::vec4(-0.5f, 1.0f, 0.5f, 1.0f))});
            continue;
        }

        std::vector<AABB> wallPieces{collider_from_object(_scene, object.id)};
        if (_bluePortal.placed && _bluePortal.hostWallObject == object.id) {
            carve_portal_opening(wallPieces, _bluePortal);
        }
        if (_orangePortal.placed && _orangePortal.hostWallObject == object.id) {
            carve_portal_opening(wallPieces, _orangePortal);
        }
        for (const AABB& piece : wallPieces) {
            add_collider_if_nonempty(_activeWallColliders, piece);
        }
    }
}

void VulkanEngine::place_portal(Portal& portal, const Portal& otherPortal)
{
    const glm::vec3 rayOrigin = mainCamera.position;
    const glm::vec3 rayDirection = glm::normalize(glm::vec3(
        mainCamera.getRotationMatrix() * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));

    std::optional<RaycastHit> closestHit;
    AABB closestWall{};
    SceneObjectID closestWallObject = InvalidSceneObject;
    for (const SceneObject& object : _scene.objects) {
        if (!object.alive || !object.portalPlaceable) {
            continue;
        }
        const AABB wall = collider_from_object(_scene, object.id);
        const std::optional<RaycastHit> hit = raycast_aabb(
            rayOrigin, rayDirection, wall);
        if (hit.has_value() &&
            (!closestHit.has_value() || hit->distance < closestHit->distance)) {
            closestHit = hit;
            closestWall = wall;
            closestWallObject = object.id;
        }
    }

    if (!closestHit.has_value() || closestWallObject == InvalidSceneObject) {
        return;
    }

    constexpr float PortalSurfaceOffset = 0.01f;
    Portal candidate = portal;
    candidate.placed = true;
    candidate.hostWallObject = closestWallObject;
    candidate.position = closestHit->position +
        closestHit->normal * PortalSurfaceOffset;
    orient_portal(candidate, closestHit->normal);

    if (!snap_portal_to_wall(candidate, closestWall)) {
        return;
    }
    if (portals_overlap(candidate, otherPortal)) {
        return;
    }

    portal = candidate;
}

void VulkanEngine::retract_portals()
{
    // Resetting the complete value also clears hostWallObject. That avoids a
    // stale portal opening if its former wall is edited after retraction.
    _bluePortal = Portal{};
    _orangePortal = Portal{};
    _portalTraversalCooldown = 0.0f;
    rebuild_collision_from_scene();
}
