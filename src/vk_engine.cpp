#include "vk_engine.h"
#include <vk_types.h>
#include <vk_initializers.h>
#include <VkBootstrap.h>
#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_initializers.h>
#include <vk_types.h>
#include <vk_images.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <thread>
#include <unordered_map>
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
#include <vk_pipelines.h>
#include <glm/gtx/transform.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/vec2.hpp>
#include <algorithm>
#include <simdjson.h>
#include <stb_image.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"
#include "ImGuizmo.h"

constexpr bool bUseValidationLayers = false;

VulkanEngine* loadedEngine = nullptr;

namespace {

constexpr const char* EditorSceneDirectory = "../../assets/scenes";
constexpr const char* LastEditorScenePath = "../../assets/scenes/.last_scene";
constexpr const char* DefaultEditorSceneFilename = "sandbox.json";
constexpr uint64_t EditorSceneVersion = 1;

std::optional<std::string> normalize_scene_filename(std::string_view name)
{
    if (name.empty() || name.find_first_of("\\/:*?\"<>|") != std::string_view::npos) {
        return std::nullopt;
    }
    std::string filename{name};
    if (!filename.ends_with(".json")) {
        filename += ".json";
    }
    return filename;
}

std::filesystem::path editor_scene_path(std::string_view filename)
{
    return std::filesystem::path(EditorSceneDirectory) / std::string(filename);
}

struct SavedSceneObject {
    uint32_t oldID{0};
    int64_t oldParent{-1};
    std::string name;
    Transform transform{};
    bool visible{true};
    bool hasCollision{false};
    bool portalPlaceable{false};
    RenderLayer layer{RenderLayer::World};
    CollisionShape collisionShape{CollisionShape::Box};
    glm::vec3 colliderCenter{0.0f};
    glm::vec3 colliderHalfExtents{0.5f};
    SceneAssetKind assetKind{SceneAssetKind::None};
    TimeTrialRole timeTrialRole{TimeTrialRole::None};
    std::string modelPath;
};

const char* scene_asset_name(SceneAssetKind kind)
{
    switch (kind) {
    case SceneAssetKind::FloorQuad: return "floor";
    case SceneAssetKind::UnitCube: return "cube";
    case SceneAssetKind::SurfRamp: return "ramp";
    case SceneAssetKind::ImportedGLTF: return "gltf";
    case SceneAssetKind::None: return "empty";
    }
    return "empty";
}

std::optional<SceneAssetKind> scene_asset_from_name(std::string_view name)
{
    if (name == "floor") return SceneAssetKind::FloorQuad;
    if (name == "cube") return SceneAssetKind::UnitCube;
    if (name == "ramp") return SceneAssetKind::SurfRamp;
    if (name == "gltf") return SceneAssetKind::ImportedGLTF;
    if (name == "empty") return SceneAssetKind::None;
    return std::nullopt;
}

const char* time_trial_role_name(TimeTrialRole role)
{
    switch (role) {
    case TimeTrialRole::SpawnPoint: return "spawn";
    case TimeTrialRole::StartTrigger: return "start";
    case TimeTrialRole::FinishTrigger: return "finish";
    case TimeTrialRole::None: return "none";
    }
    return "none";
}

std::optional<TimeTrialRole> time_trial_role_from_name(std::string_view name)
{
    if (name == "spawn") return TimeTrialRole::SpawnPoint;
    if (name == "start") return TimeTrialRole::StartTrigger;
    if (name == "finish") return TimeTrialRole::FinishTrigger;
    if (name == "none") return TimeTrialRole::None;
    return std::nullopt;
}

std::string json_escape(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '\\': escaped += "\\\\"; break;
        case '\"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += character; break;
        }
    }
    return escaped;
}

bool read_json_vec3(simdjson::dom::element element, glm::vec3& value)
{
    simdjson::dom::array values;
    if (element.get_array().get(values)) {
        return false;
    }
    auto valueIt = values.begin();
    for (int index = 0; index < 3; index++) {
        if (valueIt == values.end()) {
            return false;
        }
        double component = 0.0;
        if ((*valueIt).get_double().get(component)) {
            return false;
        }
        value[index] = static_cast<float>(component);
        ++valueIt;
    }
    return valueIt == values.end();
}

// Inverse of scene.h's Y * X * Z Euler convention.  ImGuizmo gives us a
// modified matrix; scene objects store editable components, so recover those
// components after each drag instead of keeping a second matrix representation.
Transform transform_from_matrix(const glm::mat4& matrix)
{
    constexpr float Epsilon = 0.00001f;
    Transform transform{};
    transform.position = glm::vec3(matrix[3]);

    glm::mat3 rotation{matrix};
    transform.scale = glm::vec3(
        glm::length(rotation[0]),
        glm::length(rotation[1]),
        glm::length(rotation[2]));
    if (transform.scale.x < Epsilon || transform.scale.y < Epsilon ||
        transform.scale.z < Epsilon) {
        return transform;
    }
    rotation[0] /= transform.scale.x;
    rotation[1] /= transform.scale.y;
    rotation[2] /= transform.scale.z;

    // For R = Ry(yaw) * Rx(pitch) * Rz(roll):
    // pitch = asin(-R[1][2]), yaw = atan2(R[0][2], R[2][2]),
    // roll = atan2(R[1][0], R[1][1]). GLM uses column-major indexing.
    transform.rotation.x = std::asin(std::clamp(-rotation[2][1], -1.0f, 1.0f));
    const float cosPitch = std::cos(transform.rotation.x);
    if (std::abs(cosPitch) > Epsilon) {
        transform.rotation.y = std::atan2(rotation[2][0], rotation[2][2]);
        transform.rotation.z = std::atan2(rotation[0][1], rotation[1][1]);
    } else {
        // At the singularity yaw and roll describe the same degree of
        // freedom. Preserve a stable, editable representation.
        transform.rotation.y = std::atan2(-rotation[0][2], rotation[0][0]);
        transform.rotation.z = 0.0f;
    }
    return transform;
}

} // namespace

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

// Subtract one vertical portal opening from a group of wall collider pieces.
// Repeating this for every portal hosted by a wall handles both the usual
// one-opening case and two separate portals on the same wall.
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

bool is_visible(const RenderObject& object, const glm::mat4& viewProjection)
{
    const std::array<glm::vec3, 8> corners = {
        glm::vec3{1, 1, 1}, glm::vec3{1, 1, -1},
        glm::vec3{1, -1, 1}, glm::vec3{1, -1, -1},
        glm::vec3{-1, 1, 1}, glm::vec3{-1, 1, -1},
        glm::vec3{-1, -1, 1}, glm::vec3{-1, -1, -1}};

    const glm::mat4 objectMatrix = viewProjection * object.transform;
    glm::vec3 minClip{1.5f};
    glm::vec3 maxClip{-1.5f};

    for (const glm::vec3& corner : corners) {
        glm::vec4 projected = objectMatrix * glm::vec4(
            object.bounds.origin + corner * object.bounds.extents,
            1.0f);
        if (projected.w <= 0.0f) {
            return true;
        }
        projected /= projected.w;
        minClip = glm::min(minClip, glm::vec3(projected));
        maxClip = glm::max(maxClip, glm::vec3(projected));
    }

    return !(minClip.z > 1.0f || maxClip.z < 0.0f ||
             minClip.x > 1.0f || maxClip.x < -1.0f ||
             minClip.y > 1.0f || maxClip.y < -1.0f);
}

VulkanEngine& VulkanEngine::Get() {return *loadedEngine;}
void VulkanEngine::init() 
{
    //only one engine init is allowed with the application
    assert(loadedEngine == nullptr);
    loadedEngine = this;

    // initializedSDL and create a window with it

    SDL_Init(SDL_INIT_VIDEO);

    SDL_WindowFlags window_flags =
        static_cast<SDL_WindowFlags>(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

    _window = SDL_CreateWindow(
        "Vulkan Engine",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        _windowExtent.width,
        _windowExtent.height,
        window_flags
    );
    set_mouse_capture(true);
    // creates the vulkan instance debug messengers, selectio of physical gpu, and device
    init_vulkan();
    init_swapchain(); // collection of image buggesr that allows vulkan to write daat into those buffers. prevents screen tearing
    init_commands(); // allocates memory and strucures needed to record and submit rendering isntructions to the gpu command poo and command buffer
    // * opengl draw commands instantly while vulkan have to record the rawing isntrucitons into a buffer first
    init_sync_structures(); // creates traffic cops that sync timing between cpu and gpu. fences and semaphores
    init_descriptors();
    init_pipelines();
    init_default_data();
    init_imgui();

    apply_scene_spawn_point();

    mainCamera.velocity = glm::vec3(0.0f);
    //mainCamera.position = glm::vec3(30.0f, 0.0f, -85.0f);
    //mainCamera.pitch = 0.0f;
    mainCamera.position = glm::vec3(0.0f, 5.0f, 12.0f);
    mainCamera.pitch = glm::radians(-20.0f);
    mainCamera.yaw = 0.0f;

    _playerMovement.position = _playerMovement.settings.spawnPosition;
    _playerMovement.velocity = glm::vec3(0.0f);
    _playerMovement.grounded = true;
    
    //evverything went fine
    _isInitialized = true;

    
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

void VulkanEngine::set_mouse_capture(bool captured)
{
    _mouseCaptured = captured;
    SDL_SetRelativeMouseMode(captured ? SDL_TRUE : SDL_FALSE);

    if (!captured) {
        _playerInput.forward = false;
        _playerInput.backward = false;
        _playerInput.left = false;
        _playerInput.right = false;
    }
}

void VulkanEngine::set_editor_mode(bool enabled)
{
    if (_editorMode == enabled) {
        return;
    }

    _editorMode = enabled;
    _editorCameraLooking = false;
    _physicsAccumulator = 0.0f;
    _playerInsideStartTrigger = false;
    _playerInsideFinishTrigger = false;

    if (enabled) {
        // Start where the player was looking, then let the editor camera move
        // independently.  This feels much less disorienting than spawning a
        // second camera at an arbitrary point in the level.
        _editorCamera = mainCamera;
        _editorCamera.velocity = glm::vec3(0.0f);
        _editorCamera.moveSpeed = 8.0f;
        set_mouse_capture(false);
    } else {
        _editorCamera.velocity = glm::vec3(0.0f);
        apply_scene_spawn_point();
        _playerMovement.position = _playerMovement.settings.spawnPosition;
        _playerMovement.velocity = glm::vec3(0.0f);
        _playerMovement.grounded = false;
        reset_time_trial();
        set_mouse_capture(true);
    }
}

const Camera& VulkanEngine::render_camera() const
{
    return _editorMode ? _editorCamera : mainCamera;
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

void VulkanEngine::init_vulkan()
{
    vkb::InstanceBuilder builder;

    //make the vulkan instance with basic debug features
    auto inst_ret = builder.set_app_name("Example Vulkan Application")
        .request_validation_layers(bUseValidationLayers)
        .use_default_debug_messenger()
        .require_api_version(1,3,0)
        .build();

    vkb::Instance vkb_inst = inst_ret.value();
    //grab the instance
    _instance = vkb_inst.instance;
    _debug_messenger = vkb_inst.debug_messenger;

    SDL_Vulkan_CreateSurface(_window, _instance, &_surface);

    //vulkan 1.3 features
    VkPhysicalDeviceVulkan13Features features { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features.dynamicRendering = true;
    features.synchronization2 = true;

    // vulkan 1.2 features
    VkPhysicalDeviceVulkan12Features features12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.bufferDeviceAddress = true;
    features12.descriptorIndexing = true;

    // Portal views use a hardware clip distance so geometry behind the exit
    // portal is removed before rasterization/depth testing.
    VkPhysicalDeviceFeatures features10{};
    features10.shaderClipDistance = VK_TRUE;

    // use vkbootstrap to select a gpu
    // we want a gpu that can write tot eh sdl surfance and supporters vulkan 1.3 with correct features
    vkb::PhysicalDeviceSelector selector { vkb_inst};
    vkb::PhysicalDevice physicalDevice = selector
        .set_minimum_version(1,3)
        .set_required_features(features10)
        .set_required_features_13(features)
        .set_required_features_12(features12)
        .set_surface(_surface)
        .select()
        .value();

    // create the vulkan device now

    vkb::DeviceBuilder deviceBuilder {physicalDevice};
    vkb::Device vkbDevice = deviceBuilder.build().value();

    // get the VkDevice handle used in the rest of the vulan apllication 
    _device = vkbDevice.device;
    _chosenGPU = physicalDevice.physical_device;
    _graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    _graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.physicalDevice = _chosenGPU;
    allocatorInfo.device = _device;
    allocatorInfo.instance = _instance;
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    vmaCreateAllocator(&allocatorInfo, &_allocator);

    _mainDeletionQueue.push_function([&]() {
        vmaDestroyAllocator(_allocator);
    });
}

void VulkanEngine::init_commands()
{
    // Describe command pools before trying to create them.
    VkCommandPoolCreateInfo commandPoolInfo =
        vkinit::command_pool_create_info(
            _graphicsQueueFamily,
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

    // Create the immediate-submit command pool.
    VK_CHECK(vkCreateCommandPool(
        _device,
        &commandPoolInfo,
        nullptr,
        &_immCommandPool));

    // Allocate the immediate-submit command buffer.
    VkCommandBufferAllocateInfo immCmdAllocInfo =
        vkinit::command_buffer_allocate_info(_immCommandPool, 1);

    VK_CHECK(vkAllocateCommandBuffers(
        _device,
        &immCmdAllocInfo,
        &_immCommandBuffer));

    // Create the command resources used by normal frames.
    for (int i = 0; i < FRAME_OVERLAP; i++) {
        VK_CHECK(vkCreateCommandPool(
            _device,
            &commandPoolInfo,
            nullptr,
            &_frames[i]._commandPool));

        VkCommandBufferAllocateInfo cmdAllocInfo =
            vkinit::command_buffer_allocate_info(
                _frames[i]._commandPool, 1);

        VK_CHECK(vkAllocateCommandBuffers(
            _device,
            &cmdAllocInfo,
            &_frames[i]._mainCommandBuffer));
    }

    _mainDeletionQueue.push_function([=]() {
        vkDestroyCommandPool(_device, _immCommandPool, nullptr);
    });
}
void VulkanEngine::init_sync_structures()
{
    // create syncronization structures
    //one fence to contorl when the gpou has finished
    //and 2 semaphores to syncronzie rendering with swapchain 
    VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
    VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();
    for (int i = 0; i < FRAME_OVERLAP; i++) {
		VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_frames[i]._renderFence));

		VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &_frames[i]._swapchainSemaphore));
		VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &_frames[i]._renderSemaphore));
	}
    VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_immFence));
    _mainDeletionQueue.push_function([=]() {vkDestroyFence(_device, _immFence, nullptr); });
}

void VulkanEngine::cleanup()
{
    if(_isInitialized) {
        vkDeviceWaitIdle(_device);

        // The explicit File > Save Scene command is still useful for named
        // checkpoints, but closing the editor should not discard unsaved
        // level construction work.
        if (_sceneDirty) {
            save_editor_scene();
        }

    for (int i = 0; i < FRAME_OVERLAP; i++) {
        vkDestroyCommandPool(
            _device,
            _frames[i]._commandPool,
            nullptr);

        vkDestroyFence(
            _device,
            _frames[i]._renderFence,
            nullptr);

        vkDestroySemaphore(
            _device,
            _frames[i]._renderSemaphore,
            nullptr);

        vkDestroySemaphore(
            _device,
            _frames[i]._swapchainSemaphore,
            nullptr);

        _frames[i]._deletionQueue.flush();
    }
        // Scene objects can hold the last reference to a loaded glTF, so
        // they must release it while the device is still alive.
        _scene.objects.clear();
        loadedScenes.clear();
        _playerModel.reset();
        _mainDeletionQueue.flush();
        destroy_swapchain();

        vkDestroySurfaceKHR(_instance, _surface, nullptr);
        vkDestroyDevice(_device, nullptr);

        vkb::destroy_debug_utils_messenger(_instance, _debug_messenger);
        vkDestroyInstance(_instance, nullptr);
        SDL_SetRelativeMouseMode(SDL_FALSE);
        SDL_DestroyWindow(_window);

    }

    // clear engine pointer 
    loadedEngine = nullptr;
}

void VulkanEngine::draw(float deltaTime)
{

    //wait until the gpu has finished rendering the last frame. timeout of 1 second
    VK_CHECK(vkWaitForFences(_device, 1, &get_current_frame()._renderFence, true, 1000000000));
    get_current_frame()._deletionQueue.flush();
    get_current_frame()._frameDescriptors.clear_pools(_device);
    //request image from the swapchain 
    uint32_t swapchainImageIndex;
    VkResult acquireResult = vkAcquireNextImageKHR(
        _device,
        _swapchain,
        1000000000,
        get_current_frame()._swapchainSemaphore,
        nullptr,
        &swapchainImageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR ||
        acquireResult == VK_SUBOPTIMAL_KHR) {
        resize_requested = true;
        return;
    }
    VK_CHECK(acquireResult);
	VK_CHECK(vkResetFences(_device, 1, &get_current_frame()._renderFence));
    VkCommandBuffer cmd = get_current_frame()._mainCommandBuffer;

    // we are sure that commands finished executing so reset
    VK_CHECK(vkResetCommandBuffer(cmd,0));

    // begin the command buffer recording and we will use this only once
    VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    //start recording

	_drawExtent.width = std::max(
        1u,
        static_cast<uint32_t>(
            std::min(_swapchainExtent.width, _drawImage.imageExtent.width) * renderScale));
	_drawExtent.height = std::max(
        1u,
        static_cast<uint32_t>(
            std::min(_swapchainExtent.height, _drawImage.imageExtent.height) * renderScale));

    update_scene(deltaTime);

    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

	// transition our main draw image into general layout so we can write into it
	// we will overwrite it all so we dont care about what was the older layout
	vkutil::transition_image(cmd, _drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

	ComputeEffect& effect = backgroundEffects[currentBackgroundEffect];

	// bind the selected background compute pipeline
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, effect.pipeline);

	// bind the descriptor set containing the draw image for the compute pipeline
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, effect.layout, 0, 1, &_drawImageDescriptors, 0, nullptr);

	vkCmdPushConstants(cmd, effect.layout, VK_SHADER_STAGE_COMPUTE_BIT,
		0, sizeof(ComputePushConstants), &effect.data);

	// execute the compute pipeline dispatch. We are using 16x16 workgroup size so we need to divide by it
	vkCmdDispatch(cmd,
		static_cast<uint32_t>(std::ceil(_drawExtent.width / 16.0)),
		static_cast<uint32_t>(std::ceil(_drawExtent.height / 16.0)), 1);

	vkutil::transition_image(cmd, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	vkutil::transition_image(cmd, _depthImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
	stats.drawcall_count = 0;
	stats.triangle_count = 0;
	stats.world_drawcall_count = 0;
	stats.portal_drawcall_count = 0;
	stats.mesh_draw_time = 0.0f;
	draw_geometry(
        cmd,
        mainDrawContext,
        sceneData.viewproj,
        get_current_frame().sceneDescriptor,
        true);
	stats.world_drawcall_count = stats.drawcall_count;
    // The portal view remains live all the way to the crossing plane.  Hiding
    // it for a frame-rate-sized safety band exposed the solid host wall before
    // physics teleported the player, causing the black flash.
    draw_portal_masks(cmd);
    if (_useOffscreenPortalCameras) {
        draw_offscreen_portal_views(cmd);
    } else {
        draw_portal_views(cmd);
    }
	stats.portal_drawcall_count = stats.drawcall_count - stats.world_drawcall_count;

    // Collider bounds are an editor-only overlay.  They are intentionally
    // drawn after portal composition, so they never affect playable portal
    // views or the saved scene itself.
    if (_editorMode && _showColliderBounds) {
        draw_collider_debug_bounds(cmd);
    }

	// transition the draw image and the swapchain image into their correct transfer layouts
	vkutil::transition_image(cmd, _drawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	// execute a copy from the draw image into the swapchain
	vkutil::copy_image_to_image(cmd, _drawImage.image, _swapchainImages[swapchainImageIndex], _drawExtent, _swapchainExtent);

	// Draw ImGui directly into the swapchain image with dynamic rendering.
	vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	draw_imgui(cmd, _swapchainImageViews[swapchainImageIndex]);

	// Set the swapchain image layout to Present so we can show it on screen.
	vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

	//finalize the command buffer (we can no longer add commands, but it can now be executed)
	VK_CHECK(vkEndCommandBuffer(cmd));

	//prepare the submission to the queue.
	//wait until the swapchain image is ready, then signal when rendering is finished
	VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd);
	VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		get_current_frame()._swapchainSemaphore);
	VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		get_current_frame()._renderSemaphore);

	VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo, &signalInfo, &waitInfo);

	//submit command buffer to the queue and execute it
	VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit, get_current_frame()._renderFence));

    //prepare present
	// this will put the image we just rendered to into the visible window.
	// we want to wait on the _renderSemaphore for that, 
	// as its necessary that drawing commands have finished before the image is displayed to the user
    VkPresentInfoKHR presentInfo = {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.pNext = nullptr;
	presentInfo.pSwapchains = &_swapchain;
	presentInfo.swapchainCount = 1;

	presentInfo.pWaitSemaphores = &get_current_frame()._renderSemaphore;
	presentInfo.waitSemaphoreCount = 1;

	presentInfo.pImageIndices = &swapchainImageIndex;

	VkResult presentResult = vkQueuePresentKHR(_graphicsQueue, &presentInfo);
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR ||
        presentResult == VK_SUBOPTIMAL_KHR) {
        resize_requested = true;
    } else {
        VK_CHECK(presentResult);
    }
    
	//increase the number of frames drawn
	_frameNumber++;
}

void VulkanEngine::run(){
    SDL_Event e; 
    bool bQuit = false;
    auto previousTime = std::chrono::steady_clock::now();

    //main loop
    while(!bQuit) {
        const auto currentTime = std::chrono::steady_clock::now();
        float deltaTime = std::chrono::duration<float>(currentTime - previousTime).count();
        previousTime = currentTime;
        // Avoid a large movement jump after dragging/resuming the window.
        deltaTime = std::min(deltaTime, 0.1f);

        //handle events on queue
        while(SDL_PollEvent(&e) != 0) {
            // close the window when the user alt f4 or clicks the X
            if(e.type == SDL_QUIT)
                bQuit = true;
            
                if(e.type == SDL_WINDOWEVENT) {
                    if(e.window.event == SDL_WINDOWEVENT_MINIMIZED){
                        stop_rendering = true;
                    }
                    if(e.window.event == SDL_WINDOWEVENT_RESTORED) {
                        stop_rendering = false;
                    }
                    if(e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                       e.window.event == SDL_WINDOWEVENT_RESIZED) {
                        resize_requested = true;
                    }
                }
                // Let ImGui observe every event before deciding whether the
                // editor camera should consume it.
                ImGui_ImplSDL2_ProcessEvent(&e);

                if(e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                    bQuit = true;
                }

                if (e.type == SDL_KEYDOWN &&
                    e.key.keysym.sym == SDLK_TAB &&
                    e.key.repeat == 0) {
                    set_editor_mode(!_editorMode);
                }

                // Handle this before the gameplay WASD code below.  Without
                // consuming the event, the S in Ctrl+S also starts backward
                // movement for one frame (or while the key is held).
                if (e.type == SDL_KEYDOWN &&
                    e.key.keysym.sym == SDLK_s &&
                    (e.key.keysym.mod & KMOD_CTRL) != 0 &&
                    e.key.repeat == 0 &&
                    !ImGui::GetIO().WantTextInput) {
                    save_editor_scene();
                    _playerInput.backward = false;
                    continue;
                }

                if (!_editorMode &&
                    e.type == SDL_KEYDOWN &&
                    e.key.keysym.sym == SDLK_r &&
                    e.key.repeat == 0) {
                    retract_portals();
                }

                if (_editorMode) {
                    // Match the conventional ImGuizmo/Unity-style transform
                    // bindings, but leave W/A/S/D to the fly camera while
                    // RMB is held.
                    if (e.type == SDL_KEYDOWN && e.key.repeat == 0 &&
                        !_editorCameraLooking &&
                        !ImGui::GetIO().WantCaptureKeyboard) {
                        if (e.key.keysym.sym == SDLK_w) {
                            _gizmoOperation = EditorGizmoOperation::Translate;
                        } else if (e.key.keysym.sym == SDLK_e) {
                            _gizmoOperation = EditorGizmoOperation::Rotate;
                        } else if (e.key.keysym.sym == SDLK_r) {
                            _gizmoOperation = EditorGizmoOperation::Scale;
                        } else if (e.key.keysym.sym == SDLK_s) {
                            _gizmoSnapping = !_gizmoSnapping;
                        }
                    }
                    if (e.type == SDL_KEYDOWN &&
                        e.key.keysym.sym == SDLK_d &&
                        (e.key.keysym.mod & KMOD_CTRL) != 0 &&
                        e.key.repeat == 0 &&
                        !ImGui::GetIO().WantCaptureKeyboard) {
                        duplicate_selected_scene_object();
                    }
                    if (e.type == SDL_KEYDOWN &&
                        e.key.keysym.sym == SDLK_DELETE &&
                        e.key.repeat == 0 &&
                        !ImGui::GetIO().WantCaptureKeyboard) {
                        delete_selected_scene_object();
                    }

                    // The cursor remains free for ImGui. Hold RMB over the
                    // scene to temporarily capture it for fly-camera input.
                    if (e.type == SDL_MOUSEBUTTONDOWN &&
                        e.button.button == SDL_BUTTON_RIGHT &&
                        !ImGui::GetIO().WantCaptureMouse) {
                        _editorCameraLooking = true;
                        set_mouse_capture(true);
                    }
                    if (e.type == SDL_MOUSEBUTTONUP &&
                        e.button.button == SDL_BUTTON_RIGHT &&
                        _editorCameraLooking) {
                        _editorCameraLooking = false;
                        _editorCamera.velocity = glm::vec3(0.0f);
                        set_mouse_capture(false);
                    }
                    if (_editorCameraLooking &&
                        (e.type == SDL_MOUSEMOTION ||
                         e.type == SDL_KEYDOWN || e.type == SDL_KEYUP)) {
                        _editorCamera.processSDLEvent(e);
                    }
                    if (e.type == SDL_MOUSEBUTTONDOWN &&
                        e.button.button == SDL_BUTTON_LEFT &&
                        !ImGui::GetIO().WantCaptureMouse &&
                        !ImGuizmo::IsOver()) {
                        select_scene_object_at_screen_position(
                            e.button.x, e.button.y);
                    }
                } else {
                    if (_mouseCaptured && e.type == SDL_MOUSEMOTION) {
                        mainCamera.processSDLEvent(e);
                    }

                    if (_mouseCaptured && e.type == SDL_KEYDOWN) {
                        if (e.key.keysym.sym == SDLK_w) _playerInput.forward = true;
                        if (e.key.keysym.sym == SDLK_s) _playerInput.backward = true;
                        if (e.key.keysym.sym == SDLK_a) _playerInput.left = true;
                        if (e.key.keysym.sym == SDLK_d) _playerInput.right = true;
                        if (e.key.keysym.sym == SDLK_SPACE && e.key.repeat == 0) {
                            _playerInput.jumpPressed = true;
                        }
                    }

                    if (_mouseCaptured && e.type == SDL_MOUSEWHEEL) {
                        int wheelY = e.wheel.y;
                        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                            wheelY = -wheelY;
                        }
                        if (wheelY < 0) {
                            _playerInput.jumpPressed = true;
                        }
                    }

                    if (_mouseCaptured && e.type == SDL_MOUSEBUTTONDOWN &&
                        e.button.button == SDL_BUTTON_LEFT) {
                        place_portal(_bluePortal, _orangePortal);
                    }
                    if (_mouseCaptured && e.type == SDL_MOUSEBUTTONDOWN &&
                        e.button.button == SDL_BUTTON_RIGHT) {
                        place_portal(_orangePortal, _bluePortal);
                    }
                    if (_mouseCaptured && e.type == SDL_KEYUP) {
                        if (e.key.keysym.sym == SDLK_w) _playerInput.forward = false;
                        if (e.key.keysym.sym == SDLK_s) _playerInput.backward = false;
                        if (e.key.keysym.sym == SDLK_a) _playerInput.left = false;
                        if (e.key.keysym.sym == SDLK_d) _playerInput.right = false;
                    }
                }
        }
        // do not draw if we are minimized
        if(stop_rendering) {
            //throttle the speed to avoid endless spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (resize_requested) {
            resize_swapchain();
            if (stop_rendering) {
                continue;
            }
        }

        if (_editorMode) {
            _editorCamera.update(deltaTime);
            _physicsAccumulator = 0.0f;
        } else {
            _physicsAccumulator = std::min(
                _physicsAccumulator + deltaTime,
                PhysicsDt * static_cast<float>(MaxPhysicsSteps));
            _playerInput.yaw = mainCamera.yaw;
            while (_physicsAccumulator >= PhysicsDt) {
                update_physics(PhysicsDt);
                _physicsAccumulator -= PhysicsDt;
            }
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        if (_editorMode) {
            // This full-window dockspace has a transparent centre, so the
            // Vulkan scene remains the editor viewport between side panels.
            const ImGuiID dockspaceID = ImGui::DockSpaceOverViewport(
                0,
                ImGui::GetMainViewport(),
                ImGuiDockNodeFlags_PassthruCentralNode);
            setup_default_dock_layout(dockspaceID);
            draw_editor_menu();

            // The player model's offset, facing correction, and scale now
            // live on this object instead of in a separate tuning panel.
            draw_hierarchy_panel();
            draw_inspector_panel();

            if (_showDebugPanels) {
                if (ImGui::Begin("Render Settings")) {
                    ImGui::SliderFloat("Resolution Scale", &renderScale, 0.3f, 1.0f);
                    if (!backgroundEffects.empty()) {
                        ComputeEffect& selected = backgroundEffects[currentBackgroundEffect];
                        ImGui::Text("Effect: %s", selected.name);
                        ImGui::SliderInt(
                            "Effect Index",
                            &currentBackgroundEffect,
                            0,
                            static_cast<int>(backgroundEffects.size()) - 1);
                        ImGui::InputFloat4("data1", &selected.data.data1.x);
                        ImGui::InputFloat4("data2", &selected.data.data2.x);
                        ImGui::InputFloat4("data3", &selected.data.data3.x);
                        ImGui::InputFloat4("data4", &selected.data.data4.x);
                    }
                }
                ImGui::End();

                if (ImGui::Begin("Movement Tuning")) {
                    PlayerMovementSettings& movement = _playerMovement.settings;
                    ImGui::SliderFloat("Gravity", &movement.gravity, 1.0f, 60.0f);
                    ImGui::SliderFloat("Jump Speed", &movement.jumpSpeed, 1.0f, 20.0f);
                    ImGui::SliderFloat("Ground Speed", &movement.maxGroundSpeed, 1.0f, 20.0f);
                    ImGui::SliderFloat("Ground Accel", &movement.groundAcceleration, 1.0f, 100.0f);
                    ImGui::SliderFloat("Ground Friction", &movement.groundFriction, 0.0f, 20.0f);
                    ImGui::SliderFloat("Air Accel", &movement.airAcceleration, 0.0f, 50.0f);
                    ImGui::SliderFloat("Air Wish Cap", &movement.airWishSpeedCap, 0.1f, 20.0f);
                    ImGui::SliderFloat("Jump Buffer", &movement.jumpBufferSeconds, 0.0f, 0.25f);
                }
                ImGui::End();
            }
        }

        if (_editorMode) {
            draw_editor_gizmo();
        }

        stats.frametime = deltaTime * 1000.0f;
        const float horizontalSpeed = glm::length(glm::vec2(
            _playerMovement.velocity.x,
            _playerMovement.velocity.z));
        if (_editorMode && _showDebugPanels) {
            if (ImGui::Begin("Statistics")) {
                ImGui::Text("Speed %.2f", horizontalSpeed);
                ImGui::Text("Frame time %.3f ms", stats.frametime);
                ImGui::Text("Scene update %.3f ms", stats.scene_update_time);
                ImGui::Text("Mesh draw %.3f ms", stats.mesh_draw_time);
                ImGui::Text("Triangles %d", stats.triangle_count);
                ImGui::Separator();
                ImGui::Text("World draw calls %d", stats.world_drawcall_count);
                ImGui::Text("Portal draw calls %d", stats.portal_drawcall_count);
                ImGui::Text("Total draw calls %d", stats.drawcall_count);
                ImGui::Text(
                    "Portal mode: %s",
                    _useOffscreenPortalCameras
                        ? "Offscreen camera (primary only)"
                        : (_portalRecursionEnabled
                            ? "Direct stencil (one recursive level)"
                            : "Direct stencil (primary only)"));
            }
            ImGui::End();
        }

        if (!_editorMode) {
            const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImDrawList* crosshair = ImGui::GetForegroundDrawList();
            // Portal-style status reticle: blue is left mouse and orange is
            // right mouse. Bright means the portal is placed; dim means it is
            // available and the next click will create it.
            const ImU32 bluePortalColor = _bluePortal.placed
                ? IM_COL32(45, 195, 255, 255)
                : IM_COL32(45, 195, 255, 90);
            const ImU32 orangePortalColor = _orangePortal.placed
                ? IM_COL32(255, 155, 35, 255)
                : IM_COL32(255, 155, 35, 90);
            crosshair->PathArcTo(
                center, 16.0f, IM_PI * 0.5f, IM_PI * 1.5f, 14);
            crosshair->PathStroke(bluePortalColor, ImDrawFlags_None, 2.5f);
            crosshair->PathArcTo(
                center, 16.0f, -IM_PI * 0.5f, IM_PI * 0.5f, 14);
            crosshair->PathStroke(orangePortalColor, ImDrawFlags_None, 2.5f);
            const std::string speedLabel = fmt::format("Speed {:.1f}", horizontalSpeed);
            crosshair->AddText(
                ImVec2(18.0f, ImGui::GetIO().DisplaySize.y - 34.0f),
                IM_COL32(220, 230, 245, 255),
                speedLabel.c_str());
            crosshair->AddText(
                ImVec2(18.0f, ImGui::GetIO().DisplaySize.y - 56.0f),
                IM_COL32(150, 165, 185, 210),
                "LMB Blue  |  RMB Orange  |  R Retract");

            const char* timerState = _timeTrialRunning
                ? "RUNNING"
                : (_timeTrialFinished ? "FINISHED" : "READY");
            const std::string timerLabel = fmt::format(
                "{}  {:02}:{:06.3f}",
                timerState,
                static_cast<int>(_timeTrialSeconds / 60.0f),
                std::fmod(_timeTrialSeconds, 60.0f));
            const ImVec2 timerSize = ImGui::CalcTextSize(timerLabel.c_str());
            crosshair->AddText(
                ImVec2(center.x - timerSize.x * 0.5f, 26.0f),
                _timeTrialFinished
                    ? IM_COL32(100, 245, 155, 255)
                    : IM_COL32(235, 240, 250, 255),
                timerLabel.c_str());
            if (_timeTrialBestSeconds >= 0.0f) {
                const std::string bestLabel = fmt::format(
                    "BEST {:02}:{:06.3f}",
                    static_cast<int>(_timeTrialBestSeconds / 60.0f),
                    std::fmod(_timeTrialBestSeconds, 60.0f));
                const ImVec2 bestSize = ImGui::CalcTextSize(bestLabel.c_str());
                crosshair->AddText(
                    ImVec2(center.x - bestSize.x * 0.5f, 48.0f),
                    IM_COL32(255, 210, 90, 255),
                    bestLabel.c_str());
            }
        }

        ImGui::Render();
        draw(deltaTime);
    }
}

void VulkanEngine::create_swapchain(uint32_t width, uint32_t height)
{
    vkb::SwapchainBuilder swapchainBuilder{ _chosenGPU, _device, _surface};

    _swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;

    vkb::Swapchain vkbSwapchain = swapchainBuilder
        .set_desired_format(VkSurfaceFormatKHR{ .format = _swapchainImageFormat, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
        .set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
        .add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR)
        .set_desired_min_image_count(vkb::SwapchainBuilder::TRIPLE_BUFFERING)
        .set_desired_extent(width, height)
        .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        .build()
        .value();

    _swapchainExtent = vkbSwapchain.extent;
    //store the swapchain and its related images
    _swapchain = vkbSwapchain.swapchain;
    _swapchainImages = vkbSwapchain.get_images().value();
    _swapchainImageViews = vkbSwapchain.get_image_views().value();
}

void VulkanEngine::init_swapchain() 
{
    create_swapchain(_windowExtent.width, _windowExtent.height);
    // draw image size will match the window
    VkExtent3D drawImageExtent = {
        _windowExtent.width, 
        _windowExtent.height, 
        1
    };

    // hardcoding raw format to 32 bit float
    _drawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    _drawImage.imageExtent = drawImageExtent;

    VkImageUsageFlags drawImageUsages{};

    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkImageCreateInfo rimg_info = vkinit::image_create_info(_drawImage.imageFormat, drawImageUsages, drawImageExtent);

    // for the draw image, we want to allocate it from gpu local memory
    VmaAllocationCreateInfo rimg_allocinfo = {};
    rimg_allocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // allocate and create the image

    VK_CHECK(vmaCreateImage(_allocator, &rimg_info, &rimg_allocinfo, &_drawImage.image, &_drawImage.allocation, nullptr));

    // build a image-view for the draw image to use for rendering

    VkImageViewCreateInfo rview_info = vkinit::imageview_create_info(_drawImage.imageFormat, _drawImage.image, VK_IMAGE_ASPECT_COLOR_BIT);

    VK_CHECK(vkCreateImageView(_device, &rview_info, nullptr, &_drawImage.imageView));

    // Real portal rendering uses the stencil half of this image to mark each
    // portal opening. Depth still handles normal world visibility.
    _depthImage.imageFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
    _depthImage.imageExtent = drawImageExtent;

    VkImageUsageFlags depthImageUsages = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImageCreateInfo dimg_info = vkinit::image_create_info(
        _depthImage.imageFormat, depthImageUsages, drawImageExtent);
    VK_CHECK(vmaCreateImage(
        _allocator,
        &dimg_info,
        &rimg_allocinfo,
        &_depthImage.image,
        &_depthImage.allocation,
        nullptr));

    VkImageViewCreateInfo dview_info = vkinit::imageview_create_info(
        _depthImage.imageFormat,
        _depthImage.image,
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VK_CHECK(vkCreateImageView(_device, &dview_info, nullptr, &_depthImage.imageView));

    // add to deletion queues
    _mainDeletionQueue.push_function([=]() {
        vkDestroyImageView(_device, _drawImage.imageView, nullptr);
        vmaDestroyImage(_allocator, _drawImage.image, _drawImage.allocation);
        vkDestroyImageView(_device, _depthImage.imageView, nullptr);
        vmaDestroyImage(_allocator, _depthImage.image, _depthImage.allocation);
    });
}

void VulkanEngine::destroy_swapchain()
{
    vkDestroySwapchainKHR(_device, _swapchain, nullptr);

    // destroy swapchain resoruces

    for(int i = 0; i < _swapchainImageViews.size(); i ++){

        vkDestroyImageView(_device, _swapchainImageViews[i], nullptr);
    }
}

void VulkanEngine::resize_swapchain()
{
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(_window, &width, &height);

    if (width == 0 || height == 0) {
        stop_rendering = true;
        return;
    }

    vkDeviceWaitIdle(_device);
    destroy_swapchain();

    _windowExtent.width = static_cast<uint32_t>(width);
    _windowExtent.height = static_cast<uint32_t>(height);
    create_swapchain(_windowExtent.width, _windowExtent.height);
    resize_requested = false;
}

void vkutil::copy_image_to_image(VkCommandBuffer cmd, VkImage source, VkImage destination, VkExtent2D srcSize, VkExtent2D dstSize)
{
	VkImageBlit2 blitRegion{ .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2, .pNext = nullptr };

	blitRegion.srcOffsets[1].x = srcSize.width;
	blitRegion.srcOffsets[1].y = srcSize.height;
	blitRegion.srcOffsets[1].z = 1;

	blitRegion.dstOffsets[1].x = dstSize.width;
	blitRegion.dstOffsets[1].y = dstSize.height;
	blitRegion.dstOffsets[1].z = 1;

	blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blitRegion.srcSubresource.baseArrayLayer = 0;
	blitRegion.srcSubresource.layerCount = 1;
	blitRegion.srcSubresource.mipLevel = 0;

	blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blitRegion.dstSubresource.baseArrayLayer = 0;
	blitRegion.dstSubresource.layerCount = 1;
	blitRegion.dstSubresource.mipLevel = 0;

	VkBlitImageInfo2 blitInfo{ .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2, .pNext = nullptr };
	blitInfo.dstImage = destination;
	blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	blitInfo.srcImage = source;
	blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	blitInfo.filter = VK_FILTER_LINEAR;
	blitInfo.regionCount = 1;
	blitInfo.pRegions = &blitRegion;

	vkCmdBlitImage2(cmd, &blitInfo);
}

void VulkanEngine::draw_background(VkCommandBuffer cmd)
{
	//make a clear-color from frame number. This will flash with a 120 frame period.
	VkClearColorValue clearValue;
	float flash = std::abs(std::sin(_frameNumber / 120.f));
	clearValue = { { 0.0f, 0.0f, flash, 1.0f } };

	VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);

	//clear image
	vkCmdClearColorImage(cmd, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &clearRange);
}

void VulkanEngine::init_descriptors()
{
    std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes =
    {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 }
    };

    globalDescriptorAllocator.init(_device, 10, sizes);

    for (FrameData& frame : _frames) {
        frame._frameDescriptors.init(_device, 1000, sizes);
    }

    // make the descriptor set layout for our compute to draw
    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        // The gradient shader ignores this binding, while the panorama sky
        // shader samples it as a regular 2D equirectangular texture.
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _drawImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
    }

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _singleImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_FRAGMENT_BIT);
    }

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        _gpuSceneDataDescriptorLayout = builder.build(
            _device,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    }

    for (FrameData& frame : _frames) {
        frame.sceneBuffer = create_buffer(
            sizeof(GPUSceneData),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU);
        // These sets live for the frame's lifetime.  They cannot come from
        // _frameDescriptors because that allocator is reset every frame.
        frame.sceneDescriptor = globalDescriptorAllocator.allocate(
            _device, _gpuSceneDataDescriptorLayout);
        DescriptorWriter sceneWriter;
        sceneWriter.write_buffer(
            0,
            frame.sceneBuffer.buffer,
            sizeof(GPUSceneData),
            0,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        sceneWriter.update_set(_device, frame.sceneDescriptor);

        for (uint32_t view = 0; view < PortalViewCount; ++view) {
            frame.portalSceneBuffers[view] = create_buffer(
                sizeof(GPUSceneData),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VMA_MEMORY_USAGE_CPU_TO_GPU);
            frame.portalSceneDescriptors[view] = globalDescriptorAllocator.allocate(
                _device, _gpuSceneDataDescriptorLayout);

            DescriptorWriter portalSceneWriter;
            portalSceneWriter.write_buffer(
                0,
                frame.portalSceneBuffers[view].buffer,
                sizeof(GPUSceneData),
                0,
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            portalSceneWriter.update_set(
                _device, frame.portalSceneDescriptors[view]);
        }
    }

    // allocate a descriptor set for our draw image

    _drawImageDescriptors = globalDescriptorAllocator.allocate(_device, _drawImageDescriptorLayout);

    DescriptorWriter writer;
    writer.write_image(
        0,
        _drawImage.imageView,
        VK_NULL_HANDLE,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    writer.update_set(_device, _drawImageDescriptors);

    //make sure both the des alloc and new layout get cleaned up properly
    _mainDeletionQueue.push_function([&](){
        globalDescriptorAllocator.destroy_pools(_device);
        for (FrameData& frame : _frames) {
            frame._frameDescriptors.destroy_pools(_device);
            destroy_buffer(frame.sceneBuffer);
            for (AllocatedBuffer& portalSceneBuffer : frame.portalSceneBuffers) {
                destroy_buffer(portalSceneBuffer);
            }
        }

        vkDestroyDescriptorSetLayout(_device, _drawImageDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _singleImageDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _gpuSceneDataDescriptorLayout, nullptr);
    });
}
void VulkanEngine::init_pipelines()
{
    init_background_pipelines();
    metalRoughMaterial.build_pipelines(this);
}

void VulkanEngine::draw_geometry(
    VkCommandBuffer cmd,
    const DrawContext& drawContext,
    const glm::mat4& viewProjection,
    VkDescriptorSet sceneDescriptor,
    bool clearDepthAndStencil,
    MaterialPipeline* overridePipeline,
    uint32_t stencilReference,
    bool useFrustumCulling,
    uint32_t stencilCompareMask)
{
    const auto startTime = std::chrono::steady_clock::now();

    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    depthAttachment.loadOp = clearDepthAndStencil
        ? VK_ATTACHMENT_LOAD_OP_CLEAR
        : VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderingAttachmentInfo stencilAttachment = depthAttachment;

    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, &depthAttachment);
    renderInfo.pStencilAttachment = &stencilAttachment;

    vkCmdBeginRendering(cmd, &renderInfo);
    VkViewport viewport{};
    viewport.width = static_cast<float>(_drawExtent.width);
    viewport.height = static_cast<float>(_drawExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = _drawExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdSetStencilReference(
        cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencilReference);
    vkCmdSetStencilCompareMask(
        cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencilCompareMask);
    vkCmdSetStencilWriteMask(
        cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0x00);

    std::vector<uint32_t> opaqueDraws;
    opaqueDraws.reserve(drawContext.OpaqueSurfaces.size());
    for (uint32_t i = 0; i < drawContext.OpaqueSurfaces.size(); ++i) {
        // Oblique portal projections replace their near plane.  The simple
        // CPU frustum test above assumes an ordinary projection, so using it
        // there wrongly throws away visible destination walls and exposes the
        // background.  The GPU still performs the real clip/depth tests.
        if (!useFrustumCulling ||
            is_visible(drawContext.OpaqueSurfaces[i], viewProjection)) {
            opaqueDraws.push_back(i);
        }
    }

    std::sort(
        opaqueDraws.begin(),
        opaqueDraws.end(),
        [&](uint32_t leftIndex, uint32_t rightIndex) {
            const RenderObject& left = drawContext.OpaqueSurfaces[leftIndex];
            const RenderObject& right = drawContext.OpaqueSurfaces[rightIndex];
            if (left.material == right.material) {
                return left.indexBuffer < right.indexBuffer;
            }
            return std::less<MaterialInstance*>{}(left.material, right.material);
        });

    MaterialPipeline* lastPipeline = nullptr;
    MaterialInstance* lastMaterial = nullptr;
    VkBuffer lastIndexBuffer = VK_NULL_HANDLE;

    auto draw = [&](const RenderObject& renderObject) {
        if (renderObject.material == nullptr) {
            return;
        }

        MaterialPipeline* pipeline = overridePipeline != nullptr
            ? overridePipeline
            : renderObject.material->pipeline;
        if (pipeline != lastPipeline) {
            lastPipeline = pipeline;
            vkCmdBindPipeline(
                cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
            vkCmdBindDescriptorSets(
                cmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline->layout,
                0,
                1,
                &sceneDescriptor,
                0,
                nullptr);
        }
        if (renderObject.material != lastMaterial) {
            lastMaterial = renderObject.material;
            vkCmdBindDescriptorSets(
                cmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline->layout,
                1,
                1,
                &renderObject.material->materialSet,
                0,
                nullptr);
        }

        if (renderObject.indexBuffer != lastIndexBuffer) {
            lastIndexBuffer = renderObject.indexBuffer;
            vkCmdBindIndexBuffer(
                cmd, renderObject.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        }

        GPUDrawPushConstants pushConstants{};
        pushConstants.worldMatrix = renderObject.transform;
        pushConstants.vertexBuffer = renderObject.vertexBufferAddress;
        vkCmdPushConstants(
            cmd,
            pipeline->layout,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(GPUDrawPushConstants),
            &pushConstants);
        vkCmdDrawIndexed(
            cmd, renderObject.indexCount, 1, renderObject.firstIndex, 0, 0);

        ++stats.drawcall_count;
        stats.triangle_count += static_cast<int>(renderObject.indexCount / 3);
    };

    for (uint32_t drawIndex : opaqueDraws) {
        draw(drawContext.OpaqueSurfaces[drawIndex]);
    }
    for (const RenderObject& renderObject : drawContext.TransparentSurfaces) {
        draw(renderObject);
    }

    vkCmdEndRendering(cmd);

    stats.mesh_draw_time += std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - startTime).count();
}

RenderObject VulkanEngine::make_portal_render_object(
    const Portal& portal,
    MaterialInstance& material) const
{
    const glm::vec3 right = glm::normalize(glm::cross(portal.up, portal.normal));
    glm::mat4 transform(1.0f);
    transform[0] = glm::vec4(right * (portal.halfWidth * 2.0f), 0.0f);
    transform[1] = glm::vec4(portal.up * (portal.halfHeight * 2.0f), 0.0f);
    transform[2] = glm::vec4(portal.normal, 0.0f);
    transform[3] = glm::vec4(portal.position, 1.0f);

    return RenderObject{
        .indexCount = 6,
        .firstIndex = 0,
        .indexBuffer = _portalMesh.indexBuffer.buffer,
        .material = &material,
        .bounds = _portalBounds,
        .transform = transform,
        .vertexBufferAddress = _portalMesh.vertexBufferAddress,
    };
}

void VulkanEngine::draw_portal_masks(VkCommandBuffer cmd)
{
    if (!_bluePortal.placed || !_orangePortal.placed) {
        return;
    }

    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderingAttachmentInfo stencilAttachment = depthAttachment;
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, &depthAttachment);
    renderInfo.pStencilAttachment = &stencilAttachment;

    vkCmdBeginRendering(cmd, &renderInfo);
    VkViewport viewport{};
    viewport.width = static_cast<float>(_drawExtent.width);
    viewport.height = static_cast<float>(_drawExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = _drawExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const VkDescriptorSet sceneDescriptor = get_current_frame().sceneDescriptor;
    const auto drawMask = [&](MaterialPipeline& pipeline,
                              const Portal& portal,
                              MaterialInstance& material,
                              uint32_t stencilReference,
                              uint32_t stencilWriteMask) {
        const RenderObject object = make_portal_render_object(portal, material);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout, 0, 1,
            &sceneDescriptor, 0, nullptr);
        vkCmdSetStencilReference(
            cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencilReference);
        vkCmdSetStencilCompareMask(
            cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
        vkCmdSetStencilWriteMask(
            cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencilWriteMask);
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout, 1, 1,
            &material.materialSet, 0, nullptr);
        vkCmdBindIndexBuffer(cmd, object.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        GPUDrawPushConstants pushConstants{};
        pushConstants.worldMatrix = object.transform;
        pushConstants.vertexBuffer = object.vertexBufferAddress;
        vkCmdPushConstants(
            cmd, pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
            sizeof(GPUDrawPushConstants), &pushConstants);
        vkCmdDrawIndexed(cmd, object.indexCount, 1, object.firstIndex, 0, 0);
    };

    // First mark only portal pixels that passed the main scene's depth test.
    // This prevents a portal hidden behind a nearer panel from drawing over it.
    drawMask(
        metalRoughMaterial.portalStencilPipeline,
        _bluePortal,
        _bluePortalMaterial,
        BluePortalView + 1,
        0xff);
    drawMask(
        metalRoughMaterial.portalStencilPipeline,
        _orangePortal,
        _orangePortalMaterial,
        OrangePortalView + 1,
        0xff);

    // Then set far depth only inside each already-visible stencil silhouette,
    // opening room for its virtual scene without touching foreground depth.
    drawMask(
        metalRoughMaterial.portalMaskPipeline,
        _bluePortal,
        _bluePortalMaterial,
        BluePortalView + 1,
        0x00);
    drawMask(
        metalRoughMaterial.portalMaskPipeline,
        _orangePortal,
        _orangePortalMaterial,
        OrangePortalView + 1,
        0x00);
    vkCmdEndRendering(cmd);
}

void VulkanEngine::draw_recursive_portal_mask(
    VkCommandBuffer cmd,
    const Portal& portal,
    MaterialInstance& material,
    VkDescriptorSet sceneDescriptor,
    uint32_t parentStencilReference,
    uint32_t recursiveStencilReference,
    uint32_t recursiveStencilBit)
{
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderingAttachmentInfo stencilAttachment = depthAttachment;
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, &depthAttachment);
    renderInfo.pStencilAttachment = &stencilAttachment;

    vkCmdBeginRendering(cmd, &renderInfo);
    VkViewport viewport{};
    viewport.width = static_cast<float>(_drawExtent.width);
    viewport.height = static_cast<float>(_drawExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = _drawExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const RenderObject object = make_portal_render_object(portal, material);
    const auto drawMask = [&](MaterialPipeline& pipeline,
                              uint32_t compareMask,
                              uint32_t writeMask) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout, 0, 1,
            &sceneDescriptor, 0, nullptr);
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout, 1, 1,
            &material.materialSet, 0, nullptr);
        vkCmdSetStencilReference(
            cmd, VK_STENCIL_FACE_FRONT_AND_BACK, recursiveStencilReference);
        vkCmdSetStencilCompareMask(
            cmd, VK_STENCIL_FACE_FRONT_AND_BACK, compareMask);
        vkCmdSetStencilWriteMask(
            cmd, VK_STENCIL_FACE_FRONT_AND_BACK, writeMask);
        vkCmdBindIndexBuffer(cmd, object.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        GPUDrawPushConstants pushConstants{};
        pushConstants.worldMatrix = object.transform;
        pushConstants.vertexBuffer = object.vertexBufferAddress;
        vkCmdPushConstants(
            cmd, pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
            sizeof(GPUDrawPushConstants), &pushConstants);
        vkCmdDrawIndexed(cmd, object.indexCount, 1, object.firstIndex, 0, 0);
    };

    // The recursive mask can only be written inside its primary portal.
    drawMask(
        metalRoughMaterial.portalRecursiveStencilPipeline,
        parentStencilReference,
        recursiveStencilBit);
    // Clear depth only where both the parent and recursive bits are set.
    drawMask(
        metalRoughMaterial.portalMaskPipeline,
        recursiveStencilReference,
        0x00);
    vkCmdEndRendering(cmd);
}

void VulkanEngine::draw_portal_views(VkCommandBuffer cmd)
{
    if (!_bluePortal.placed || !_orangePortal.placed) {
        return;
    }

    FrameData& frame = get_current_frame();
    constexpr uint32_t BluePrimaryStencil = 0x01;
    constexpr uint32_t OrangePrimaryStencil = 0x02;
    constexpr uint32_t BlueRecursiveBit = 0x04;
    constexpr uint32_t OrangeRecursiveBit = 0x08;
    constexpr uint32_t BlueRecursiveStencil = BluePrimaryStencil | BlueRecursiveBit;
    constexpr uint32_t OrangeRecursiveStencil = OrangePrimaryStencil | OrangeRecursiveBit;

    draw_portal_sky(cmd, _portalSceneData[BluePortalView], BluePrimaryStencil);
    draw_geometry(
        cmd,
        portalViewDrawContext,
        _portalSceneData[BluePortalView].viewproj,
        frame.portalSceneDescriptors[BluePortalView],
        false,
        &metalRoughMaterial.portalViewPipeline,
        BluePrimaryStencil,
        false);
    if (_portalRecursionEnabled) {
        draw_recursive_portal_mask(
            cmd,
            _bluePortal,
            _bluePortalMaterial,
            frame.portalSceneDescriptors[BluePortalView],
            BluePrimaryStencil,
            BlueRecursiveStencil,
            BlueRecursiveBit);
        draw_portal_sky(
            cmd,
            _portalSceneData[BluePortalRecursiveView],
            BlueRecursiveStencil,
            BlueRecursiveStencil);
        draw_geometry(
            cmd,
            portalViewDrawContext,
            _portalSceneData[BluePortalRecursiveView].viewproj,
            frame.portalSceneDescriptors[BluePortalRecursiveView],
            false,
            &metalRoughMaterial.portalViewPipeline,
            BlueRecursiveStencil,
            false,
            BlueRecursiveStencil);
    }

    draw_portal_sky(cmd, _portalSceneData[OrangePortalView], OrangePrimaryStencil);
    draw_geometry(
        cmd,
        portalViewDrawContext,
        _portalSceneData[OrangePortalView].viewproj,
        frame.portalSceneDescriptors[OrangePortalView],
        false,
        &metalRoughMaterial.portalViewPipeline,
        OrangePrimaryStencil,
        false);
    if (_portalRecursionEnabled) {
        draw_recursive_portal_mask(
            cmd,
            _orangePortal,
            _orangePortalMaterial,
            frame.portalSceneDescriptors[OrangePortalView],
            OrangePrimaryStencil,
            OrangeRecursiveStencil,
            OrangeRecursiveBit);
        draw_portal_sky(
            cmd,
            _portalSceneData[OrangePortalRecursiveView],
            OrangeRecursiveStencil,
            OrangeRecursiveStencil);
        draw_geometry(
            cmd,
            portalViewDrawContext,
            _portalSceneData[OrangePortalRecursiveView].viewproj,
            frame.portalSceneDescriptors[OrangePortalRecursiveView],
            false,
            &metalRoughMaterial.portalViewPipeline,
            OrangeRecursiveStencil,
            false,
            OrangeRecursiveStencil);
    }
}

void VulkanEngine::draw_geometry_to_portal_camera(
    VkCommandBuffer cmd,
    const DrawContext& drawContext,
    const glm::mat4& viewProjection,
    VkDescriptorSet sceneDescriptor,
    const AllocatedImage& colorTarget)
{
    const auto startTime = std::chrono::steady_clock::now();
    VkClearValue clearColor{};
    clearColor.color = {{0.025f, 0.045f, 0.085f, 1.0f}};
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        colorTarget.imageView, &clearColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _portalCameraDepthImage.imageView,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo stencilAttachment = depthAttachment;
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _portalCameraExtent, &colorAttachment, &depthAttachment);
    renderInfo.pStencilAttachment = &stencilAttachment;

    vkCmdBeginRendering(cmd, &renderInfo);
    VkViewport viewport{};
    viewport.width = static_cast<float>(_portalCameraExtent.width);
    viewport.height = static_cast<float>(_portalCameraExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = _portalCameraExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    MaterialInstance* lastMaterial = nullptr;
    VkBuffer lastIndexBuffer = VK_NULL_HANDLE;
    for (const RenderObject& renderObject : drawContext.OpaqueSurfaces) {
        if (renderObject.material == nullptr) {
            continue;
        }

        vkCmdBindPipeline(
            cmd,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            metalRoughMaterial.portalOffscreenPipeline.pipeline);
        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            metalRoughMaterial.portalOffscreenPipeline.layout,
            0,
            1,
            &sceneDescriptor,
            0,
            nullptr);
        if (renderObject.material != lastMaterial) {
            lastMaterial = renderObject.material;
            vkCmdBindDescriptorSets(
                cmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                metalRoughMaterial.portalOffscreenPipeline.layout,
                1,
                1,
                &renderObject.material->materialSet,
                0,
                nullptr);
        }
        if (renderObject.indexBuffer != lastIndexBuffer) {
            lastIndexBuffer = renderObject.indexBuffer;
            vkCmdBindIndexBuffer(cmd, renderObject.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        }
        GPUDrawPushConstants pushConstants{};
        pushConstants.worldMatrix = renderObject.transform;
        pushConstants.vertexBuffer = renderObject.vertexBufferAddress;
        vkCmdPushConstants(
            cmd,
            metalRoughMaterial.portalOffscreenPipeline.layout,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(GPUDrawPushConstants),
            &pushConstants);
        vkCmdDrawIndexed(
            cmd, renderObject.indexCount, 1, renderObject.firstIndex, 0, 0);
        ++stats.drawcall_count;
        stats.triangle_count += static_cast<int>(renderObject.indexCount / 3);
    }
    vkCmdEndRendering(cmd);
    stats.mesh_draw_time += std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - startTime).count();
}

void VulkanEngine::draw_offscreen_portal_views(VkCommandBuffer cmd)
{
    if (!_bluePortal.placed || !_orangePortal.placed) {
        return;
    }

    FrameData& frame = get_current_frame();
    const auto renderCamera = [&](uint32_t targetIndex, uint32_t viewIndex) {
        AllocatedImage& target = _portalCameraImages[targetIndex];
        // Every target is completely cleared before use, so the old contents
        // are irrelevant. This is valid from either first-use or shader-read.
        vkutil::transition_image(
            cmd,
            target.image,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        vkutil::transition_image(
            cmd,
            _portalCameraDepthImage.image,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        draw_geometry_to_portal_camera(
            cmd,
            portalViewDrawContext,
            _portalSceneData[viewIndex].viewproj,
            frame.portalSceneDescriptors[viewIndex],
            target);
        vkutil::transition_image(
            cmd,
            target.image,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    };

    // This experiment deliberately renders only one virtual view per portal.
    // The normal stencil mode below remains the recursive reference path.
    renderCamera(0, BluePortalView);
    renderCamera(1, OrangePortalView);

    DrawContext blueComposite{};
    blueComposite.OpaqueSurfaces.push_back(make_portal_render_object(
        _bluePortal, _portalCameraMaterials[0]));
    draw_geometry(
        cmd,
        blueComposite,
        sceneData.viewproj,
        frame.sceneDescriptor,
        false,
        &metalRoughMaterial.portalCompositePipeline,
        BluePortalView + 1,
        false);

    DrawContext orangeComposite{};
    orangeComposite.OpaqueSurfaces.push_back(make_portal_render_object(
        _orangePortal, _portalCameraMaterials[1]));
    draw_geometry(
        cmd,
        orangeComposite,
        sceneData.viewproj,
        frame.sceneDescriptor,
        false,
        &metalRoughMaterial.portalCompositePipeline,
        OrangePortalView + 1,
        false);
}

void VulkanEngine::draw_portal_sky(
    VkCommandBuffer cmd,
    const GPUSceneData& skyCamera,
    uint32_t stencilReference,
    uint32_t stencilCompareMask)
{
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderingAttachmentInfo stencilAttachment = depthAttachment;
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, &depthAttachment);
    renderInfo.pStencilAttachment = &stencilAttachment;

    vkCmdBeginRendering(cmd, &renderInfo);
    VkViewport viewport{};
    viewport.width = static_cast<float>(_drawExtent.width);
    viewport.height = static_cast<float>(_drawExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = _drawExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdSetStencilReference(
        cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencilReference);
    vkCmdSetStencilCompareMask(
        cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencilCompareMask);
    vkCmdSetStencilWriteMask(
        cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0x00);
    vkCmdBindPipeline(
        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _portalSkyPipeline.pipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        _portalSkyPipeline.layout,
        0,
        1,
        &_skyboxDescriptor,
        0,
        nullptr);
    const ComputePushConstants& backgroundData =
        backgroundEffects[currentBackgroundEffect].data;
    PortalSkyPushConstants pushConstants{};
    pushConstants.data1 = backgroundData.data1;
    pushConstants.data2 = backgroundData.data2;
    pushConstants.settings = glm::vec4(
        static_cast<float>(currentBackgroundEffect),
        static_cast<float>(_drawExtent.width),
        static_cast<float>(_drawExtent.height),
        0.0f);
    // The inverse view matrix is the camera's world transform.  Its first
    // two columns are right/up; local -Z is the camera's forward direction.
    const glm::mat4 cameraWorld = glm::inverse(skyCamera.view);
    pushConstants.cameraRight = glm::vec4(glm::normalize(glm::vec3(cameraWorld[0])), 0.0f);
    pushConstants.cameraUp = glm::vec4(glm::normalize(glm::vec3(cameraWorld[1])), 0.0f);
    pushConstants.cameraForward = glm::vec4(
        glm::normalize(-glm::vec3(cameraWorld[2])), 0.0f);
    vkCmdPushConstants(
        cmd,
        _portalSkyPipeline.layout,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(PortalSkyPushConstants),
        &pushConstants);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void VulkanEngine::draw_collider_debug_bounds(VkCommandBuffer cmd)
{
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderingAttachmentInfo stencilAttachment = depthAttachment;
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, &depthAttachment);
    renderInfo.pStencilAttachment = &stencilAttachment;

    vkCmdBeginRendering(cmd, &renderInfo);
    VkViewport viewport{};
    viewport.width = static_cast<float>(_drawExtent.width);
    viewport.height = static_cast<float>(_drawExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = _drawExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(
        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _colliderDebugPipeline.pipeline);
    for (const SceneObject& object : _scene.objects) {
        if (!object.alive || !object.hasCollision ||
            object.collisionShape == CollisionShape::GroundPlane) {
            continue;
        }

        // Draw the final world AABB used by physics—not merely the object's
        // local box—so a rotated object still shows the conservative bounds
        // the player and portal raycast actually use.
        const AABB collider = collider_from_object(_scene, object.id);
        const glm::vec3 center = (collider.min + collider.max) * 0.5f;
        const glm::vec3 fullExtents = (collider.max - collider.min) * 1.005f;
        const glm::mat4 colliderTransform = glm::translate(
            glm::mat4(1.0f), center) * glm::scale(
            glm::mat4(1.0f), fullExtents);

        ColliderDebugPushConstants pushConstants{};
        pushConstants.viewProjection = sceneData.viewproj;
        pushConstants.model = colliderTransform;
        vkCmdPushConstants(
            cmd,
            _colliderDebugPipeline.layout,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(ColliderDebugPushConstants),
            &pushConstants);
        vkCmdDraw(cmd, 24, 1, 0, 0);
        ++stats.drawcall_count;
    }
    vkCmdEndRendering(cmd);
}

GPUSceneData VulkanEngine::build_scene_data(const glm::mat4& view) const
{
    GPUSceneData data{};
    data.view = view;
    // Passing far, then near, intentionally builds the reversed-depth
    // projection used by the existing GREATER_OR_EQUAL depth pipeline.
    data.proj = glm::perspective(
        glm::radians(70.0f),
        static_cast<float>(_drawExtent.width) / static_cast<float>(_drawExtent.height),
        10000.0f,
        0.1f);
    data.proj[1][1] *= -1.0f;
    data.viewproj = data.proj * data.view;
    data.ambientColor = glm::vec4(0.1f);
    data.sunlightDirection = glm::vec4(0.0f, 1.0f, 0.5f, 1.0f);
    data.sunlightColor = glm::vec4(1.0f);
    return data;
}

GPUSceneData VulkanEngine::build_portal_scene_data(
    const glm::mat4& view,
    const Portal& destination) const
{
    GPUSceneData data = build_scene_data(view);

    // Keep the room-facing side of the destination portal and clip the
    // outside/behind-wall side. A tiny offset avoids a precision fight with
    // the wall face. This is performed in the portal fragment shader rather
    // than by mutating the reversed-Z projection matrix.
    constexpr float ClipEpsilon = 0.01f;
    const glm::vec3 clipPoint = destination.position +
        destination.normal * ClipEpsilon;
    data.portalClipPlane = glm::vec4(
        destination.normal,
        -glm::dot(destination.normal, clipPoint));
    data.portalClipEnabled = glm::vec4(1.0f);
    return data;
}

void GLTFMetallic_Roughness::build_pipelines(VulkanEngine* engine)
{
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule portalMaskVertexShader = VK_NULL_HANDLE;
    VkShaderModule portalViewVertexShader = VK_NULL_HANDLE;
    VkShaderModule portalViewFragmentShader = VK_NULL_HANDLE;
    VkShaderModule portalCompositeFragmentShader = VK_NULL_HANDLE;
    VkShaderModule portalSkyVertexShader = VK_NULL_HANDLE;
    VkShaderModule portalSkyFragmentShader = VK_NULL_HANDLE;
    VkShaderModule colliderDebugVertexShader = VK_NULL_HANDLE;
    VkShaderModule colliderDebugFragmentShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module("../../shaders/mesh.frag.spv", engine->_device, &fragmentShader) ||
        !vkutil::load_shader_module("../../shaders/mesh.vert.spv", engine->_device, &vertexShader) ||
        !vkutil::load_shader_module("../../shaders/portal_mask.vert.spv", engine->_device, &portalMaskVertexShader) ||
        !vkutil::load_shader_module("../../shaders/portal_view.vert.spv", engine->_device, &portalViewVertexShader) ||
        !vkutil::load_shader_module("../../shaders/portal_view.frag.spv", engine->_device, &portalViewFragmentShader) ||
        !vkutil::load_shader_module("../../shaders/portal_composite.frag.spv", engine->_device, &portalCompositeFragmentShader) ||
        !vkutil::load_shader_module("../../shaders/portal_sky.vert.spv", engine->_device, &portalSkyVertexShader) ||
        !vkutil::load_shader_module("../../shaders/portal_sky.frag.spv", engine->_device, &portalSkyFragmentShader) ||
        !vkutil::load_shader_module("../../shaders/collider_debug.vert.spv", engine->_device, &colliderDebugVertexShader) ||
        !vkutil::load_shader_module("../../shaders/collider_debug.frag.spv", engine->_device, &colliderDebugFragmentShader)) {
        fmt::print("Error loading material shaders\n");
        if (fragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(engine->_device, fragmentShader, nullptr);
        }
        if (vertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(engine->_device, vertexShader, nullptr);
        }
        if (portalMaskVertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(engine->_device, portalMaskVertexShader, nullptr);
        }
        if (portalViewVertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(engine->_device, portalViewVertexShader, nullptr);
        }
        if (portalViewFragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(engine->_device, portalViewFragmentShader, nullptr);
        }
        if (portalCompositeFragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(engine->_device, portalCompositeFragmentShader, nullptr);
        }
        if (portalSkyVertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(engine->_device, portalSkyVertexShader, nullptr);
        }
        if (portalSkyFragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(engine->_device, portalSkyFragmentShader, nullptr);
        }
        if (colliderDebugVertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(engine->_device, colliderDebugVertexShader, nullptr);
        }
        if (colliderDebugFragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(engine->_device, colliderDebugFragmentShader, nullptr);
        }
        return;
    }

    DescriptorLayoutBuilder layoutBuilder;
    layoutBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    layoutBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    layoutBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    materialLayout = layoutBuilder.build(
        engine->_device,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

    VkDescriptorSetLayout layouts[] = {
        engine->_gpuSceneDataDescriptorLayout,
        materialLayout};
    VkPushConstantRange matrixRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(GPUDrawPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &matrixRange;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(engine->_device, &layoutInfo, nullptr, &pipelineLayout));
    opaquePipeline.layout = pipelineLayout;
    transparentPipeline.layout = pipelineLayout;
    portalStencilPipeline.layout = pipelineLayout;
    portalRecursiveStencilPipeline.layout = pipelineLayout;
    portalMaskPipeline.layout = pipelineLayout;
    portalViewPipeline.layout = pipelineLayout;
    portalOffscreenPipeline.layout = pipelineLayout;
    portalCompositePipeline.layout = pipelineLayout;

    PipelineBuilder builder;
    builder._pipelineLayout = pipelineLayout;
    builder.set_shaders(vertexShader, fragmentShader);
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    builder.disable_blending();
    builder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
    builder.set_color_attachment_format(engine->_drawImage.imageFormat);
    builder.set_depth_format(engine->_depthImage.imageFormat);
    builder.set_stencil_format(engine->_depthImage.imageFormat);
    opaquePipeline.pipeline = builder.build_pipeline(engine->_device);

    // This pass samples a previously-rendered virtual camera image. Its
    // fragment shader is intentionally unlit, because the source scene was
    // already lit while rendering into that image.
    builder.enable_stenciltest(VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP);
    builder.set_shaders(vertexShader, portalCompositeFragmentShader);
    portalCompositePipeline.pipeline = builder.build_pipeline(engine->_device);

    builder.enable_blending_additive();
    builder.enable_depthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);
    transparentPipeline.pipeline = builder.build_pipeline(engine->_device);

    // Mark a portal in stencil only where its real, slightly front-offset
    // surface is visible against the already-rendered main scene. This pass
    // intentionally does not change depth yet.
    builder.disable_blending();
    builder.set_color_write_mask(0);
    builder.enable_depthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);
    builder.enable_stenciltest(VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_REPLACE);
    builder.set_shaders(vertexShader, fragmentShader);
    portalStencilPipeline.pipeline = builder.build_pipeline(engine->_device);

    // A recursive portal mask must already be inside its parent portal's
    // stencil value. It writes one additional bit without changing the
    // parent's bits.
    builder.enable_stenciltest(VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_REPLACE);
    portalRecursiveStencilPipeline.pipeline = builder.build_pipeline(engine->_device);

    // Clear main-scene depth to the far value only where the preceding
    // stencil pass succeeded. portal_mask.vert forces reversed depth to zero.
    builder.enable_depthtest(true, VK_COMPARE_OP_ALWAYS);
    builder.enable_stenciltest(VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP);
    builder.set_shaders(portalMaskVertexShader, fragmentShader);
    portalMaskPipeline.pipeline = builder.build_pipeline(engine->_device);

    // The linked world's pixels pass only where its portal's stencil value
    // was written by the mask pass.
    builder.set_color_write_mask(
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
    builder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
    builder.enable_stenciltest(VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP);
    builder.set_shaders(portalViewVertexShader, portalViewFragmentShader);
    portalViewPipeline.pipeline = builder.build_pipeline(engine->_device);

    // Same oblique-clipped portal-view shader, but without stencil testing:
    // it renders into a standalone camera target before composition.
    builder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
    portalOffscreenPipeline.pipeline = builder.build_pipeline(engine->_device);

    VkPipelineLayoutCreateInfo portalSkyLayoutInfo = vkinit::pipeline_layout_create_info();
    portalSkyLayoutInfo.setLayoutCount = 1;
    portalSkyLayoutInfo.pSetLayouts = &engine->_singleImageDescriptorLayout;
    VkPushConstantRange portalSkyRange{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PortalSkyPushConstants),
    };
    portalSkyLayoutInfo.pushConstantRangeCount = 1;
    portalSkyLayoutInfo.pPushConstantRanges = &portalSkyRange;
    VK_CHECK(vkCreatePipelineLayout(
        engine->_device,
        &portalSkyLayoutInfo,
        nullptr,
        &engine->_portalSkyPipeline.layout));

    PipelineBuilder skyBuilder;
    skyBuilder._pipelineLayout = engine->_portalSkyPipeline.layout;
    skyBuilder.set_shaders(portalSkyVertexShader, portalSkyFragmentShader);
    skyBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    skyBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    skyBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    skyBuilder.set_multisampling_none();
    skyBuilder.disable_blending();
    skyBuilder.disable_depthtest();
    skyBuilder.enable_stenciltest(VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP);
    skyBuilder.set_color_attachment_format(engine->_drawImage.imageFormat);
    skyBuilder.set_depth_format(engine->_depthImage.imageFormat);
    skyBuilder.set_stencil_format(engine->_depthImage.imageFormat);
    engine->_portalSkyPipeline.pipeline = skyBuilder.build_pipeline(engine->_device);

    VkPipelineLayoutCreateInfo colliderDebugLayoutInfo =
        vkinit::pipeline_layout_create_info();
    VkPushConstantRange colliderDebugRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(ColliderDebugPushConstants),
    };
    colliderDebugLayoutInfo.pushConstantRangeCount = 1;
    colliderDebugLayoutInfo.pPushConstantRanges = &colliderDebugRange;
    VK_CHECK(vkCreatePipelineLayout(
        engine->_device,
        &colliderDebugLayoutInfo,
        nullptr,
        &engine->_colliderDebugPipeline.layout));

    PipelineBuilder colliderDebugBuilder;
    colliderDebugBuilder._pipelineLayout = engine->_colliderDebugPipeline.layout;
    colliderDebugBuilder.set_shaders(
        colliderDebugVertexShader, colliderDebugFragmentShader);
    colliderDebugBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
    colliderDebugBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    colliderDebugBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    colliderDebugBuilder.set_multisampling_none();
    colliderDebugBuilder.disable_blending();
    colliderDebugBuilder.enable_depthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);
    colliderDebugBuilder.set_color_attachment_format(engine->_drawImage.imageFormat);
    colliderDebugBuilder.set_depth_format(engine->_depthImage.imageFormat);
    colliderDebugBuilder.set_stencil_format(engine->_depthImage.imageFormat);
    engine->_colliderDebugPipeline.pipeline =
        colliderDebugBuilder.build_pipeline(engine->_device);

    vkDestroyShaderModule(engine->_device, fragmentShader, nullptr);
    vkDestroyShaderModule(engine->_device, vertexShader, nullptr);
    vkDestroyShaderModule(engine->_device, portalMaskVertexShader, nullptr);
    vkDestroyShaderModule(engine->_device, portalViewVertexShader, nullptr);
    vkDestroyShaderModule(engine->_device, portalViewFragmentShader, nullptr);
    vkDestroyShaderModule(engine->_device, portalCompositeFragmentShader, nullptr);
    vkDestroyShaderModule(engine->_device, portalSkyVertexShader, nullptr);
    vkDestroyShaderModule(engine->_device, portalSkyFragmentShader, nullptr);
    vkDestroyShaderModule(engine->_device, colliderDebugVertexShader, nullptr);
    vkDestroyShaderModule(engine->_device, colliderDebugFragmentShader, nullptr);

    engine->_mainDeletionQueue.push_function([this, engine]() {
        vkDestroyPipeline(engine->_device, opaquePipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, transparentPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, portalStencilPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, portalRecursiveStencilPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, portalMaskPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, portalViewPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, portalOffscreenPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, portalCompositePipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, engine->_portalSkyPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, engine->_colliderDebugPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(engine->_device, opaquePipeline.layout, nullptr);
        vkDestroyPipelineLayout(engine->_device, engine->_portalSkyPipeline.layout, nullptr);
        vkDestroyPipelineLayout(engine->_device, engine->_colliderDebugPipeline.layout, nullptr);
        vkDestroyDescriptorSetLayout(engine->_device, materialLayout, nullptr);
    });
}

void GLTFMetallic_Roughness::clear_resources(VkDevice device)
{
    vkDestroyPipeline(device, opaquePipeline.pipeline, nullptr);
    vkDestroyPipeline(device, transparentPipeline.pipeline, nullptr);
    vkDestroyPipeline(device, portalStencilPipeline.pipeline, nullptr);
    vkDestroyPipeline(device, portalRecursiveStencilPipeline.pipeline, nullptr);
    vkDestroyPipeline(device, portalMaskPipeline.pipeline, nullptr);
    vkDestroyPipeline(device, portalViewPipeline.pipeline, nullptr);
    vkDestroyPipeline(device, portalOffscreenPipeline.pipeline, nullptr);
    vkDestroyPipeline(device, portalCompositePipeline.pipeline, nullptr);
    vkDestroyPipelineLayout(device, opaquePipeline.layout, nullptr);
    vkDestroyDescriptorSetLayout(device, materialLayout, nullptr);
}

MaterialInstance GLTFMetallic_Roughness::write_material(
    VkDevice device,
    MaterialPass pass,
    const MaterialResources& resources,
    DescriptorAllocatorGrowable& descriptorAllocator)
{
    MaterialInstance material{};
    material.passType = pass;
    material.pipeline = pass == MaterialPass::Transparent
        ? &transparentPipeline
        : &opaquePipeline;
    material.materialSet = descriptorAllocator.allocate(device, materialLayout);
    writer.clear();
    writer.write_buffer(
        0,
        resources.dataBuffer,
        sizeof(MaterialConstants),
        resources.dataBufferOffset,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    writer.write_image(
        1,
        resources.colorImage.imageView,
        resources.colorSampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.write_image(
        2,
        resources.metalRoughImage.imageView,
        resources.metalRoughSampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.update_set(device, material.materialSet);
    return material;
}

AllocatedBuffer VulkanEngine::create_buffer(
    size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage)
{
    VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.pNext = nullptr;
    bufferInfo.size = allocSize;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = memoryUsage;
    if (memoryUsage == VMA_MEMORY_USAGE_CPU_ONLY ||
        memoryUsage == VMA_MEMORY_USAGE_CPU_TO_GPU) {
        allocationInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    AllocatedBuffer newBuffer{};
    VK_CHECK(vmaCreateBuffer(
        _allocator,
        &bufferInfo,
        &allocationInfo,
        &newBuffer.buffer,
        &newBuffer.allocation,
        &newBuffer.info));

    return newBuffer;
}

void VulkanEngine::destroy_buffer(const AllocatedBuffer& buffer)
{
    if (buffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(_allocator, buffer.buffer, buffer.allocation);
    }
}

GPUMeshBuffers VulkanEngine::uploadMesh(
    std::span<uint32_t> indices, std::span<Vertex> vertices)
{
    const size_t vertexBufferSize = vertices.size_bytes();
    const size_t indexBufferSize = indices.size_bytes();

    GPUMeshBuffers newSurface{};
    newSurface.vertexBuffer = create_buffer(
        vertexBufferSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    VkBufferDeviceAddressInfo addressInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = newSurface.vertexBuffer.buffer};
    newSurface.vertexBufferAddress = vkGetBufferDeviceAddress(_device, &addressInfo);

    newSurface.indexBuffer = create_buffer(
        indexBufferSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    AllocatedBuffer staging = create_buffer(
        vertexBufferSize + indexBufferSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_CPU_ONLY);

    std::memcpy(staging.info.pMappedData, vertices.data(), vertexBufferSize);
    std::memcpy(
        static_cast<char*>(staging.info.pMappedData) + vertexBufferSize,
        indices.data(),
        indexBufferSize);

    immediate_submit([&](VkCommandBuffer cmd) {
        VkBufferCopy vertexCopy{};
        vertexCopy.srcOffset = 0;
        vertexCopy.dstOffset = 0;
        vertexCopy.size = vertexBufferSize;
        vkCmdCopyBuffer(
            cmd, staging.buffer, newSurface.vertexBuffer.buffer, 1, &vertexCopy);

        VkBufferCopy indexCopy{};
        indexCopy.srcOffset = vertexBufferSize;
        indexCopy.dstOffset = 0;
        indexCopy.size = indexBufferSize;
        vkCmdCopyBuffer(
            cmd, staging.buffer, newSurface.indexBuffer.buffer, 1, &indexCopy);
    });

    destroy_buffer(staging);
    return newSurface;
}

AllocatedImage VulkanEngine::create_image(
    VkExtent3D size,
    VkFormat format,
    VkImageUsageFlags usage,
    bool mipmapped)
{
    AllocatedImage image{};
    image.imageFormat = format;
    image.imageExtent = size;

    VkImageCreateInfo imageInfo = vkinit::image_create_info(format, usage, size);
    if (mipmapped) {
        imageInfo.mipLevels = static_cast<uint32_t>(
            std::floor(std::log2(std::max(size.width, size.height)))) + 1;
    }

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VK_CHECK(vmaCreateImage(
        _allocator,
        &imageInfo,
        &allocationInfo,
        &image.image,
        &image.allocation,
        nullptr));

    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (format == VK_FORMAT_D32_SFLOAT) {
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    } else if (format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    VkImageViewCreateInfo viewInfo = vkinit::imageview_create_info(format, image.image, aspect);
    viewInfo.subresourceRange.levelCount = imageInfo.mipLevels;
    VK_CHECK(vkCreateImageView(_device, &viewInfo, nullptr, &image.imageView));
    return image;
}

AllocatedImage VulkanEngine::create_image(
    void* data,
    VkExtent3D size,
    VkFormat format,
    VkImageUsageFlags usage,
    bool mipmapped)
{
    const size_t dataSize = static_cast<size_t>(size.width) * size.height * size.depth * 4;
    AllocatedBuffer uploadBuffer = create_buffer(
        dataSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    std::memcpy(uploadBuffer.info.pMappedData, data, dataSize);

    AllocatedImage image = create_image(
        size,
        format,
        usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        mipmapped);

    immediate_submit([&](VkCommandBuffer cmd) {
        vkutil::transition_image(
            cmd,
            image.image,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy copyRegion{};
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageExtent = size;
        vkCmdCopyBufferToImage(
            cmd,
            uploadBuffer.buffer,
            image.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &copyRegion);

        if (mipmapped) {
            vkutil::generate_mipmaps(
                cmd,
                image.image,
                VkExtent2D{image.imageExtent.width, image.imageExtent.height});
        } else {
            vkutil::transition_image(
                cmd,
                image.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });

    destroy_buffer(uploadBuffer);
    return image;
}

void VulkanEngine::destroy_image(const AllocatedImage& image)
{
    if (image.imageView != VK_NULL_HANDLE) {
        vkDestroyImageView(_device, image.imageView, nullptr);
    }
    if (image.image != VK_NULL_HANDLE) {
        vmaDestroyImage(_allocator, image.image, image.allocation);
    }
}

void VulkanEngine::init_default_data()
{
    uint32_t white = glm::packUnorm4x8(glm::vec4(1.0f));
    uint32_t grey = glm::packUnorm4x8(glm::vec4(0.66f, 0.66f, 0.66f, 1.0f));
    uint32_t black = glm::packUnorm4x8(glm::vec4(0.0f));
    _whiteImage = create_image(
        &white, {1, 1, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
    _greyImage = create_image(
        &grey, {1, 1, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
    _blackImage = create_image(
        &black, {1, 1, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
    std::array<uint32_t, 6> indices{};
    std::array<Vertex, 4> corners{};
    // A unit quad. The floor scene object scales it to the arena size, so
    // the rendered floor and the walkable ground plane cannot disagree.
    corners[0].position = glm::vec3(-0.5f, 0.f, -0.5f);
    corners[0].uv_x = 0.f;
    corners[0].uv_y = 0.f;
    corners[0].normal = glm::vec3(0.f, 1.f, 0.f);
    corners[0].color = glm::vec4(1.f);

    corners[1].position = glm::vec3(0.5f, 0.f, -0.5f);
    corners[1].uv_x = 1.f;
    corners[1].uv_y = 0.f;
    corners[1].normal = glm::vec3(0.f, 1.f, 0.f);
    corners[1].color = glm::vec4(1.f);

    corners[2].position = glm::vec3(0.5f, 0.f, 0.5f);
    corners[2].uv_x = 1.f;
    corners[2].uv_y = 1.f;
    corners[2].normal = glm::vec3(0.f, 1.f, 0.f);
    corners[2].color = glm::vec4(1.f);

    corners[3].position = glm::vec3(-0.5f, 0.f, 0.5f);
    corners[3].uv_x = 0.f;
    corners[3].uv_y = 1.f;
    corners[3].normal = glm::vec3(0.f, 1.f, 0.f);
    corners[3].color = glm::vec4(1.f);
    
    indices[0] = 0;
    indices[1] = 2;
    indices[2] = 1;

    indices[3] = 0;
    indices[4] = 3;
    indices[5] = 2;

    _floorMesh = uploadMesh(indices, corners);

    _floorBounds.origin = glm::vec3(0.0f);
    _floorBounds.extents = glm::vec3(0.5f, 0.0f, 0.5f);
    _floorBounds.sphereRadius = glm::length(_floorBounds.extents);

    std::vector<Vertex> wallVertices;
    std::vector<uint32_t> wallIndices;
    const auto makeWallVertex = [](const glm::vec3& position,
                                   const glm::vec3& normal,
                                   float u,
                                   float v) {
        Vertex vertex{};
        vertex.position = position;
        vertex.normal = normal;
        vertex.uv_x = u;
        vertex.uv_y = v;
        vertex.color = glm::vec4(1.0f);
        return vertex;
    };
    const auto addWallFace = [&](const glm::vec3& a,
                                 const glm::vec3& b,
                                 const glm::vec3& c,
                                 const glm::vec3& d,
                                 const glm::vec3& normal) {
        const uint32_t first = static_cast<uint32_t>(wallVertices.size());
        wallVertices.push_back(makeWallVertex(a, normal, 0.0f, 0.0f));
        wallVertices.push_back(makeWallVertex(b, normal, 1.0f, 0.0f));
        wallVertices.push_back(makeWallVertex(c, normal, 1.0f, 1.0f));
        wallVertices.push_back(makeWallVertex(d, normal, 0.0f, 1.0f));
        wallIndices.insert(wallIndices.end(), {
            first, first + 1, first + 2,
            first, first + 2, first + 3});
    };

    constexpr float half = 0.5f;
    addWallFace({-half, -half, half}, {half, -half, half},
                {half, half, half}, {-half, half, half}, {0.0f, 0.0f, 1.0f});
    addWallFace({half, -half, -half}, {-half, -half, -half},
                {-half, half, -half}, {half, half, -half}, {0.0f, 0.0f, -1.0f});
    addWallFace({half, -half, half}, {half, -half, -half},
                {half, half, -half}, {half, half, half}, {1.0f, 0.0f, 0.0f});
    addWallFace({-half, -half, -half}, {-half, -half, half},
                {-half, half, half}, {-half, half, -half}, {-1.0f, 0.0f, 0.0f});
    addWallFace({-half, half, -half}, {-half, half, half},
                {half, half, half}, {half, half, -half}, {0.0f, 1.0f, 0.0f});
    addWallFace({-half, -half, half}, {-half, -half, -half},
                {half, -half, -half}, {half, -half, half}, {0.0f, -1.0f, 0.0f});

    _wallMesh = uploadMesh(wallIndices, wallVertices);
    _wallBounds.origin = glm::vec3(0.0f);
    _wallBounds.extents = glm::vec3(0.5f);
    _wallBounds.sphereRadius = glm::length(_wallBounds.extents);

    // Unit wedge used by the Surf Ramp editor asset. It rises along local +Z:
    // the low edge is y=0 at z=-0.5 and the high edge is y=1 at z=0.5.
    std::vector<Vertex> rampVertices;
    std::vector<uint32_t> rampIndices;
    const auto addRampFace = [&](const glm::vec3& a,
                                 const glm::vec3& b,
                                 const glm::vec3& c,
                                 const glm::vec3& d,
                                 const glm::vec3& normal) {
        const uint32_t first = static_cast<uint32_t>(rampVertices.size());
        rampVertices.push_back(makeWallVertex(a, normal, 0.0f, 0.0f));
        rampVertices.push_back(makeWallVertex(b, normal, 1.0f, 0.0f));
        rampVertices.push_back(makeWallVertex(c, normal, 1.0f, 1.0f));
        rampVertices.push_back(makeWallVertex(d, normal, 0.0f, 1.0f));
        rampIndices.insert(rampIndices.end(), {
            first, first + 1, first + 2, first, first + 2, first + 3});
    };
    const auto addRampTriangle = [&](const glm::vec3& a,
                                     const glm::vec3& b,
                                     const glm::vec3& c,
                                     const glm::vec3& normal) {
        const uint32_t first = static_cast<uint32_t>(rampVertices.size());
        rampVertices.push_back(makeWallVertex(a, normal, 0.0f, 0.0f));
        rampVertices.push_back(makeWallVertex(b, normal, 1.0f, 0.0f));
        rampVertices.push_back(makeWallVertex(c, normal, 0.5f, 1.0f));
        rampIndices.insert(rampIndices.end(), {first, first + 1, first + 2});
    };
    const glm::vec3 rampLowLeft{-0.5f, 0.0f, -0.5f};
    const glm::vec3 rampLowRight{0.5f, 0.0f, -0.5f};
    const glm::vec3 rampHighRight{0.5f, 1.0f, 0.5f};
    const glm::vec3 rampHighLeft{-0.5f, 1.0f, 0.5f};
    const glm::vec3 rampBackRight{0.5f, 0.0f, 0.5f};
    const glm::vec3 rampBackLeft{-0.5f, 0.0f, 0.5f};
    addRampFace(rampLowLeft, rampHighLeft, rampHighRight, rampLowRight,
                glm::normalize(glm::vec3(0.0f, 1.0f, -1.0f)));
    addRampFace(rampLowLeft, rampLowRight, rampBackRight, rampBackLeft,
                glm::vec3(0.0f, -1.0f, 0.0f));
    addRampTriangle(rampLowRight, rampHighRight, rampBackRight,
                    glm::vec3(1.0f, 0.0f, 0.0f));
    addRampTriangle(rampLowLeft, rampBackLeft, rampHighLeft,
                    glm::vec3(-1.0f, 0.0f, 0.0f));
    addRampFace(rampBackLeft, rampBackRight, rampHighRight, rampHighLeft,
                glm::vec3(0.0f, 0.0f, 1.0f));

    _rampMesh = uploadMesh(rampIndices, rampVertices);
    _rampBounds.origin = glm::vec3(0.0f, 0.5f, 0.0f);
    _rampBounds.extents = glm::vec3(0.5f);
    _rampBounds.sphereRadius = glm::length(_rampBounds.extents);

    std::array<Vertex, 4> portalVertices{};
    portalVertices[0] = {
        .position = {-0.5f, -0.5f, 0.0f}, .uv_x = 0.0f,
        .normal = {0.0f, 0.0f, 1.0f}, .uv_y = 0.0f, .color = glm::vec4(1.0f)};
    portalVertices[1] = {
        .position = {0.5f, -0.5f, 0.0f}, .uv_x = 1.0f,
        .normal = {0.0f, 0.0f, 1.0f}, .uv_y = 0.0f, .color = glm::vec4(1.0f)};
    portalVertices[2] = {
        .position = {0.5f, 0.5f, 0.0f}, .uv_x = 1.0f,
        .normal = {0.0f, 0.0f, 1.0f}, .uv_y = 1.0f, .color = glm::vec4(1.0f)};
    portalVertices[3] = {
        .position = {-0.5f, 0.5f, 0.0f}, .uv_x = 0.0f,
        .normal = {0.0f, 0.0f, 1.0f}, .uv_y = 1.0f, .color = glm::vec4(1.0f)};
    std::array<uint32_t, 6> portalIndices{0, 1, 2, 0, 2, 3};
    _portalMesh = uploadMesh(portalIndices, portalVertices);
    _portalBounds.origin = glm::vec3(0.0f);
    _portalBounds.extents = glm::vec3(0.5f, 0.5f, 0.0f);
    _portalBounds.sphereRadius = glm::length(_portalBounds.extents);


    std::array<uint32_t, 16 * 16> checkerboard{};
    uint32_t magenta = glm::packUnorm4x8(glm::vec4(1, 0, 1, 1));
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            checkerboard[y * 16 + x] = ((x % 2) ^ (y % 2)) ? magenta : black;
        }
    }
    _errorCheckerboardImage = create_image(
        checkerboard.data(),
        {16, 16, 1},
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT);

    VkSamplerCreateInfo samplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(_device, &samplerInfo, nullptr, &_defaultSamplerNearest));
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    VK_CHECK(vkCreateSampler(_device, &samplerInfo, nullptr, &_defaultSamplerLinear));

    // The supplied asset is a 2:1 equirectangular panorama.  Horizontal
    // wrapping joins its left/right edges; clamping vertically avoids pulling
    // texels from the opposite pole when looking straight up or down.
    VkSamplerCreateInfo skyboxSamplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    skyboxSamplerInfo.magFilter = VK_FILTER_LINEAR;
    skyboxSamplerInfo.minFilter = VK_FILTER_LINEAR;
    skyboxSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    skyboxSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    skyboxSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    skyboxSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    skyboxSamplerInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(_device, &skyboxSamplerInfo, nullptr, &_skyboxSampler));

    constexpr const char* SkyboxPath = "../../assets/textures/skybox.png";
    int skyboxWidth = 0;
    int skyboxHeight = 0;
    int skyboxChannels = 0;
    stbi_uc* skyboxPixels = stbi_load(
        SkyboxPath,
        &skyboxWidth,
        &skyboxHeight,
        &skyboxChannels,
        STBI_rgb_alpha);
    if (skyboxPixels != nullptr) {
        _skyboxImage = create_image(
            skyboxPixels,
            {static_cast<uint32_t>(skyboxWidth),
             static_cast<uint32_t>(skyboxHeight),
             1},
            VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_SAMPLED_BIT);
        stbi_image_free(skyboxPixels);
        fmt::print("Loaded equirectangular skybox: {} ({}x{})\n",
                   SkyboxPath, skyboxWidth, skyboxHeight);
    } else {
        // Keep the descriptor valid even if an asset is missing on another
        // machine.  The log tells the developer why the sky is plain blue.
        fmt::print("Failed to load skybox {}: {}\n",
                   SkyboxPath,
                   stbi_failure_reason() != nullptr ? stbi_failure_reason() : "unknown");
        const uint32_t fallbackSky = glm::packUnorm4x8(glm::vec4(0.25f, 0.45f, 0.75f, 1.0f));
        _skyboxImage = create_image(
            const_cast<uint32_t*>(&fallbackSky),
            {1, 1, 1},
            VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_SAMPLED_BIT);
    }

    // The compute background needs the panorama at binding 1.  The portal
    // graphics pass uses its own single-image descriptor at set 0.
    DescriptorWriter skyboxWriter;
    skyboxWriter.write_image(
        1,
        _skyboxImage.imageView,
        _skyboxSampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    skyboxWriter.update_set(_device, _drawImageDescriptors);

    _skyboxDescriptor = globalDescriptorAllocator.allocate(
        _device, _singleImageDescriptorLayout);
    skyboxWriter.clear();
    skyboxWriter.write_image(
        0,
        _skyboxImage.imageView,
        _skyboxSampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    skyboxWriter.update_set(_device, _skyboxDescriptor);
    
    _floorMaterialBuffer = create_buffer(
    sizeof(GLTFMetallic_Roughness::MaterialConstants),
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
    VMA_MEMORY_USAGE_CPU_TO_GPU);

    auto* floorConstants =
    static_cast<GLTFMetallic_Roughness::MaterialConstants*>(
        _floorMaterialBuffer.info.pMappedData);

    *floorConstants = {};
    floorConstants->colorFactors = glm::vec4(1.0f);
    floorConstants->metal_rough_factors = glm::vec4(0.0f, 0.8f, 1.0f, 0.0f);

    GLTFMetallic_Roughness::MaterialResources floorResources{};
    floorResources.colorImage = _whiteImage;
    floorResources.colorSampler = _defaultSamplerLinear;
    floorResources.metalRoughImage = _whiteImage;
    floorResources.metalRoughSampler = _defaultSamplerLinear;
    floorResources.dataBuffer = _floorMaterialBuffer.buffer;
    floorResources.dataBufferOffset = 0;

    _floorMaterial = metalRoughMaterial.write_material(
        _device,
        MaterialPass::MainColor,
        floorResources,
        globalDescriptorAllocator);

    _wallMaterialBuffer = create_buffer(
        sizeof(GLTFMetallic_Roughness::MaterialConstants),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    auto* wallConstants = static_cast<GLTFMetallic_Roughness::MaterialConstants*>(
        _wallMaterialBuffer.info.pMappedData);
    *wallConstants = {};
    wallConstants->colorFactors = glm::vec4(0.18f, 0.32f, 0.55f, 1.0f);
    wallConstants->metal_rough_factors = glm::vec4(0.0f, 0.8f, 0.0f, 0.0f);

    GLTFMetallic_Roughness::MaterialResources wallResources = floorResources;
    wallResources.dataBuffer = _wallMaterialBuffer.buffer;
    _wallMaterial = metalRoughMaterial.write_material(
        _device,
        MaterialPass::MainColor,
        wallResources,
        globalDescriptorAllocator);

    // There is no character asset in assets/ yet, so start with a visible
    // collision-sized proxy.  It is rendered only by portal cameras; the
    // first-person main camera never sees the box enclosing itself.
    _playerBounds = _wallBounds;
    _playerMaterialBuffer = create_buffer(
        sizeof(GLTFMetallic_Roughness::MaterialConstants),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    auto* playerConstants = static_cast<GLTFMetallic_Roughness::MaterialConstants*>(
        _playerMaterialBuffer.info.pMappedData);
    *playerConstants = {};
    playerConstants->colorFactors = glm::vec4(0.95f, 0.90f, 0.75f, 1.0f);
    playerConstants->metal_rough_factors = glm::vec4(0.0f, 0.9f, 0.0f, 0.0f);

    GLTFMetallic_Roughness::MaterialResources playerResources = floorResources;
    playerResources.dataBuffer = _playerMaterialBuffer.buffer;
    _playerMaterial = metalRoughMaterial.write_material(
        _device,
        MaterialPass::MainColor,
        playerResources,
        globalDescriptorAllocator);

    _bluePortalMaterialBuffer = create_buffer(
        sizeof(GLTFMetallic_Roughness::MaterialConstants),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    auto* bluePortalConstants =
        static_cast<GLTFMetallic_Roughness::MaterialConstants*>(
            _bluePortalMaterialBuffer.info.pMappedData);
    *bluePortalConstants = {};
    // The current material shader has no emissive input yet, so use a bright
    // base color to keep the placement prototype obvious on every wall face.
    bluePortalConstants->colorFactors = glm::vec4(0.2f, 1.5f, 8.0f, 1.0f);

    GLTFMetallic_Roughness::MaterialResources bluePortalResources = floorResources;
    bluePortalResources.dataBuffer = _bluePortalMaterialBuffer.buffer;
    _bluePortalMaterial = metalRoughMaterial.write_material(
        _device,
        MaterialPass::MainColor,
        bluePortalResources,
        globalDescriptorAllocator);

    _orangePortalMaterialBuffer = create_buffer(
        sizeof(GLTFMetallic_Roughness::MaterialConstants),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    auto* orangePortalConstants =
        static_cast<GLTFMetallic_Roughness::MaterialConstants*>(
            _orangePortalMaterialBuffer.info.pMappedData);
    *orangePortalConstants = {};
    orangePortalConstants->colorFactors = glm::vec4(8.0f, 1.2f, 0.15f, 1.0f);

    GLTFMetallic_Roughness::MaterialResources orangePortalResources = floorResources;
    orangePortalResources.dataBuffer = _orangePortalMaterialBuffer.buffer;
    _orangePortalMaterial = metalRoughMaterial.write_material(
        _device,
        MaterialPass::MainColor,
        orangePortalResources,
        globalDescriptorAllocator);

    init_portal_camera_targets();

    // This model is deliberately not put in loadedScenes: that collection is
    // rendered by the main first-person camera.  We draw this one only into
    // portalViewDrawContext so the player can see their body through a portal.
    auto playerModel = loadGltf(this, "../../assets/tung_tung_tung_sahur.glb");
    if (playerModel) {
        _playerModel = *playerModel;
    } else {
        fmt::print("Failed to load tung_tung_tung_sahur.glb\n");
    }

    //auto structureScene = loadGltf(this, "../../assets/structure.glb");
    //if (structureScene) {
      //  loadedScenes["structure"] = *structureScene;
    //} else {
      //  fmt::print("Failed to load structure.glb; trying basicmesh.glb\n");
        //auto basicScene = loadGltf(this, "../../assets/basicmesh.glb");
        //if (basicScene) {
          //  loadedScenes["basicmesh"] = *basicScene;
        //}
    //}

    build_sandbox_scene();
    // A missing file simply leaves the starter sandbox intact on the first
    // launch.  After the first File > Save Scene, this restores the level.
    restore_last_editor_scene_name();
    load_editor_scene();

    _mainDeletionQueue.push_function([this]() {
        destroy_buffer(_orangePortalMaterialBuffer);
        destroy_buffer(_bluePortalMaterialBuffer);
        destroy_buffer(_portalMesh.vertexBuffer);
        destroy_buffer(_portalMesh.indexBuffer);
        destroy_buffer(_wallMaterialBuffer);
        destroy_buffer(_rampMesh.vertexBuffer);
        destroy_buffer(_rampMesh.indexBuffer);
        destroy_buffer(_wallMesh.vertexBuffer);
        destroy_buffer(_wallMesh.indexBuffer);
        destroy_buffer(_playerMaterialBuffer);
        destroy_buffer(_floorMaterialBuffer);
        destroy_buffer(_floorMesh.vertexBuffer);
        destroy_buffer(_floorMesh.indexBuffer);
        vkDestroySampler(_device, _skyboxSampler, nullptr);
        vkDestroySampler(_device, _defaultSamplerNearest, nullptr);
        vkDestroySampler(_device, _defaultSamplerLinear, nullptr);
        destroy_image(_skyboxImage);
        destroy_image(_whiteImage);
        destroy_image(_greyImage);
        destroy_image(_blackImage);
        destroy_image(_errorCheckerboardImage);
    });
}

void VulkanEngine::init_portal_camera_targets()
{
    const VkExtent3D targetExtent{
        _portalCameraExtent.width,
        _portalCameraExtent.height,
        1};
    for (AllocatedImage& image : _portalCameraImages) {
        image = create_image(
            targetExtent,
            _drawImage.imageFormat,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }
    _portalCameraDepthImage = create_image(
        targetExtent,
        _depthImage.imageFormat,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);

    for (uint32_t index = 0; index < PortalCameraTargetCount; ++index) {
        _portalCameraMaterialBuffers[index] = create_buffer(
            sizeof(GLTFMetallic_Roughness::MaterialConstants),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU);
        auto* constants = static_cast<GLTFMetallic_Roughness::MaterialConstants*>(
            _portalCameraMaterialBuffers[index].info.pMappedData);
        *constants = {};
        constants->colorFactors = glm::vec4(1.0f);

        GLTFMetallic_Roughness::MaterialResources resources{};
        resources.colorImage = _portalCameraImages[index];
        resources.colorSampler = _defaultSamplerLinear;
        resources.metalRoughImage = _whiteImage;
        resources.metalRoughSampler = _defaultSamplerLinear;
        resources.dataBuffer = _portalCameraMaterialBuffers[index].buffer;
        _portalCameraMaterials[index] = metalRoughMaterial.write_material(
            _device,
            MaterialPass::MainColor,
            resources,
            globalDescriptorAllocator);
        _portalCameraMaterials[index].pipeline =
            &metalRoughMaterial.portalCompositePipeline;
    }

    _mainDeletionQueue.push_function([this]() {
        for (const AllocatedBuffer& buffer : _portalCameraMaterialBuffers) {
            destroy_buffer(buffer);
        }
        for (const AllocatedImage& image : _portalCameraImages) {
            destroy_image(image);
        }
        destroy_image(_portalCameraDepthImage);
    });
}

void MeshNode::Draw(const glm::mat4& topMatrix, DrawContext& ctx)
{
    if (mesh == nullptr || mesh->meshBuffers.indexBuffer.buffer == VK_NULL_HANDLE) {
        Node::Draw(topMatrix, ctx);
        return;
    }

    const glm::mat4 nodeMatrix = topMatrix * worldTransform;
    for (auto& surface : mesh->surfaces) {
        RenderObject object{};
        object.indexCount = surface.count;
        object.firstIndex = surface.startIndex;
        object.indexBuffer = mesh->meshBuffers.indexBuffer.buffer;
        object.material = surface.material ? &surface.material->data : nullptr;
        object.bounds = surface.bounds;
        object.transform = nodeMatrix;
        object.vertexBufferAddress = mesh->meshBuffers.vertexBufferAddress;
        if (object.material != nullptr) {
            if (object.material->passType == MaterialPass::Transparent) {
                ctx.TransparentSurfaces.push_back(object);
            } else {
                ctx.OpaqueSurfaces.push_back(object);
            }
        }
    }
    Node::Draw(topMatrix, ctx);
}

// Draws one hierarchy row plus its children.  Selection is the only edit the
// first version of the panel performs.
static void draw_hierarchy_node(
    Scene& scene,
    SceneObjectID id,
    SceneObjectID& selected)
{
    const SceneObject* object = scene.get(id);
    if (object == nullptr) {
        return;
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_SpanAvailWidth |
        ImGuiTreeNodeFlags_DefaultOpen;
    if (object->children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (id == selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    const bool opened = ImGui::TreeNodeEx(
        reinterpret_cast<void*>(static_cast<uintptr_t>(id)),
        flags,
        "%s",
        object->name.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selected = id;
    }

    // Dragging a static object onto another static object makes it a child.
    // Physics/player/portal-driven nodes deliberately stay fixed so the editor
    // cannot create a hierarchy their owner overwrites next frame.
    if (!object->transformDrivenExternally &&
        ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("SCENE_OBJECT", &id, sizeof(id));
        ImGui::Text("Parent %s", object->name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                "SCENE_OBJECT")) {
            if (payload->DataSize == sizeof(SceneObjectID)) {
                const SceneObjectID child = *static_cast<const SceneObjectID*>(
                    payload->Data);
                const SceneObject* childObject = scene.get(child);
                if (childObject != nullptr &&
                    !childObject->transformDrivenExternally &&
                    scene.set_parent(child, id)) {
                    selected = child;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (opened && !object->children.empty()) {
        for (const SceneObjectID child : object->children) {
            draw_hierarchy_node(scene, child, selected);
        }
        ImGui::TreePop();
    }
}

void VulkanEngine::draw_hierarchy_panel()
{
    if (ImGui::Begin("Hierarchy")) {
        for (const SceneObject& object : _scene.objects) {
            if (object.alive && object.parent == InvalidSceneObject) {
                draw_hierarchy_node(_scene, object.id, _selectedSceneObject);
            }
        }
    }
    ImGui::End();
}

void VulkanEngine::draw_inspector_panel()
{
    if (ImGui::Begin("Inspector")) {
        SceneObject* object = _scene.get(_selectedSceneObject);
        if (object == nullptr) {
            ImGui::TextUnformatted("No object selected.");
        } else {
            ImGui::Text("Name: %s", object->name.c_str());
            ImGui::Separator();

            // Transforms written by physics or portal placement would be
            // overwritten again next frame, so show them read-only.
            const bool driven = object->transformDrivenExternally;
            bool changed = false;
            if (driven) {
                ImGui::TextUnformatted("Transform is driven by the simulation.");
            }
            ImGui::BeginDisabled(driven);
            changed |= ImGui::DragFloat3(
                "Position", &object->localTransform.position.x, 0.05f);

            glm::vec3 rotationDegrees = glm::degrees(object->localTransform.rotation);
            if (ImGui::DragFloat3("Rotation", &rotationDegrees.x, 0.5f)) {
                object->localTransform.rotation = glm::radians(rotationDegrees);
                changed = true;
            }

            changed |= ImGui::DragFloat3(
                "Scale", &object->localTransform.scale.x, 0.05f);
            ImGui::EndDisabled();

            ImGui::Separator();
            changed |= ImGui::Checkbox("Visible", &object->visible);
            changed |= ImGui::Checkbox("Has Collision", &object->hasCollision);
            changed |= ImGui::Checkbox("Portal Placeable", &object->portalPlaceable);
            if (object->timeTrialRole != TimeTrialRole::None) {
                const char* roleLabel = object->timeTrialRole == TimeTrialRole::SpawnPoint
                    ? "Spawn Point"
                    : (object->timeTrialRole == TimeTrialRole::StartTrigger
                        ? "Start Timer Trigger"
                        : "Finish Timer Trigger");
                ImGui::TextDisabled("Time-trial role: %s", roleLabel);
            }
            const bool needsEditableBox = object->hasCollision ||
                object->timeTrialRole == TimeTrialRole::StartTrigger ||
                object->timeTrialRole == TimeTrialRole::FinishTrigger;
            if (needsEditableBox &&
                object->collisionShape == CollisionShape::Box) {
                ImGui::SeparatorText("Box Collider");
                changed |= ImGui::DragFloat3(
                    "Center", &object->colliderCenter.x, 0.05f);
                if (ImGui::DragFloat3(
                        "Half Extents",
                        &object->colliderHalfExtents.x,
                        0.05f,
                        0.01f,
                        1000.0f)) {
                    object->colliderHalfExtents = glm::max(
                        object->colliderHalfExtents, glm::vec3(0.01f));
                    changed = true;
                }
                ImGui::TextDisabled(
                    "Center/size are local to this actor. Full size = half extents x 2.");
            } else if (object->hasCollision &&
                       object->collisionShape == CollisionShape::SurfRamp) {
                ImGui::SeparatorText("Surf Ramp Collider");
                ImGui::TextDisabled("Local +Z rises from the low edge to the high edge.");
                ImGui::TextDisabled("Scale X/Y/Z controls width, height, and length.");
                ImGui::TextDisabled("Steep ramps use surf physics; do not use a zero scale.");
            }
            if (changed && !driven) {
                _sceneDirty = true;
                rebuild_collision_from_scene();
            }

            if ((_bluePortal.placed && _bluePortal.hostWallObject == object->id) ||
                (_orangePortal.placed && _orangePortal.hostWallObject == object->id)) {
                ImGui::TextDisabled(
                    "Translation gizmo is disabled while a portal is attached.");
            }

            if (object->hasCollision &&
                object->collisionShape != CollisionShape::GroundPlane) {
                const AABB collider = collider_from_object(
                    _scene, object->id);
                ImGui::Text(
                    "Collider min %.2f %.2f %.2f",
                    collider.min.x, collider.min.y, collider.min.z);
                ImGui::Text(
                    "Collider max %.2f %.2f %.2f",
                    collider.max.x, collider.max.y, collider.max.z);
            }

            const bool isRequiredSandboxObject =
                object->id == _sandboxRoot ||
                object->id == _floorObject ||
                object->transformDrivenExternally;
            const bool hostsPlacedPortal =
                (_bluePortal.placed && _bluePortal.hostWallObject == object->id) ||
                (_orangePortal.placed && _orangePortal.hostWallObject == object->id);
            ImGui::Separator();
            ImGui::BeginDisabled(isRequiredSandboxObject || hostsPlacedPortal);
            if (ImGui::Button("Delete Selected")) {
                delete_selected_scene_object();
            }
            ImGui::EndDisabled();
            if (isRequiredSandboxObject) {
                ImGui::TextDisabled("Sandbox-owned objects cannot be deleted.");
            } else if (hostsPlacedPortal) {
                ImGui::TextDisabled("Remove or move the attached portal first.");
            } else {
                ImGui::TextDisabled("Shortcut: Delete");
            }
        }
    }
    ImGui::End();
}

void VulkanEngine::draw_editor_gizmo()
{
    SceneObject* object = _scene.get(_selectedSceneObject);
    if (object == nullptr || object->transformDrivenExternally) {
        return;
    }

    // A portal's placement data is still owned by the portal system. Moving
    // its host wall while it has a placed portal would leave that portal off
    // the wall, so block the gizmo until portal parenting is introduced.
    if ((_bluePortal.placed && _bluePortal.hostWallObject == object->id) ||
        (_orangePortal.placed && _orangePortal.hostWallObject == object->id)) {
        return;
    }

    const Camera& camera = render_camera();
    // ImGuizmo draws in ImGui's screen space, which performs its own Y flip.
    // Do not pass the renderer's Vulkan-flipped, reversed-Z projection here:
    // that made the overlay behave as if it were attached to the camera
    // rather than the selected world-space object.
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    const glm::mat4 gizmoProjection = glm::perspective(
        glm::radians(70.0f),
        displaySize.x / std::max(displaySize.y, 1.0f),
        0.1f,
        10000.0f);
    glm::mat4 worldTransform = _scene.world_matrix(object->id);

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
    ImGuizmo::SetRect(0.0f, 0.0f, displaySize.x, displaySize.y);

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    float snapValues[3]{_translationSnap, _translationSnap, _translationSnap};
    switch (_gizmoOperation) {
    case EditorGizmoOperation::Translate:
        operation = ImGuizmo::TRANSLATE;
        break;
    case EditorGizmoOperation::Rotate:
        operation = ImGuizmo::ROTATE;
        snapValues[0] = _rotationSnapDegrees;
        break;
    case EditorGizmoOperation::Scale:
        operation = ImGuizmo::SCALE;
        snapValues[0] = _scaleSnap;
        break;
    }

    if (!ImGuizmo::Manipulate(
            glm::value_ptr(camera.getViewMatrix()),
            glm::value_ptr(gizmoProjection),
            operation,
            _gizmoLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
            glm::value_ptr(worldTransform),
            nullptr,
            _gizmoSnapping ? snapValues : nullptr)) {
        return;
    }

    // ImGuizmo edits a world matrix. Scene objects store local components, so
    // convert back through the parent's world matrix before saving the change.
    glm::mat4 parentWorld{1.0f};
    if (object->parent != InvalidSceneObject) {
        parentWorld = _scene.world_matrix(object->parent);
    }
    const glm::mat4 localTransform = glm::inverse(parentWorld) * worldTransform;
    object->localTransform = transform_from_matrix(localTransform);

    // Keep collision and portal raycasts coherent while the arrow is dragged.
    _sceneDirty = true;
    rebuild_collision_from_scene();
}

void VulkanEngine::select_scene_object_at_screen_position(
    int screenX,
    int screenY)
{
    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSize(_window, &windowWidth, &windowHeight);
    if (windowWidth <= 0 || windowHeight <= 0) {
        return;
    }

    // Convert the mouse position into a world-space ray through the editor
    // camera. The projection has Vulkan's flipped Y, so screen Y maps to NDC
    // in the ordinary top-to-bottom SDL direction here.
    const float ndcX = 2.0f * static_cast<float>(screenX) /
            static_cast<float>(windowWidth) - 1.0f;
    const float ndcY = 2.0f * static_cast<float>(screenY) /
            static_cast<float>(windowHeight) - 1.0f;
    const GPUSceneData cameraData = build_scene_data(
        _editorCamera.getViewMatrix());
    const glm::mat4 inverseViewProjection = glm::inverse(cameraData.viewproj);
    glm::vec4 farPoint = inverseViewProjection * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    farPoint /= farPoint.w;

    const glm::vec3 rayOrigin = _editorCamera.position;
    const glm::vec3 rayDirection = glm::normalize(
        glm::vec3(farPoint) - rayOrigin);

    std::optional<RaycastHit> closestHit;
    SceneObjectID closestObject = InvalidSceneObject;
    for (const SceneObject& object : _scene.objects) {
        // Version 1 selects the scene's axis-aligned editable primitives.
        // Imported models get mesh picking later, once their bounds are
        // represented in the scene system.
        if (!object.alive || !object.visible || !object.hasCollision ||
            object.collisionShape == CollisionShape::GroundPlane) {
            continue;
        }

        const std::optional<RaycastHit> hit = raycast_aabb(
            rayOrigin,
            rayDirection,
            collider_from_object(_scene, object.id));
        if (hit.has_value() &&
            (!closestHit.has_value() || hit->distance < closestHit->distance)) {
            closestHit = hit;
            closestObject = object.id;
        }
    }

    if (closestObject != InvalidSceneObject) {
        _selectedSceneObject = closestObject;
    }
}

void VulkanEngine::draw_editor_menu()
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
            save_editor_scene();
        }
        if (ImGui::MenuItem("Save Scene As...")) {
            _sceneNameInput.fill('\0');
            const size_t copyLength = std::min(
                _activeSceneFilename.size(), _sceneNameInput.size() - 1);
            std::memcpy(
                _sceneNameInput.data(), _activeSceneFilename.data(), copyLength);
            ImGui::OpenPopup("Save Scene As");
        }
        if (ImGui::MenuItem("Reload Current Scene")) {
            load_editor_scene();
        }
        if (ImGui::BeginMenu("Open Scene")) {
            std::vector<std::string> sceneFilenames;
            std::error_code directoryError;
            for (const std::filesystem::directory_entry& entry :
                 std::filesystem::directory_iterator(
                     EditorSceneDirectory, directoryError)) {
                if (directoryError) {
                    break;
                }
                if (entry.is_regular_file() && entry.path().extension() == ".json") {
                    sceneFilenames.push_back(entry.path().filename().string());
                }
            }
            std::sort(sceneFilenames.begin(), sceneFilenames.end());
            if (sceneFilenames.empty()) {
                ImGui::TextDisabled("No saved scenes yet.");
            }
            for (const std::string& filename : sceneFilenames) {
                if (ImGui::MenuItem(
                        filename.c_str(),
                        nullptr,
                        filename == _activeSceneFilename)) {
                    load_editor_scene_named(filename);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::TextDisabled("../../assets/scenes/%s%s",
            _activeSceneFilename.c_str(),
            _sceneDirty ? "  (unsaved changes)" : "");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Create")) {
        if (ImGui::MenuItem("Empty Actor")) {
            create_editor_actor("Actor", SceneAssetKind::None, false);
        }
        if (ImGui::MenuItem("Cube")) {
            create_editor_actor("Cube", SceneAssetKind::UnitCube, false);
        }
        if (ImGui::MenuItem("Wall")) {
            create_editor_actor(
                "Wall", SceneAssetKind::UnitCube, true, true);
        }
        if (ImGui::MenuItem("Floor Platform")) {
            create_editor_actor("Floor", SceneAssetKind::FloorQuad, true);
        }
        if (ImGui::MenuItem("Surf Ramp")) {
            create_editor_actor("Surf Ramp", SceneAssetKind::SurfRamp, true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Spawn Point")) {
            // This first time-trial loop uses one respawn marker. Replacing
            // it leaves the old marker as an ordinary editable cube.
            for (SceneObject& object : _scene.objects) {
                if (object.alive && object.timeTrialRole == TimeTrialRole::SpawnPoint) {
                    object.timeTrialRole = TimeTrialRole::None;
                }
            }
            create_editor_actor(
                "Spawn Point", SceneAssetKind::UnitCube, false, false,
                TimeTrialRole::SpawnPoint);
        }
        if (ImGui::MenuItem("Start Timer Trigger")) {
            create_editor_actor(
                "Start Trigger", SceneAssetKind::UnitCube, false, false,
                TimeTrialRole::StartTrigger);
        }
        if (ImGui::MenuItem("Finish Timer Trigger")) {
            create_editor_actor(
                "Finish Trigger", SceneAssetKind::UnitCube, false, false,
                TimeTrialRole::FinishTrigger);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Import GLB/glTF...")) {
            _gltfPathInput.fill('\0');
            ImGui::OpenPopup("Import GLB/glTF");
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        const bool hasSelection = _scene.get(_selectedSceneObject) != nullptr;
        if (ImGui::BeginMenu("Gizmo")) {
            if (ImGui::MenuItem(
                    "Move", "W",
                    _gizmoOperation == EditorGizmoOperation::Translate)) {
                _gizmoOperation = EditorGizmoOperation::Translate;
            }
            if (ImGui::MenuItem(
                    "Rotate", "E",
                    _gizmoOperation == EditorGizmoOperation::Rotate)) {
                _gizmoOperation = EditorGizmoOperation::Rotate;
            }
            if (ImGui::MenuItem(
                    "Scale", "R",
                    _gizmoOperation == EditorGizmoOperation::Scale)) {
                _gizmoOperation = EditorGizmoOperation::Scale;
            }
            ImGui::Separator();
            ImGui::MenuItem("Local space", nullptr, &_gizmoLocalSpace);
            ImGui::MenuItem("Enable snapping", "S", &_gizmoSnapping);
            if (_gizmoOperation == EditorGizmoOperation::Translate) {
                ImGui::DragFloat("Move snap", &_translationSnap, 0.05f, 0.05f, 10.0f);
            } else if (_gizmoOperation == EditorGizmoOperation::Rotate) {
                ImGui::DragFloat(
                    "Rotation snap", &_rotationSnapDegrees, 1.0f, 1.0f, 180.0f);
            } else {
                ImGui::DragFloat("Scale snap", &_scaleSnap, 0.05f, 0.01f, 10.0f);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Duplicate Selected", "Ctrl+D", false, hasSelection)) {
            duplicate_selected_scene_object();
        }
        if (ImGui::MenuItem("Delete Selected", "Delete", false, hasSelection)) {
            delete_selected_scene_object();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Portals")) {
        if (ImGui::MenuItem("Retract Both", "R")) {
            retract_portals();
        }
        ImGui::Separator();
        ImGui::MenuItem(
            "Use Offscreen Camera Experiment",
            nullptr,
            &_useOffscreenPortalCameras);
        ImGui::BeginDisabled(_useOffscreenPortalCameras);
        ImGui::MenuItem(
            "Direct Stencil Recursion",
            nullptr,
            &_portalRecursionEnabled);
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Time Trial")) {
        if (ImGui::MenuItem("Reset Run")) {
            reset_time_trial();
        }
        ImGui::TextDisabled(
            "Create Spawn Point, Start Timer Trigger, and Finish Timer Trigger.");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window")) {
        ImGui::MenuItem("Show Debug Panels", nullptr, &_showDebugPanels);
        if (ImGui::MenuItem("Reset Editor Layout")) {
            _resetEditorLayoutRequested = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem(
            "Show Collider Bounds",
            nullptr,
            &_showColliderBounds,
            _editorMode);
        if (!_editorMode) {
            ImGui::TextDisabled("Enter Edit Mode to show collider bounds.");
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    if (ImGui::Button("Play")) {
        set_editor_mode(false);
    }
    ImGui::EndMainMenuBar();

    if (ImGui::BeginPopupModal(
            "Save Scene As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Scene files are stored in assets/scenes.");
        ImGui::InputText("Filename", _sceneNameInput.data(), _sceneNameInput.size());
        ImGui::TextDisabled(".json is added automatically.");
        if (ImGui::Button("Save")) {
            if (save_editor_scene_as(_sceneNameInput.data())) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal(
            "Import GLB/glTF", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Enter an asset path relative to bin/Debug.");
        ImGui::TextDisabled("Example: ../../assets/my_model.glb");
        ImGui::InputText("Asset path", _gltfPathInput.data(), _gltfPathInput.size());
        if (ImGui::Button("Import")) {
            if (import_gltf_actor(_gltfPathInput.data()) != InvalidSceneObject) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void VulkanEngine::setup_default_dock_layout(uint32_t dockspaceID)
{
    // A saved ImGui layout wins after the first run.  This block only builds
    // the initial editor arrangement, or runs again from Window > Reset.
    if (!_resetEditorLayoutRequested && ImGui::DockBuilderGetNode(dockspaceID) != nullptr) {
        return;
    }

    _resetEditorLayoutRequested = false;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspaceID);
    ImGui::DockBuilderAddNode(
        dockspaceID,
        ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dockspaceID, viewport->WorkSize);

    ImGuiID centerID = dockspaceID;
    ImGuiID leftID = ImGui::DockBuilderSplitNode(
        centerID, ImGuiDir_Left, 0.22f, nullptr, &centerID);
    ImGuiID rightID = ImGui::DockBuilderSplitNode(
        centerID, ImGuiDir_Right, 0.27f, nullptr, &centerID);
    ImGuiID bottomID = ImGui::DockBuilderSplitNode(
        centerID, ImGuiDir_Down, 0.25f, nullptr, &centerID);

    ImGui::DockBuilderDockWindow("Hierarchy", leftID);
    ImGui::DockBuilderDockWindow("Inspector", rightID);
    ImGui::DockBuilderDockWindow("Render Settings", bottomID);
    ImGui::DockBuilderDockWindow("Movement Tuning", bottomID);
    ImGui::DockBuilderDockWindow("Statistics", bottomID);
    ImGui::DockBuilderFinish(dockspaceID);
}

bool VulkanEngine::delete_selected_scene_object()
{
    SceneObject* object = _scene.get(_selectedSceneObject);
    if (object == nullptr) {
        return false;
    }

    // These objects are owned by the sandbox/player/portal systems. They
    // need a dedicated replacement workflow rather than a generic delete.
    const bool isRequiredSandboxObject =
        object->id == _sandboxRoot ||
        object->id == _floorObject ||
        object->transformDrivenExternally;
    const bool hostsPlacedPortal =
        (_bluePortal.placed && _bluePortal.hostWallObject == object->id) ||
        (_orangePortal.placed && _orangePortal.hostWallObject == object->id);
    if (isRequiredSandboxObject || hostsPlacedPortal) {
        return false;
    }

    if (!_scene.destroy_object(object->id)) {
        return false;
    }

    _selectedSceneObject = InvalidSceneObject;
    _sceneDirty = true;
    rebuild_collision_from_scene();
    return true;
}

bool VulkanEngine::duplicate_selected_scene_object()
{
    const SceneObject* source = _scene.get(_selectedSceneObject);
    if (source == nullptr || source->id == _sandboxRoot ||
        source->id == _floorObject || source->transformDrivenExternally) {
        return false;
    }
    for (const SceneObject* current = source;
         current != nullptr;
         current = _scene.get(current->parent)) {
        if (current->transformDrivenExternally) {
            return false;
        }
    }

    // Copy the scene-facing data, then rebuild the primitive from its stable
    // asset kind. Copying raw VkBuffer/material pointers is unnecessary and
    // would make persistence harder to reason about.
    const SceneObjectID duplicateID = _scene.create_object(
        source->name + " Copy " + std::to_string(_nextCreatedActorNumber++),
        source->parent);
    SceneObject* duplicate = _scene.get(duplicateID);
    if (duplicate == nullptr) {
        return false;
    }

    duplicate->localTransform = source->localTransform;
    duplicate->localTransform.position += glm::vec3(0.5f, 0.0f, 0.5f);
    duplicate->visible = source->visible;
    duplicate->hasCollision = source->hasCollision;
    duplicate->portalPlaceable = source->portalPlaceable;
    duplicate->layer = source->layer;
    duplicate->collisionShape = source->collisionShape;
    duplicate->colliderCenter = source->colliderCenter;
    duplicate->colliderHalfExtents = source->colliderHalfExtents;
    duplicate->timeTrialRole = source->timeTrialRole;
    duplicate->modelPath = source->modelPath;
    if (source->assetKind == SceneAssetKind::ImportedGLTF) {
        // Instances of the same imported scene can share its loaded GPU data.
        duplicate->assetKind = SceneAssetKind::ImportedGLTF;
        duplicate->model = source->model;
    } else {
        assign_scene_asset(*duplicate, source->assetKind);
    }

    _selectedSceneObject = duplicateID;
    _sceneDirty = true;
    rebuild_collision_from_scene();
    return true;
}

SceneObjectID VulkanEngine::create_editor_actor(
    const char* baseName,
    SceneAssetKind assetKind,
    bool collidable,
    bool portalPlaceable,
    TimeTrialRole timeTrialRole)
{
    const std::string name = std::string(baseName) + " " +
        std::to_string(_nextCreatedActorNumber++);
    const SceneObjectID id = _scene.create_object(name, _sandboxRoot);
    SceneObject* object = _scene.get(id);
    if (object == nullptr) {
        return InvalidSceneObject;
    }
    object->timeTrialRole = timeTrialRole;

    const Camera& camera = render_camera();
    const glm::vec3 forward = glm::normalize(glm::vec3(
        camera.getRotationMatrix() * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
    const glm::vec3 spawnWorldPosition = camera.position + forward * 5.0f;
    const glm::mat4 parentWorld = _sandboxRoot == InvalidSceneObject
        ? glm::mat4(1.0f)
        : _scene.world_matrix(_sandboxRoot);
    object->localTransform.position = glm::vec3(
        glm::inverse(parentWorld) * glm::vec4(spawnWorldPosition, 1.0f));

    if (assetKind != SceneAssetKind::None) {
        assign_scene_asset(*object, assetKind);
    }
    if (assetKind == SceneAssetKind::UnitCube) {
        object->localTransform.scale = collidable
            ? glm::vec3(4.0f, 3.0f, 0.5f)
            : glm::vec3(1.0f);
    } else if (assetKind == SceneAssetKind::FloorQuad) {
        object->localTransform.scale = glm::vec3(10.0f, 1.0f, 10.0f);
        object->collisionShape = CollisionShape::GroundPlane;
    } else if (assetKind == SceneAssetKind::SurfRamp) {
        object->localTransform.scale = glm::vec3(8.0f, 12.0f, 12.0f);
        object->collisionShape = CollisionShape::SurfRamp;
        // Matches the wedge's local bounding box, for editor picking/debug.
        object->colliderCenter = glm::vec3(0.0f, 0.5f, 0.0f);
        object->colliderHalfExtents = glm::vec3(0.5f);
    }

    if (timeTrialRole == TimeTrialRole::SpawnPoint) {
        // A low marker shows the exact feet position used by fall reset.
        object->localTransform.scale = glm::vec3(0.6f, 0.12f, 0.6f);
    } else if (timeTrialRole == TimeTrialRole::StartTrigger ||
               timeTrialRole == TimeTrialRole::FinishTrigger) {
        // A trigger is a non-solid volume. Its visible cube and editable
        // local box use the same dimensions, so what you see is what starts
        // or finishes the run.
        object->localTransform.scale = glm::vec3(2.0f, 1.0f, 2.0f);
        object->colliderCenter = glm::vec3(0.0f);
        object->colliderHalfExtents = glm::vec3(0.5f);
    }

    object->hasCollision = collidable;
    object->portalPlaceable = portalPlaceable;
    _selectedSceneObject = id;
    _sceneDirty = true;
    rebuild_collision_from_scene();
    return id;
}

SceneObjectID VulkanEngine::import_gltf_actor(std::string_view modelPath)
{
    if (modelPath.empty()) {
        return InvalidSceneObject;
    }

    auto model = loadGltf(this, std::filesystem::path(modelPath));
    if (!model) {
        fmt::print("Failed to import glTF actor: {}\n", modelPath);
        return InvalidSceneObject;
    }

    const std::filesystem::path path(modelPath);
    std::string baseName = path.stem().string();
    if (baseName.empty()) {
        baseName = "GLTF Actor";
    }
    const SceneObjectID id = _scene.create_object(
        baseName + " " + std::to_string(_nextCreatedActorNumber++),
        _sandboxRoot);
    SceneObject* object = _scene.get(id);
    if (object == nullptr) {
        return InvalidSceneObject;
    }

    const Camera& camera = render_camera();
    const glm::vec3 forward = glm::normalize(glm::vec3(
        camera.getRotationMatrix() * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
    const glm::mat4 parentWorld = _scene.world_matrix(_sandboxRoot);
    const glm::vec3 spawnWorldPosition = camera.position + forward * 5.0f;
    object->localTransform.position = glm::vec3(
        glm::inverse(parentWorld) * glm::vec4(spawnWorldPosition, 1.0f));
    object->assetKind = SceneAssetKind::ImportedGLTF;
    object->modelPath = std::string(modelPath);
    object->model = *model;

    _selectedSceneObject = id;
    _sceneDirty = true;
    return id;
}

void VulkanEngine::assign_scene_asset(
    SceneObject& object,
    SceneAssetKind assetKind)
{
    object.assetKind = assetKind;
    object.primitive = {};
    object.model.reset();

    switch (assetKind) {
    case SceneAssetKind::FloorQuad:
        object.primitive = MeshPrimitive{
            .indexCount = 6,
            .firstIndex = 0,
            .indexBuffer = _floorMesh.indexBuffer.buffer,
            .vertexBufferAddress = _floorMesh.vertexBufferAddress,
            .bounds = _floorBounds,
            .material = &_floorMaterial};
        break;
    case SceneAssetKind::UnitCube:
        object.primitive = MeshPrimitive{
            .indexCount = 36,
            .firstIndex = 0,
            .indexBuffer = _wallMesh.indexBuffer.buffer,
            .vertexBufferAddress = _wallMesh.vertexBufferAddress,
            .bounds = _wallBounds,
            .material = &_wallMaterial};
        break;
    case SceneAssetKind::SurfRamp:
        object.primitive = MeshPrimitive{
            .indexCount = 24,
            .firstIndex = 0,
            .indexBuffer = _rampMesh.indexBuffer.buffer,
            .vertexBufferAddress = _rampMesh.vertexBufferAddress,
            .bounds = _rampBounds,
            .material = &_wallMaterial};
        break;
    case SceneAssetKind::ImportedGLTF: {
        if (object.modelPath.empty()) {
            fmt::print("Scene glTF actor '{}' has no model path.\n", object.name);
            break;
        }
        auto model = loadGltf(this, object.modelPath);
        if (model) {
            object.model = *model;
        } else {
            fmt::print(
                "Failed to load scene glTF '{}' for actor '{}'.\n",
                object.modelPath,
                object.name);
        }
        break;
    }
    case SceneAssetKind::None:
        break;
    }

    // Reuse the bright portal/player materials for level-authoring markers.
    // Material pointers are rebuilt from the saved role every time a scene is
    // loaded, just like the primitive itself.
    if (object.primitive.valid()) {
        if (object.timeTrialRole == TimeTrialRole::SpawnPoint) {
            object.primitive.material = &_playerMaterial;
        } else if (object.timeTrialRole == TimeTrialRole::StartTrigger) {
            object.primitive.material = &_bluePortalMaterial;
        } else if (object.timeTrialRole == TimeTrialRole::FinishTrigger) {
            object.primitive.material = &_orangePortalMaterial;
        }
    }
}

void VulkanEngine::create_runtime_scene_objects()
{
    // These are simulation-owned. They are recreated after loading a level,
    // rather than being serialized with the editor-authored geometry.
    _playerObject = _scene.create_object("Player", _sandboxRoot);
    if (SceneObject* player = _scene.get(_playerObject)) {
        player->transformDrivenExternally = true;
        player->layer = RenderLayer::PortalViewOnly;
    }

    _playerModelObject = _scene.create_object("Player Model", _playerObject);
    if (SceneObject* playerModel = _scene.get(_playerModelObject)) {
        playerModel->layer = RenderLayer::PortalViewOnly;
        if (_playerModel) {
            playerModel->model = _playerModel;
            // The source glTF is about 14 units tall at scale 1.0.
            playerModel->localTransform.scale = glm::vec3(0.12f);
        } else {
            const PlayerMovementSettings& settings = _playerMovement.settings;
            assign_scene_asset(*playerModel, SceneAssetKind::UnitCube);
            playerModel->primitive.material = &_playerMaterial;
            playerModel->primitive.bounds = _playerBounds;
            playerModel->localTransform.position = glm::vec3(
                0.0f, settings.playerHeight * 0.5f, 0.0f);
            playerModel->localTransform.scale = glm::vec3(
                settings.playerHalfWidth * 2.0f,
                settings.playerHeight,
                settings.playerHalfWidth * 2.0f);
        }
    }

    const auto createPortalObject = [&](const char* name) {
        const SceneObjectID id = _scene.create_object(name, _sandboxRoot);
        if (SceneObject* portalObject = _scene.get(id)) {
            portalObject->visible = false;
            portalObject->transformDrivenExternally = true;
        }
        return id;
    };
    _bluePortalObject = createPortalObject("Blue Portal");
    _orangePortalObject = createPortalObject("Orange Portal");
}

bool VulkanEngine::save_editor_scene()
{
    const std::filesystem::path scenePath = editor_scene_path(_activeSceneFilename);
    std::error_code directoryError;
    std::filesystem::create_directories(scenePath.parent_path(), directoryError);
    if (directoryError) {
        fmt::print("Could not create scene directory: {}\n", directoryError.message());
        return false;
    }

    std::ofstream file(scenePath, std::ios::trunc);
    if (!file) {
        fmt::print("Could not save scene: {}\n", scenePath.string());
        return false;
    }

    file << std::setprecision(9);
    file << "{\n  \"version\": " << EditorSceneVersion
         << ",\n  \"nextActor\": " << _nextCreatedActorNumber
         << ",\n  \"objects\": [\n";

    bool firstObject = true;
    const auto writeVec3 = [&](const glm::vec3& value) {
        file << '[' << value.x << ", " << value.y << ", " << value.z << ']';
    };
    const auto runtime_owned = [&](const SceneObject& object) {
        for (const SceneObject* current = &object;
             current != nullptr;
             current = _scene.get(current->parent)) {
            if (current->transformDrivenExternally) {
                return true;
            }
        }
        return false;
    };
    for (const SceneObject& object : _scene.objects) {
        // Physics/player and the portal system rebuild their objects from
        // current runtime state.  Saving them would make stale data win on
        // the next launch.
        if (!object.alive || runtime_owned(object)) {
            continue;
        }
        if (!firstObject) {
            file << ",\n";
        }
        firstObject = false;
        file << "    {\"id\": " << object.id
             << ", \"parent\": "
             << (object.parent == InvalidSceneObject
                    ? -1
                    : static_cast<int64_t>(object.parent))
             << ", \"name\": \"" << json_escape(object.name) << "\""
             << ", \"position\": ";
        writeVec3(object.localTransform.position);
        file << ", \"rotation\": ";
        writeVec3(object.localTransform.rotation);
        file << ", \"scale\": ";
        writeVec3(object.localTransform.scale);
        file << ", \"colliderCenter\": ";
        writeVec3(object.colliderCenter);
        file << ", \"colliderHalfExtents\": ";
        writeVec3(object.colliderHalfExtents);
        file << ", \"visible\": " << (object.visible ? "true" : "false")
             << ", \"hasCollision\": " << (object.hasCollision ? "true" : "false")
             << ", \"portalPlaceable\": "
             << (object.portalPlaceable ? "true" : "false")
             << ", \"layer\": " << static_cast<int>(object.layer)
             << ", \"collisionShape\": "
             << static_cast<int>(object.collisionShape)
             << ", \"timeTrialRole\": \""
             << time_trial_role_name(object.timeTrialRole)
             << "\", \"asset\": \"" << scene_asset_name(object.assetKind)
             << "\", \"modelPath\": \""
             << json_escape(object.modelPath) << "\"}";
    }
    file << "\n  ]\n}\n";
    if (!file) {
        fmt::print("Could not finish writing scene: {}\n", scenePath.string());
        return false;
    }

    _sceneDirty = false;
    std::ofstream lastSceneFile(LastEditorScenePath, std::ios::trunc);
    if (lastSceneFile) {
        lastSceneFile << _activeSceneFilename << '\n';
    }
    fmt::print("Saved editor scene: {}\n", scenePath.string());
    return true;
}

bool VulkanEngine::save_editor_scene_as(std::string_view sceneName)
{
    const std::optional<std::string> filename = normalize_scene_filename(sceneName);
    if (!filename.has_value()) {
        fmt::print("Invalid scene filename: {}\n", sceneName);
        return false;
    }
    const std::string previousFilename = _activeSceneFilename;
    _activeSceneFilename = *filename;
    if (save_editor_scene()) {
        return true;
    }
    _activeSceneFilename = previousFilename;
    return false;
}

void VulkanEngine::restore_last_editor_scene_name()
{
    std::ifstream lastSceneFile(LastEditorScenePath);
    std::string filename;
    if (!lastSceneFile || !std::getline(lastSceneFile, filename)) {
        return;
    }
    const std::optional<std::string> normalized = normalize_scene_filename(filename);
    if (normalized.has_value() && std::filesystem::exists(editor_scene_path(*normalized))) {
        _activeSceneFilename = *normalized;
    }
}

bool VulkanEngine::load_editor_scene()
{
    const std::filesystem::path scenePath = editor_scene_path(_activeSceneFilename);
    if (!std::filesystem::exists(scenePath)) {
        return false;
    }

    simdjson::padded_string json;
    if (simdjson::padded_string::load(scenePath.string()).get(json)) {
        fmt::print("Could not read editor scene: {}\n", scenePath.string());
        return false;
    }
    simdjson::dom::parser parser;
    simdjson::dom::element document;
    if (parser.parse(json).get(document)) {
        fmt::print("Could not parse editor scene JSON: {}\n", scenePath.string());
        return false;
    }

    uint64_t version = 0;
    simdjson::dom::array jsonObjects;
    if (document["version"].get_uint64().get(version) ||
        version != EditorSceneVersion ||
        document["objects"].get_array().get(jsonObjects)) {
        fmt::print("Unsupported editor scene: {}\n", scenePath.string());
        return false;
    }

    uint64_t nextActor = 1;
    document["nextActor"].get_uint64().get(nextActor);

    std::vector<SavedSceneObject> savedObjects;
    for (simdjson::dom::element jsonObjectElement : jsonObjects) {
        simdjson::dom::object jsonObject;
        SavedSceneObject saved{};
        simdjson::dom::element position;
        simdjson::dom::element rotation;
        simdjson::dom::element scale;
        simdjson::dom::element colliderCenter;
        simdjson::dom::element colliderHalfExtents;
        std::string_view name;
        std::string_view assetName;
        std::string_view timeTrialRoleName;
        std::string_view modelPath;
        uint64_t id = 0;
        int64_t parent = -1;
        int64_t layer = 0;
        int64_t collisionShape = 0;

        if (jsonObjectElement.get_object().get(jsonObject) ||
            jsonObject["id"].get_uint64().get(id) ||
            jsonObject["parent"].get_int64().get(parent) ||
            jsonObject["name"].get_string().get(name) ||
            jsonObject["position"].get(position) ||
            jsonObject["rotation"].get(rotation) ||
            jsonObject["scale"].get(scale) ||
            !read_json_vec3(position, saved.transform.position) ||
            !read_json_vec3(rotation, saved.transform.rotation) ||
            !read_json_vec3(scale, saved.transform.scale) ||
            jsonObject["visible"].get_bool().get(saved.visible) ||
            jsonObject["hasCollision"].get_bool().get(saved.hasCollision) ||
            jsonObject["portalPlaceable"].get_bool().get(saved.portalPlaceable) ||
            jsonObject["layer"].get_int64().get(layer) ||
            jsonObject["collisionShape"].get_int64().get(collisionShape) ||
            jsonObject["asset"].get_string().get(assetName)) {
            fmt::print("Invalid object in editor scene: {}\n", scenePath.string());
            return false;
        }
        const std::optional<SceneAssetKind> assetKind = scene_asset_from_name(assetName);
        if (!assetKind.has_value() || layer < 0 || layer > 1 ||
            collisionShape < 0 || collisionShape > 2) {
            fmt::print("Unsupported object data in editor scene: {}\n", scenePath.string());
            return false;
        }
        saved.oldID = static_cast<uint32_t>(id);
        saved.oldParent = parent;
        saved.name = name;
        saved.layer = static_cast<RenderLayer>(layer);
        saved.collisionShape = static_cast<CollisionShape>(collisionShape);
        saved.assetKind = *assetKind;

        // This field was introduced after the first editor scenes.  Leaving
        // it absent means the object remains an ordinary actor.
        if (jsonObject["timeTrialRole"].get_string().get(timeTrialRoleName) ==
            simdjson::SUCCESS) {
            const std::optional<TimeTrialRole> role =
                time_trial_role_from_name(timeTrialRoleName);
            if (!role.has_value()) {
                fmt::print("Unsupported time-trial role in editor scene: {}\n", scenePath.string());
                return false;
            }
            saved.timeTrialRole = *role;
        }

        // Collider fields were added after the first saved scenes.  Missing
        // values deliberately use the unit-cube defaults, so old levels load
        // with exactly their original collision behaviour.
        const bool hasColliderCenter =
            jsonObject["colliderCenter"].get(colliderCenter) == simdjson::SUCCESS;
        const bool hasColliderHalfExtents =
            jsonObject["colliderHalfExtents"].get(colliderHalfExtents) == simdjson::SUCCESS;
        if (hasColliderCenter != hasColliderHalfExtents ||
            (hasColliderCenter &&
             (!read_json_vec3(colliderCenter, saved.colliderCenter) ||
              !read_json_vec3(colliderHalfExtents, saved.colliderHalfExtents))) ||
            saved.colliderHalfExtents.x <= 0.0f ||
            saved.colliderHalfExtents.y <= 0.0f ||
            saved.colliderHalfExtents.z <= 0.0f) {
            fmt::print("Invalid box collider in editor scene: {}\n", scenePath.string());
            return false;
        }
        if (saved.assetKind == SceneAssetKind::ImportedGLTF) {
            if (jsonObject["modelPath"].get_string().get(modelPath) ||
                modelPath.empty()) {
                fmt::print("Missing model path in editor scene: {}\n", scenePath.string());
                return false;
            }
            saved.modelPath = modelPath;
        }
        // Version 1 accidentally wrote Player Model even though its Player
        // parent is runtime-only.  PortalViewOnly is reserved for that
        // runtime graph, so ignore those stale entries and preserve the
        // editor-authored objects in already-saved files.
        if (saved.layer == RenderLayer::PortalViewOnly) {
            continue;
        }
        savedObjects.push_back(std::move(saved));
    }

    if (savedObjects.empty()) {
        fmt::print("Editor scene has no objects: {}\n", scenePath.string());
        return false;
    }

    Scene restoredScene{};
    std::unordered_map<uint32_t, SceneObjectID> restoredIDs;
    for (const SavedSceneObject& saved : savedObjects) {
        const SceneObjectID newID = restoredScene.create_object(saved.name);
        restoredIDs.emplace(saved.oldID, newID);
        SceneObject* object = restoredScene.get(newID);
        object->localTransform = saved.transform;
        object->visible = saved.visible;
        object->hasCollision = saved.hasCollision;
        object->portalPlaceable = saved.portalPlaceable;
        object->layer = saved.layer;
        object->collisionShape = saved.collisionShape;
        object->colliderCenter = saved.colliderCenter;
        object->colliderHalfExtents = saved.colliderHalfExtents;
        object->assetKind = saved.assetKind;
        object->timeTrialRole = saved.timeTrialRole;
        object->modelPath = saved.modelPath;
    }
    for (const SavedSceneObject& saved : savedObjects) {
        if (saved.oldParent < 0) {
            continue;
        }
        const auto child = restoredIDs.find(saved.oldID);
        const auto parent = restoredIDs.find(static_cast<uint32_t>(saved.oldParent));
        if (child == restoredIDs.end() || parent == restoredIDs.end() ||
            !restoredScene.set_parent(child->second, parent->second)) {
            fmt::print("Invalid hierarchy in editor scene: {}\n", scenePath.string());
            return false;
        }
    }

    SceneObjectID restoredRoot = InvalidSceneObject;
    SceneObjectID restoredFloor = InvalidSceneObject;
    for (SceneObject& object : restoredScene.objects) {
        if (object.parent == InvalidSceneObject && object.name == "Sandbox") {
            restoredRoot = object.id;
        }
        if (object.assetKind == SceneAssetKind::FloorQuad &&
            restoredFloor == InvalidSceneObject) {
            restoredFloor = object.id;
        }
    }
    if (restoredRoot == InvalidSceneObject || restoredFloor == InvalidSceneObject) {
        fmt::print("Editor scene is missing its Sandbox root or Floor: {}\n", scenePath.string());
        return false;
    }

    _scene = std::move(restoredScene);
    _sandboxRoot = restoredRoot;
    _floorObject = restoredFloor;
    for (SceneObject& object : _scene.objects) {
        assign_scene_asset(object, object.assetKind);
    }
    create_runtime_scene_objects();
    retract_portals();
    reset_time_trial();
    _selectedSceneObject = InvalidSceneObject;
    _nextCreatedActorNumber = static_cast<uint32_t>(std::max<uint64_t>(nextActor, 1));
    _sceneDirty = false;
    std::ofstream lastSceneFile(LastEditorScenePath, std::ios::trunc);
    if (lastSceneFile) {
        lastSceneFile << _activeSceneFilename << '\n';
    }
    rebuild_collision_from_scene();
    fmt::print("Loaded editor scene: {}\n", scenePath.string());
    return true;
}

bool VulkanEngine::load_editor_scene_named(std::string_view sceneName)
{
    const std::optional<std::string> filename = normalize_scene_filename(sceneName);
    if (!filename.has_value()) {
        return false;
    }
    const std::string previousFilename = _activeSceneFilename;
    _activeSceneFilename = *filename;
    if (load_editor_scene()) {
        return true;
    }
    _activeSceneFilename = previousFilename;
    return false;
}

void VulkanEngine::build_sandbox_scene()
{
    const MeshPrimitive floorPrimitive{
        .indexCount = 6,
        .firstIndex = 0,
        .indexBuffer = _floorMesh.indexBuffer.buffer,
        .vertexBufferAddress = _floorMesh.vertexBufferAddress,
        .bounds = _floorBounds,
        .material = &_floorMaterial};
    // The unit cube every wall and panel is built from.
    const MeshPrimitive cubePrimitive{
        .indexCount = 36,
        .firstIndex = 0,
        .indexBuffer = _wallMesh.indexBuffer.buffer,
        .vertexBufferAddress = _wallMesh.vertexBufferAddress,
        .bounds = _wallBounds,
        .material = &_wallMaterial};

    _sandboxRoot = _scene.create_object("Sandbox");

    _floorObject = _scene.create_object("Floor", _sandboxRoot);
    if (SceneObject* floor = _scene.get(_floorObject)) {
        floor->localTransform.scale = glm::vec3(50.0f, 1.0f, 50.0f);
        floor->primitive = floorPrimitive;
        floor->assetKind = SceneAssetKind::FloorQuad;
        floor->hasCollision = true;
        floor->collisionShape = CollisionShape::GroundPlane;
    }

    // Arena boundary plus a compact portal test rig around spawn.  Scale is
    // the full size of the box, so a wall's collider is exactly its mesh.
    struct WallDescription {
        const char* name;
        glm::vec3 position;
        glm::vec3 scale;
    };
    const std::array<WallDescription, 7> wallDescriptions{{
        {"North Wall", {0.0f, 1.5f, -24.75f}, {50.0f, 3.0f, 0.5f}},
        {"South Wall", {0.0f, 1.5f, 24.75f}, {50.0f, 3.0f, 0.5f}},
        {"West Wall", {-24.75f, 1.5f, 0.0f}, {0.5f, 3.0f, 50.0f}},
        {"East Wall", {24.75f, 1.5f, 0.0f}, {0.5f, 3.0f, 50.0f}},
        {"Test North Panel", {0.0f, 1.5f, -5.0f}, {6.0f, 3.0f, 0.5f}},
        {"Test South Panel", {0.0f, 1.5f, 5.0f}, {6.0f, 3.0f, 0.5f}},
        {"Test East Panel", {5.0f, 1.5f, 0.0f}, {0.5f, 3.0f, 6.0f}},
    }};
    for (const WallDescription& description : wallDescriptions) {
        const SceneObjectID id = _scene.create_object(
            description.name, _sandboxRoot);
        SceneObject* wall = _scene.get(id);
        if (wall == nullptr) {
            continue;
        }
        wall->localTransform.position = description.position;
        wall->localTransform.scale = description.scale;
        wall->primitive = cubePrimitive;
        wall->assetKind = SceneAssetKind::UnitCube;
        wall->hasCollision = true;
        wall->portalPlaceable = true;
    }

    // The player node follows physics; its model child holds the editable
    // asset offset, facing correction, and scale.
    _playerObject = _scene.create_object("Player", _sandboxRoot);
    if (SceneObject* player = _scene.get(_playerObject)) {
        player->transformDrivenExternally = true;
        player->layer = RenderLayer::PortalViewOnly;
    }

    _playerModelObject = _scene.create_object("Player Model", _playerObject);
    if (SceneObject* playerModel = _scene.get(_playerModelObject)) {
        playerModel->layer = RenderLayer::PortalViewOnly;
        if (_playerModel) {
            playerModel->model = _playerModel;
            // The imported model has a large internal glTF scale (about 14
            // world units tall at 1.0), while the player is roughly 1.8 tall.
            playerModel->localTransform.scale = glm::vec3(0.12f);
        } else {
            // Keep the collision-sized box as a visible fallback if the asset
            // fails to load on another machine.
            const PlayerMovementSettings& settings = _playerMovement.settings;
            playerModel->primitive = cubePrimitive;
            playerModel->primitive.material = &_playerMaterial;
            playerModel->primitive.bounds = _playerBounds;
            playerModel->localTransform.position = glm::vec3(
                0.0f, settings.playerHeight * 0.5f, 0.0f);
            playerModel->localTransform.scale = glm::vec3(
                settings.playerHalfWidth * 2.0f,
                settings.playerHeight,
                settings.playerHalfWidth * 2.0f);
        }
    }

    // Portals keep their own placement, traversal, and stencil logic. These
    // objects only mirror it so the hierarchy shows where each portal is.
    const auto createPortalObject = [&](const char* name) {
        const SceneObjectID id = _scene.create_object(name, _sandboxRoot);
        if (SceneObject* portalObject = _scene.get(id)) {
            portalObject->visible = false;
            portalObject->transformDrivenExternally = true;
        }
        return id;
    };
    _bluePortalObject = createPortalObject("Blue Portal");
    _orangePortalObject = createPortalObject("Orange Portal");

    // Give physics its ground plane before the first frame runs.
    rebuild_collision_from_scene();
}

void VulkanEngine::sync_scene_driven_objects()
{
    if (SceneObject* player = _scene.get(_playerObject)) {
        player->localTransform.position = _playerMovement.position;
        // Camera yaw uses a negative-Y rotation.
        player->localTransform.rotation = glm::vec3(0.0f, -mainCamera.yaw, 0.0f);
    }

    const auto syncPortalObject = [&](SceneObjectID id, const Portal& portal) {
        SceneObject* portalObject = _scene.get(id);
        if (portalObject == nullptr) {
            return;
        }
        portalObject->visible = portal.placed;
        if (!portal.placed) {
            return;
        }
        portalObject->localTransform.position = portal.position;
        // Placed portals are always axis-aligned and upright for now, so the
        // frame reduces to a yaw around the wall normal.
        portalObject->localTransform.rotation = glm::vec3(
            0.0f, std::atan2(portal.normal.x, portal.normal.z), 0.0f);
        portalObject->localTransform.scale = glm::vec3(
            portal.halfWidth * 2.0f, portal.halfHeight * 2.0f, 1.0f);
    };
    syncPortalObject(_bluePortalObject, _bluePortal);
    syncPortalObject(_orangePortalObject, _orangePortal);
}

void VulkanEngine::emit_scene_render_objects(
    RenderLayer layer,
    DrawContext& drawContext)
{
    for (const SceneObject& object : _scene.objects) {
        if (!object.alive || !object.visible || object.layer != layer) {
            continue;
        }

        const glm::mat4 world = _scene.world_matrix(object.id);
        if (object.model != nullptr) {
            object.model->Draw(world, drawContext);
            continue;
        }
        if (!object.primitive.valid()) {
            continue;
        }

        RenderObject renderObject{};
        renderObject.indexCount = object.primitive.indexCount;
        renderObject.firstIndex = object.primitive.firstIndex;
        renderObject.indexBuffer = object.primitive.indexBuffer;
        renderObject.material = object.primitive.material;
        renderObject.bounds = object.primitive.bounds;
        renderObject.transform = world;
        renderObject.vertexBufferAddress = object.primitive.vertexBufferAddress;
        drawContext.OpaqueSurfaces.push_back(renderObject);
    }
}

void VulkanEngine::update_scene(float deltaTime)
{
    const auto startTime = std::chrono::steady_clock::now();
    mainCamera.position = _playerMovement.position + glm::vec3(0.0f, 1.7f, 0.0f);
    const Camera& camera = render_camera();

    // The main compute dispatch runs after update_scene().  Feed it the
    // active camera's orientation here; translation intentionally never
    // reaches the skybox shader, preventing parallax while walking.
    if (backgroundEffects.size() > 1) {
        const glm::mat4 rotation = camera.getRotationMatrix();
        ComputeEffect& sky = backgroundEffects[1];
        sky.data.data2 = glm::vec4(
            glm::normalize(glm::vec3(rotation * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f))),
            0.0f);
        sky.data.data3 = glm::vec4(
            glm::normalize(glm::vec3(rotation * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f))),
            0.0f);
        sky.data.data4 = glm::vec4(
            glm::normalize(glm::vec3(rotation * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f))),
            0.0f);
    }

    mainDrawContext.OpaqueSurfaces.clear();
    mainDrawContext.TransparentSurfaces.clear();
    worldDrawContext.OpaqueSurfaces.clear();
    worldDrawContext.TransparentSurfaces.clear();
    portalViewDrawContext.OpaqueSurfaces.clear();
    portalViewDrawContext.TransparentSurfaces.clear();

    for (auto& [name, scene] : loadedScenes) {
        if (scene != nullptr) {
            scene->Draw(glm::mat4(1.0f), worldDrawContext);
        }
    }
    sync_scene_driven_objects();
    emit_scene_render_objects(RenderLayer::World, worldDrawContext);

    portalViewDrawContext = worldDrawContext;
    // The main camera sits inside the player, so the body is added only to
    // the portal views.
    emit_scene_render_objects(RenderLayer::PortalViewOnly, portalViewDrawContext);

    // The ordinary world is drawn first.  Once both portals exist, their
    // rectangle is replaced by the stencil/virtual-camera passes below.  A
    // single unlinked portal stays coloured so its placement is still visible.
    mainDrawContext = worldDrawContext;
    const auto addPortal = [&](const Portal& sourcePortal, MaterialInstance& material) {
        if (!sourcePortal.placed) {
            return;
        }
        mainDrawContext.OpaqueSurfaces.push_back(
            make_portal_render_object(sourcePortal, material));
    };
    if (!_bluePortal.placed || !_orangePortal.placed) {
        addPortal(_bluePortal, _bluePortalMaterial);
        addPortal(_orangePortal, _orangePortalMaterial);
    }

    sceneData = build_scene_data(camera.getViewMatrix());

    std::memcpy(
        get_current_frame().sceneBuffer.info.pMappedData,
        &sceneData,
        sizeof(sceneData));

    if (_bluePortal.placed && _orangePortal.placed) {
        const glm::vec3 cameraForward = glm::normalize(glm::vec3(
            camera.getRotationMatrix() * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
        const glm::vec3 cameraUp = glm::normalize(glm::vec3(
            camera.getRotationMatrix() * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)));

        const auto updatePortalView = [&](const Portal& source,
                                          const Portal& destination,
                                          uint32_t viewIndex,
                                          uint32_t recursiveViewIndex) {
            const glm::mat4 transfer = get_portal_transfer_transform(
                source, destination);
            // At the instant we cross a portal, the mathematically exact
            // virtual camera lies on the exit portal plane.  Keep it a tiny
            // distance behind that plane for rendering only so the clip and
            // depth passes never leave a one-frame black aperture.
            const glm::vec3 virtualPosition = stabilize_portal_view_camera(
                destination,
                glm::vec3(transfer * glm::vec4(camera.position, 1.0f)));
            const glm::vec3 virtualForward = glm::normalize(glm::vec3(
                transfer * glm::vec4(cameraForward, 0.0f)));
            const glm::vec3 virtualUp = glm::normalize(glm::vec3(
                transfer * glm::vec4(cameraUp, 0.0f)));

            _portalSceneData[viewIndex] = build_portal_scene_data(
                glm::lookAt(
                    virtualPosition,
                    virtualPosition + virtualForward,
                    virtualUp),
                destination);
            std::memcpy(
                get_current_frame().portalSceneBuffers[viewIndex].info.pMappedData,
                &_portalSceneData[viewIndex],
                sizeof(GPUSceneData));

            // One additional application of the same transform is the view
            // seen when this portal appears inside its own primary portal
            // view. Deeper recursion would repeat this same operation.
            const glm::vec3 recursivePosition = stabilize_portal_view_camera(
                destination,
                glm::vec3(transfer * glm::vec4(virtualPosition, 1.0f)));
            const glm::vec3 recursiveForward = glm::normalize(glm::vec3(
                transfer * glm::vec4(virtualForward, 0.0f)));
            const glm::vec3 recursiveUp = glm::normalize(glm::vec3(
                transfer * glm::vec4(virtualUp, 0.0f)));
            _portalSceneData[recursiveViewIndex] = build_portal_scene_data(
                glm::lookAt(
                    recursivePosition,
                    recursivePosition + recursiveForward,
                    recursiveUp),
                destination);
            std::memcpy(
                get_current_frame().portalSceneBuffers[recursiveViewIndex].info.pMappedData,
                &_portalSceneData[recursiveViewIndex],
                sizeof(GPUSceneData));
        };

        // Looking into blue means rendering the world as seen after exiting
        // orange; looking into orange is the inverse relation.
        updatePortalView(
            _bluePortal,
            _orangePortal,
            BluePortalView,
            BluePortalRecursiveView);
        updatePortalView(
            _orangePortal,
            _bluePortal,
            OrangePortalView,
            OrangePortalRecursiveView);
    }

    stats.scene_update_time = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - startTime).count();
}

void VulkanEngine::init_background_pipelines()
{
    VkPipelineLayoutCreateInfo computeLayout{};
    computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    computeLayout.pNext = nullptr;
    computeLayout.pSetLayouts = &_drawImageDescriptorLayout;
    computeLayout.setLayoutCount = 1;

    VkPushConstantRange pushConstant{};
    pushConstant.offset = 0;
    pushConstant.size = sizeof(ComputePushConstants);
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    computeLayout.pPushConstantRanges = &pushConstant;
    computeLayout.pushConstantRangeCount = 1;

    VkShaderModule gradientShader;
	if (!vkutil::load_shader_module("../../shaders/gradient_color.comp.spv", _device, &gradientShader))
	{
		fmt::print("Error loading gradient_color compute shader\n");
		return;
	}

    VkShaderModule skyShader;
	if (!vkutil::load_shader_module("../../shaders/sky.comp.spv", _device, &skyShader))
	{
		fmt::print("Error loading sky compute shader\n");
		vkDestroyShaderModule(_device, gradientShader, nullptr);
		return;
	}

    VkPipelineShaderStageCreateInfo stageinfo{};
	stageinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageinfo.pNext = nullptr;
	stageinfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageinfo.module = gradientShader;
	stageinfo.pName = "main";

    VK_CHECK(vkCreatePipelineLayout(_device, &computeLayout, nullptr, &_gradientPipelineLayout));

    VkComputePipelineCreateInfo computePipelineCreateInfo{};
	computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	computePipelineCreateInfo.pNext = nullptr;
	computePipelineCreateInfo.layout = _gradientPipelineLayout;
	computePipelineCreateInfo.stage = stageinfo;

    ComputeEffect gradient{};
    gradient.name = "gradient";
    gradient.layout = _gradientPipelineLayout;
    gradient.data.data1 = glm::vec4(1, 0, 0, 1);
    gradient.data.data2 = glm::vec4(0, 0, 1, 1);
    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &gradient.pipeline));

    stageinfo.module = skyShader;
    computePipelineCreateInfo.stage = stageinfo;

    ComputeEffect sky{};
    sky.name = "sky";
    sky.layout = _gradientPipelineLayout;
    sky.data.data1 = glm::vec4(0.1f, 0.2f, 0.4f, 0.97f);
    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &sky.pipeline));

    backgroundEffects.clear();
    backgroundEffects.push_back(gradient);
    backgroundEffects.push_back(sky);

    vkDestroyShaderModule(_device, gradientShader, nullptr);
    vkDestroyShaderModule(_device, skyShader, nullptr);

	_mainDeletionQueue.push_function([this, gradientPipeline = gradient.pipeline, skyPipeline = sky.pipeline]() {
		vkDestroyPipeline(_device, gradientPipeline, nullptr);
		vkDestroyPipeline(_device, skyPipeline, nullptr);
		vkDestroyPipelineLayout(_device, _gradientPipelineLayout, nullptr);
		});
}

void VulkanEngine::immediate_submit(std::function<void(VkCommandBuffer cmd)>&& function)
{
    VK_CHECK(vkResetFences(_device, 1, &_immFence));
	VK_CHECK(vkResetCommandBuffer(_immCommandBuffer, 0));

	VkCommandBuffer cmd = _immCommandBuffer;

	VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

	VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

	function(cmd);

	VK_CHECK(vkEndCommandBuffer(cmd));

	VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd);
	VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo, nullptr, nullptr);

	// submit command buffer to the queue and execute it.
	//  _renderFence will now block until the graphic commands finish execution
	VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit, _immFence));

	VK_CHECK(vkWaitForFences(_device, 1, &_immFence, true, 9999999999));
}
void VulkanEngine::init_imgui()
{
	// 1: create descriptor pool for IMGUI
	//  the size of the pool is very oversize, but it's copied from imgui demo
	//  itself.
	VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 } };

	VkDescriptorPoolCreateInfo pool_info = {};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool_info.maxSets = 1000;
	pool_info.poolSizeCount = (uint32_t)std::size(pool_sizes);
	pool_info.pPoolSizes = pool_sizes;

	VkDescriptorPool imguiPool;
	VK_CHECK(vkCreateDescriptorPool(_device, &pool_info, nullptr, &imguiPool));

	// 2: initialize imgui library

	// this initializes the core structures of imgui
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	// Start the editor with its own layout instead of inheriting the scattered
	// windows saved by the old in-game debug UI.
	io.IniFilename = "mirabilis_editor.ini";

	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowPadding = ImVec2(10.0f, 8.0f);
	style.FramePadding = ImVec2(7.0f, 4.0f);
	style.ItemSpacing = ImVec2(8.0f, 6.0f);
	style.WindowRounding = 4.0f;
	style.FrameRounding = 3.0f;
	style.GrabRounding = 3.0f;
	style.Colors[ImGuiCol_WindowBg] = ImVec4(0.045f, 0.055f, 0.075f, 0.96f);
	style.Colors[ImGuiCol_TitleBg] = ImVec4(0.055f, 0.075f, 0.105f, 1.0f);
	style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.075f, 0.130f, 0.190f, 1.0f);
	style.Colors[ImGuiCol_Header] = ImVec4(0.090f, 0.180f, 0.290f, 0.85f);
	style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.110f, 0.250f, 0.390f, 0.95f);
	style.Colors[ImGuiCol_Button] = ImVec4(0.080f, 0.190f, 0.310f, 0.90f);
	style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.110f, 0.270f, 0.440f, 1.0f);

	// this initializes imgui for SDL
	ImGui_ImplSDL2_InitForVulkan(_window);

	// this initializes imgui for Vulkan
	ImGui_ImplVulkan_InitInfo init_info = {};
	init_info.ApiVersion = VK_API_VERSION_1_3;
	init_info.Instance = _instance;
	init_info.PhysicalDevice = _chosenGPU;
	init_info.Device = _device;
	init_info.QueueFamily = _graphicsQueueFamily;
	init_info.Queue = _graphicsQueue;
	init_info.DescriptorPool = imguiPool;
	init_info.MinImageCount = 2;
	init_info.ImageCount = static_cast<uint32_t>(_swapchainImages.size());
	init_info.UseDynamicRendering = true;

	//dynamic rendering parameters for imgui to use
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &_swapchainImageFormat;
	init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

	ImGui_ImplVulkan_Init(&init_info);
	// The modern Vulkan backend uploads the font texture automatically during
	// its first NewFrame call.

	// add the destroy the imgui created structures
	_mainDeletionQueue.push_function([=]() {
		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplSDL2_Shutdown();
		ImGui::DestroyContext();
		vkDestroyDescriptorPool(_device, imguiPool, nullptr);
	});
}

void VulkanEngine::draw_imgui(VkCommandBuffer cmd, VkImageView targetImageView)
{
	VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
		targetImageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingInfo renderInfo = vkinit::rendering_info(
		_swapchainExtent, &colorAttachment, nullptr);

	vkCmdBeginRendering(cmd, &renderInfo);
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
	vkCmdEndRendering(cmd);
}
