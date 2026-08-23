#include "vk_engine.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <unordered_map>

#include <simdjson.h>

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

} // namespace

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

