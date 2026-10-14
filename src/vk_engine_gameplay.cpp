#include "vk_engine.h"
#include "scene_queries.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

void VulkanEngine::update_player_movement(float deltaTime)
{
    // Keep the respawn target scene-authored. This happens before integrate()
    // because integrate() owns the fall-reset check.
    apply_scene_spawn_point();
    const glm::vec3 previousPosition = _playerMovement.position;

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
    } else {
        _playerMovement.integrate(_playerInput, deltaTime);
    }

    {
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

    if (!_noClip.enabled) {
        rebuild_collision_from_scene();
        _playerMovement.resolve_world_collision(
            _activeWallColliders, _activeGroundPlanes, _activeSurfRamps);
        update_time_trial(deltaTime);
    }
    _playerInput.jumpPressed = false;
}

bool VulkanEngine::apply_scene_spawn_point()
{
    const auto spawn = find_scene_spawn_point(_scene);
    // An empty scene must not inherit the prior scene's spawn.
    _playerMovement.settings.spawnPosition = spawn.value_or(glm::vec3(0.0f));
    return spawn.has_value();
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
    const TimeTrialOverlap overlap = collect_time_trial_overlaps(_scene, {playerMin, playerMax});
    const bool insideStart = overlap.start;
    const bool insideFinish = overlap.finish;

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
    // Near-plane-safe aperture shaders keep the view live until the actual
    // crossing. An early transfer band loses entries that start inside it.
    const float previousDistance = portal_signed_distance(source, previousPosition);
    const float currentDistance = portal_signed_distance(
        source, _playerMovement.position);
    const bool crossedPortalPlane =
        previousDistance > 0.0f &&
        currentDistance <= 0.0f &&
        previousDistance - currentDistance > 0.000001f;

    if (!crossedPortalPlane) {
        return false;
    }

    const float distanceDelta = previousDistance - currentDistance;
    const float crossingFraction = distanceDelta > 0.000001f
        ? std::clamp(
            previousDistance / distanceDelta,
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
    // is still one solid cube. Keep the player on the room-facing side before
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
    _playerStep.previousPlayerYaw = mainCamera.yaw;
    _playerStep.targetPlayerYaw = mainCamera.yaw;

    // Teleporting is a camera cut: screen-space history belongs to the old
    // room, and reprojecting it across the link can flash its lighting.
    _ssgi.historyValid = false;
    _previousMainViewProjectionValid = false;

    // The signed plane crossing rejects outward motion at the exit naturally.
    // A timed cooldown instead lets quick re-entry pass through the host wall.
    return true;
}

void VulkanEngine::rebuild_collision_from_scene()
{
    std::vector<Portal> portals{_bluePortal, _orangePortal};
    for (const auto& pair : _authoredPortals.pairs) {
        portals.push_back(pair.first); portals.push_back(pair.second);
    }
    PlayerCollisionSet collision{std::move(_activeWallColliders), std::move(_activeGroundPlanes), std::move(_activeSurfRamps)};
    collect_player_collision(_scene, portals, collision);
    _activeWallColliders = std::move(collision.walls);
    _activeGroundPlanes = std::move(collision.groundPlanes);
    _activeSurfRamps = std::move(collision.surfRamps);
}

void VulkanEngine::place_portal(Portal& portal, const Portal& otherPortal)
{
    const glm::vec3 rayOrigin = mainCamera.position;
    const glm::vec3 rayDirection = glm::normalize(glm::vec3(
        mainCamera.getRotationMatrix() * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));

    std::optional<RaycastHit> closestHit;
    AABB closestWall{};
    SceneObjectID closestWallObject = InvalidSceneObject;
    for (const EntityRef object : _scene.creation_order()) {
        if (!_scene.has<PortalPlaceable>(object) || _scene.dynamic_ancestor(object)) continue;
        const AABB wall = collider_from_object(_scene, object);
        const std::optional<RaycastHit> hit = raycast_aabb(rayOrigin, rayDirection, wall);
        if (hit.has_value() &&
            (!closestHit.has_value() || hit->distance < closestHit->distance)) {
            closestHit = hit;
            closestWall = wall;
            closestWallObject = object.persistentId;
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
    _scene.set_authored_portal_hosts({});
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
    for (const EntityRef root : _scene.creation_order()) {
        const Hierarchy* hierarchy = _scene.read<Hierarchy>(root);
        const Name* name = _scene.read<Name>(root);
        if (hierarchy == nullptr || hierarchy->parent != _sandboxRoot ||
            name == nullptr || !name->value.starts_with("Long Closed Room")) continue;
        for (const PersistentID childID : hierarchy->children) {
            const EntityRef child = _scene.get(childID);
            const Name* childName = _scene.read<Name>(child);
            if (childName != nullptr && childName->value == "Center Pole") {
                rooms.push_back(RoomPole{root.persistentId,
                    glm::vec3(_scene.world_matrix(root)[3]),
                    glm::vec3(_scene.world_matrix(child)[3])});
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
        portal.hostWallObject.reset();
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

void VulkanEngine::cleanup_scene_entity(EntityRef object)
{
    const PersistentID id = object.persistentId;
    if (_bluePortal.hostWallObject == id) _bluePortal = Portal{};
    if (_orangePortal.hostWallObject == id) _orangePortal = Portal{};
    const auto hosts = [id](const Portal& portal) { return portal.hostWallObject == id; };
    std::erase_if(_authoredPortals.pairs, [&](const AuthoredPortalPair& pair) {
        return hosts(pair.first) || hosts(pair.second);
    });
    if (_authoredPortals.draft && hosts(*_authoredPortals.draft)) _authoredPortals.draft.reset();
    std::vector<PersistentID> authoredHosts;
    for (const AuthoredPortalPair& pair : _authoredPortals.pairs) {
        if (pair.first.hostWallObject) authoredHosts.push_back(*pair.first.hostWallObject);
        if (pair.second.hostWallObject) authoredHosts.push_back(*pair.second.hostWallObject);
    }
    _scene.set_authored_portal_hosts(std::move(authoredHosts));
    _previousSceneObjectTransforms.erase(id);
    _previousSceneModelTransforms.erase(id);
    if (const auto material = _sceneMaterialRuntimes.find(id);
        material != _sceneMaterialRuntimes.end()) {
        if (material->second.constantsBuffer.buffer != VK_NULL_HANDLE) {
            destroy_buffer(material->second.constantsBuffer);
        }
        _sceneMaterialRuntimes.erase(material);
    }
    if (_selectedSceneObject == id) _selectedSceneObject = InvalidSceneObject;
    if (_materialEditor.object == id) _materialEditor.object = InvalidSceneObject;
}
