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

    if (_noClip.enabled) {
        _playerMovement.previousPosition = _playerMovement.position;
        const glm::mat4 rotation = mainCamera.getRotationMatrix();
        const glm::vec3 forward = glm::normalize(glm::vec3(
            rotation * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
        const glm::vec3 right = glm::normalize(glm::vec3(
            rotation * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)));
        glm::vec3 direction =
            forward * ((_playerInput.forward ? 1.0f : 0.0f) -
                       (_playerInput.backward ? 1.0f : 0.0f)) +
            right * ((_playerInput.right ? 1.0f : 0.0f) -
                     (_playerInput.left ? 1.0f : 0.0f)) +
            glm::vec3(0.0f, 1.0f, 0.0f) *
                ((_noClip.up ? 1.0f : 0.0f) - (_noClip.down ? 1.0f : 0.0f));
        if (glm::length(direction) > 0.001f) direction = glm::normalize(direction);
        _playerMovement.velocity = direction * _noClip.speed;
        _playerMovement.position += _playerMovement.velocity * deltaTime;
        _playerMovement.grounded = false;
        _playerInput.jumpPressed = false;
        return;
    }

    const glm::vec3 previousPosition = _playerMovement.position;
    _playerMovement.integrate(_playerInput, deltaTime);

    _physicsStep.portalTraversalCooldown = std::max(
        0.0f, _physicsStep.portalTraversalCooldown - deltaTime);
    if (_physicsStep.portalTraversalCooldown <= 0.0f) {
        bool traversed = false;
        const auto tryBidirectionalPair = [&](const Portal& first,
                                               const Portal& second) {
            if (try_traverse_portal(first, second, previousPosition) ||
                try_traverse_portal(second, first, previousPosition)) {
                return true;
            }

            // Portal stencil surfaces already render with culling disabled.
            // Mirror their gameplay behavior so a link is transparent and
            // walkable from its reverse face too.
            Portal reverseFirst = first;
            Portal reverseSecond = second;
            orient_portal(reverseFirst, -first.normal);
            orient_portal(reverseSecond, -second.normal);
            return try_traverse_portal(
                       reverseFirst, reverseSecond, previousPosition) ||
                try_traverse_portal(
                    reverseSecond, reverseFirst, previousPosition);
        };
        if (_bluePortal.placed && _orangePortal.placed) {
            traversed = tryBidirectionalPair(_bluePortal, _orangePortal);
        }
        for (const AuthoredPortalPair& pair : _authoredPortals.pairs) {
            if (!traversed) {
                traversed = tryBidirectionalPair(pair.first, pair.second);
            }
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
    // A scene with no spawn marker must not inherit the prior scene's player
    // position. The empty sandbox is centred on this safe fallback.
    _playerMovement.settings.spawnPosition = glm::vec3(0.0f);
    return false;
}

void VulkanEngine::respawn_player()
{
    apply_scene_spawn_point();
    _playerMovement.position = _playerMovement.settings.spawnPosition;
    _playerMovement.previousPosition = _playerMovement.position;
    _playerMovement.velocity = glm::vec3(0.0f);
    _playerMovement.grounded = false;
    mainCamera.position = _playerMovement.position + glm::vec3(0.0f, 1.7f, 0.0f);
    mainCamera.velocity = glm::vec3(0.0f);
    _playerMovement.jumpBufferRemaining = 0.0f;
    _physicsStep.portalTraversalCooldown = 0.0f;
    reset_time_trial();
}

void VulkanEngine::reset_time_trial()
{
    _timeTrial.seconds = 0.0f;
    _timeTrial.running = false;
    _timeTrial.finished = false;
    // Treat the next overlap as a fresh entry. This makes the editor's Reset
    // button useful even when the player is currently standing in the start
    // volume.
    _timeTrial.playerInsideStartTrigger = false;
    _timeTrial.playerInsideFinishTrigger = false;
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

    if (insideStart && !_timeTrial.playerInsideStartTrigger) {
        _timeTrial.seconds = 0.0f;
        _timeTrial.running = true;
        _timeTrial.finished = false;
    }

    if (_timeTrial.running) {
        _timeTrial.seconds += deltaTime;
    }

    if (insideFinish && !_timeTrial.playerInsideFinishTrigger && _timeTrial.running) {
        _timeTrial.running = false;
        _timeTrial.finished = true;
        if (_timeTrial.bestSeconds < 0.0f ||
            _timeTrial.seconds < _timeTrial.bestSeconds) {
            _timeTrial.bestSeconds = _timeTrial.seconds;
        }
    }

    _timeTrial.playerInsideStartTrigger = insideStart;
    _timeTrial.playerInsideFinishTrigger = insideFinish;
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
    // Snap rather than interpolate.  The portal rotated the player instantly;
    // if the remaining fixed ticks of this frame eased toward the new yaw they
    // would accelerate the player through an arc they never turned through,
    // which on a rotated exit is a visible sideways shove.  Collapsing both
    // ends of the interpolation onto the new value makes those ticks use it
    // directly, and leaves the next frame's turn starting from here.
    _playerInput.yaw = mainCamera.yaw;
    _physicsStep.previousPlayerYaw = mainCamera.yaw;
    _physicsStep.targetPlayerYaw = mainCamera.yaw;

    // Prevent the next fixed tick from immediately re-entering the exit.
    _physicsStep.portalTraversalCooldown = 0.15f;
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
        for (const AuthoredPortalPair& pair : _authoredPortals.pairs) {
            if (pair.first.hostWallObject == object.id) carve_portal_opening(wallPieces, pair.first);
            if (pair.second.hostWallObject == object.id) carve_portal_opening(wallPieces, pair.second);
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
    _physicsStep.portalTraversalCooldown = 0.0f;
    rebuild_collision_from_scene();
}

void VulkanEngine::place_authored_portal_endpoint()
{
    if (_authoredPortals.pairs.size() >= MaxAuthoredPortalPairs) {
        return;
    }
    const Camera& camera = render_camera();
    const glm::vec3 rayDirection = glm::normalize(glm::vec3(
        camera.getRotationMatrix() * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));

    Portal candidate{};
    candidate.placed = true;
    // Authored links are proper freestanding actors.  The portal faces the
    // editor camera at a comfortable authoring distance; its transform can
    // then be refined in the Portals menu without any host-wall dependency.
    constexpr float DefaultPlacementDistance = 5.0f;
    candidate.position = camera.position + rayDirection * DefaultPlacementDistance;
    orient_portal(candidate, -rayDirection);
    const auto overlapsExisting = [&](const Portal& existing) {
        return portals_overlap(candidate, existing);
    };
    if (overlapsExisting(_bluePortal) || overlapsExisting(_orangePortal) ||
        (_authoredPortals.draft.has_value() && overlapsExisting(*_authoredPortals.draft))) {
        return;
    }
    for (const AuthoredPortalPair& pair : _authoredPortals.pairs) {
        if (overlapsExisting(pair.first) || overlapsExisting(pair.second)) return;
    }
    if (_authoredPortals.draft.has_value()) {
        _authoredPortals.pairs.push_back(AuthoredPortalPair{*_authoredPortals.draft, candidate});
        _authoredPortals.draft.reset();
    } else {
        _authoredPortals.draft = candidate;
    }
    _sceneDocument.dirty = true;
    rebuild_collision_from_scene();
}

void VulkanEngine::clear_authored_portals()
{
    _authoredPortals.pairs.clear();
    _authoredPortals.draft.reset();
    _sceneDocument.dirty = true;
    rebuild_collision_from_scene();
}

bool VulkanEngine::create_three_room_pole_chain()
{
    struct RoomPole {
        SceneObjectID root{InvalidSceneObject};
        glm::vec3 roomCenter{0.0f};
        glm::vec3 polePosition{0.0f};
    };
    std::vector<RoomPole> rooms;
    for (const SceneObject& root : _scene.objects) {
        if (!root.alive || root.parent != _sandboxRoot ||
            !root.name.starts_with("Long Closed Room")) {
            continue;
        }
        for (const SceneObjectID childID : root.children) {
            const SceneObject* child = _scene.get(childID);
            if (child != nullptr && child->name == "Center Pole") {
                rooms.push_back(RoomPole{root.id,
                    glm::vec3(_scene.world_matrix(root.id)[3]),
                    glm::vec3(_scene.world_matrix(childID)[3])});
                break;
            }
        }
    }
    if (rooms.size() < 3) return false;

    // The current level has its rooms arranged left-to-right. Sorting by X
    // makes the setup deterministic even after selecting the roots in a
    // different order.
    std::sort(rooms.begin(), rooms.end(), [](const RoomPole& lhs,
                                              const RoomPole& rhs) {
        return lhs.polePosition.x > rhs.polePosition.x;
    });

    const auto makeEndpoint = [](glm::vec3 position, const glm::vec3& normal,
                                 float halfWidth) {
        Portal portal{};
        portal.placed = true;
        portal.hostWallObject = InvalidSceneObject;
        portal.position = position;
        portal.halfWidth = halfWidth;
        portal.halfHeight = 2.98f;
        orient_portal(portal, normal);
        return portal;
    };
    struct Opening {
        glm::vec3 leftCenter{0.0f};
        glm::vec3 rightCenter{0.0f};
        float leftHalfWidth{1.0f};
        float rightHalfWidth{1.0f};
    };
    const auto openingForRoom = [](const RoomPole& room) {
        constexpr float WallInnerOffset = 5.84f;
        constexpr float PoleHalfWidth = 0.25f;
        // Keep the rasterized portal edge just inside the architectural gap.
        // The stencil pass intentionally ignores depth, so an exactly shared
        // edge can otherwise leak one pixel across the pole or side wall.
        constexpr float EdgeInset = 0.02f;
        constexpr float SurfaceOffsetPastPole = 0.28f;
        const float leftEdge = room.roomCenter.x - WallInnerOffset + EdgeInset;
        const float poleLeft = room.polePosition.x - PoleHalfWidth - EdgeInset;
        const float poleRight = room.polePosition.x + PoleHalfWidth + EdgeInset;
        const float rightEdge = room.roomCenter.x + WallInnerOffset - EdgeInset;
        const float y = room.roomCenter.y + 3.0f;
        const float z = room.polePosition.z + SurfaceOffsetPastPole;
        return Opening{
            .leftCenter = glm::vec3((leftEdge + poleLeft) * 0.5f, y, z),
            .rightCenter = glm::vec3((poleRight + rightEdge) * 0.5f, y, z),
            .leftHalfWidth = (poleLeft - leftEdge) * 0.5f,
            .rightHalfWidth = (rightEdge - poleRight) * 0.5f,
        };
    };
    const Opening firstOpening = openingForRoom(rooms[0]);
    const Opening secondOpening = openingForRoom(rooms[1]);
    const Opening thirdOpening = openingForRoom(rooms[2]);
    _authoredPortals.pairs.clear();
    _authoredPortals.draft.reset();
    _authoredPortals.pairs.push_back(AuthoredPortalPair{
        makeEndpoint(firstOpening.leftCenter, glm::vec3(0, 0, -1),
                     firstOpening.leftHalfWidth),
        makeEndpoint(secondOpening.leftCenter, glm::vec3(0, 0, 1),
                     secondOpening.leftHalfWidth)});
    _authoredPortals.pairs.push_back(AuthoredPortalPair{
        makeEndpoint(secondOpening.rightCenter, glm::vec3(0, 0, -1),
                     secondOpening.rightHalfWidth),
        makeEndpoint(thirdOpening.rightCenter, glm::vec3(0, 0, 1),
                     thirdOpening.rightHalfWidth)});
    _sceneDocument.dirty = true;
    rebuild_collision_from_scene();
    return true;
}
