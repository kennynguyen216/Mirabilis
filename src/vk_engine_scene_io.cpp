#include "vk_engine.h"

#include <cmath>
#include <filesystem>
#include <fstream>

#include <SDL.h>

namespace {

constexpr const char* EditorSceneDirectory = "../../assets/scenes";
constexpr const char* LastEditorScenePath = "../../assets/scenes/.last_scene";
constexpr const char* DefaultEditorSceneFilename = "sandbox.json";

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
    const char* scratch = SDL_getenv("MIRABILIS_TEST_SCENE_DIRECTORY");
    return std::filesystem::path(scratch && *scratch ? scratch : EditorSceneDirectory) / std::string(filename);
}

} // namespace

SceneSettings VulkanEngine::capture_scene_settings() const
{
    SceneSettings settings;
    settings.nextActor = _nextCreatedActorNumber;
    settings.skybox = _authoredSkyboxId.empty() ? SkyboxIds[std::clamp(_skyboxSelection, 0, static_cast<int>(SkyboxIds.size()) - 1)] : _authoredSkyboxId;
    settings.sunDirection = _shadow.sunlightDirection;
    settings.shadowsEnabled = _shadow.enabled;
    settings.shadowRadius = _shadow.radius;
    settings.shadowDepthBias = _shadow.depthBias;
    settings.shadowNormalBias = _shadow.normalBias;
    settings.sunRadiance = glm::vec3(_traceSettings.lighting.sunRadiance);
    settings.environmentIntensity = _traceSettings.lighting.environment.x;
    settings.blackEnvironment = _traceSettings.lighting.environment.y > 0.5f;
    settings.environmentTint = glm::vec3(_traceSettings.lighting.environmentTint);
    settings.warmth = _postProcess.whiteBalance.x;
    settings.tint = _postProcess.whiteBalance.y;
    settings.giSaturation = _ssgi.colorSaturation;
    if (_ssao.sceneOverride) settings.ssao = SceneSettings::SSAO{
        _ssao.settings.enabled, _ssao.settings.radius, _ssao.settings.bias,
        _ssao.settings.intensity, _ssao.settings.power};
    return settings;
}

void VulkanEngine::apply_scene_settings(const SceneSettings& settings)
{
    _nextCreatedActorNumber = settings.nextActor;
    _authoredSkyboxId = settings.skybox;
    _shadow.sunlightDirection = settings.sunDirection;
    _shadow.enabled = settings.shadowsEnabled;
    _shadow.radius = settings.shadowRadius;
    _shadow.depthBias = settings.shadowDepthBias;
    _shadow.normalBias = settings.shadowNormalBias;
    _traceSettings.lighting = TraceLighting{};
    _traceSettings.lighting.sunRadiance = glm::vec4(settings.sunRadiance, 0);
    _traceSettings.lighting.environment.x = settings.environmentIntensity;
    _traceSettings.lighting.environment.y = settings.blackEnvironment ? 1.f : 0.f;
    _traceSettings.lighting.environmentTint = glm::vec4(settings.environmentTint, 0);
    _postProcess.whiteBalance = glm::vec2(settings.warmth, settings.tint);
    _ssgi.colorSaturation = settings.giSaturation;
    _ssao.settings = SSAOSettings{};
    _ssao.sceneOverride = settings.ssao.has_value();
    if (settings.ssao) {
        const auto& ao = *settings.ssao;
        _ssao.settings.enabled = ao.enabled; _ssao.settings.radius = ao.radius;
        _ssao.settings.bias = ao.bias; _ssao.settings.intensity = ao.intensity;
        _ssao.settings.power = ao.power;
    }
    bool found = false;
    for (size_t index = 0; index < SkyboxIds.size(); ++index) {
        if (settings.skybox != SkyboxIds[index]) continue;
        found = true;
        if (static_cast<int>(index) != _skyboxSelection && !set_skybox(static_cast<int>(index)))
            fmt::print("Scene skybox '{}' could not be loaded; keeping its authored identity.\n", settings.skybox);
        break;
    }
    if (!found) fmt::print("Scene skybox '{}' is unavailable; keeping its authored identity.\n", settings.skybox);
    apply_r4_light_sources();
}

void VulkanEngine::create_runtime_scene_objects()
{
    const auto player = _scene.create_object("Player", _scene.get(_sandboxRoot));
    _playerObject = player.persistentId;
    _scene.set(player, Visibility{true, RenderLayer::PortalViewOnly}, true);
    _scene.set(player, TransformDrivenExternally{}, true);
    _scene.set(player, RuntimeOnly{}, true);

    const auto model = _scene.create_object("Player Model", player, true);
    _playerModelObject = model.persistentId;
    _scene.set(model, Visibility{true, RenderLayer::PortalViewOnly}, true);
    Transform transform;
    if (_playerModel) {
        _sceneRenderRuntimes[model.persistentId].model = _playerModel;
        transform.scale = glm::vec3(0.12f);
    } else {
        const auto& movement = _playerMovement.settings;
        assign_scene_asset(model, SceneAssetKind::UnitCube);
        auto& primitive = _sceneRenderRuntimes[model.persistentId].primitive;
        primitive.material = &_playerMaterial;
        primitive.bounds = _playerBounds;
        transform.position = glm::vec3(0.f, movement.playerHeight * 0.5f, 0.f);
        transform.scale = glm::vec3(movement.playerHalfWidth * 2.f, movement.playerHeight, movement.playerHalfWidth * 2.f);
    }
    _scene.set_local_transform(model, transform, true);
    _scene.set(model, RuntimeOnly{}, true);
    const auto createPortal = [&](const char* name) {
        const auto ref = _scene.create_object(name, _scene.get(_sandboxRoot));
        _scene.set(ref, Visibility{false, RenderLayer::World}, true);
        _scene.set(ref, TransformDrivenExternally{}, true);
        _scene.set(ref, RuntimeOnly{}, true);
        return ref.persistentId;
    };
    _bluePortalObject = createPortal("Blue Portal");
    _orangePortalObject = createPortal("Orange Portal");
}

bool VulkanEngine::save_editor_scene()
{
    const auto scenePath = editor_scene_path(_sceneDocument.activeFilename);
    std::string error;
    bool saved = false;
    if (!_editorMode && _playSnapshot) {
        mirabilis::Scene authored;
        if (!authored.restore_snapshot(_playSnapshot->entities) ||
            !authored.reserve_persistent_ids(_scene.next_persistent_id())) {
            fmt::print("Could not reconstruct authored scene for saving: {}\n", authored.last_error());
            return false;
        }
        saved = save_scene_atomic(scenePath, authored, _playSnapshot->settings, _playSnapshot->authoredPortals, error);
    } else {
        saved = save_scene_atomic(scenePath, _scene, capture_scene_settings(), _authoredPortals.pairs, error);
    }
    if (!saved) { fmt::print("Could not save scene '{}': {}\n", scenePath.string(), error); return false; }
    _sceneDocument.dirty = false;
    if (!SDL_getenv("MIRABILIS_TEST_FRAMES")) {
        std::ofstream lastSceneFile(LastEditorScenePath, std::ios::trunc);
        if (lastSceneFile) lastSceneFile << _sceneDocument.activeFilename << '\n';
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
    const std::string previousFilename = _sceneDocument.activeFilename;
    _sceneDocument.activeFilename = *filename;
    if (save_editor_scene()) {
        return true;
    }
    _sceneDocument.activeFilename = previousFilename;
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
        _sceneDocument.activeFilename = *normalized;
    }
}

// SDL's user directory is shared by Debug/Release and independent of maps.
void VulkanEngine::load_ao_preferences()
{
    char* directory = SDL_GetPrefPath("Mirabilis", "Mirabilis");
    if (!directory) return;
    std::ifstream file(std::filesystem::path(directory) / "ambient_occlusion.cfg");
    SDL_free(directory);
    int version = 0, enabled = 1, quality = 1;
    float depth = 12.0f, normal = 16.0f;
    if (!(file >> version >> enabled >> quality >> depth >> normal) ||
        version != 1 || (enabled != 0 && enabled != 1) ||
        quality < 0 || quality >= static_cast<int>(SSAOKernelSizes.size()) ||
        !std::isfinite(depth) || depth < 5.0f || depth > 120.0f ||
        !std::isfinite(normal) || normal < 1.0f || normal > 48.0f) return;
    _ssao.globalEnabled = enabled != 0;
    _ssao.quality = quality;
    _ssao.depthFalloff = depth;
    _ssao.normalFalloff = normal;
}

void VulkanEngine::save_ao_preferences() const
{
    char* directory = SDL_GetPrefPath("Mirabilis", "Mirabilis");
    if (!directory) return;
    std::ofstream file(std::filesystem::path(directory) / "ambient_occlusion.cfg");
    SDL_free(directory);
    file << "1 " << (_ssao.globalEnabled ? 1 : 0) << ' ' << _ssao.quality
         << ' ' << _ssao.depthFalloff << ' ' << _ssao.normalFalloff << '\n';
    if (!file) fmt::print("Could not save ambient occlusion preferences\n");
}

bool VulkanEngine::load_editor_scene()
{
    const auto path = editor_scene_path(_sceneDocument.activeFilename);
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) { fmt::print("Could not read scene: {}\n", path.string()); return false; }
    StagedScene staging;
    std::string error;
    if (!parse_scene_json(json, path.filename().string(), capture_scene_settings(), staging, error)) {
        fmt::print("Could not load scene '{}': {}\n", path.string(), error);
        return false;
    }
    for (const auto& warning : staging.warnings) fmt::print("Scene warning: {}\n", warning);
    if (!install_staged_scene(std::move(staging))) return false;
    _sceneDocument.dirty = false;
    if (!SDL_getenv("MIRABILIS_TEST_FRAMES")) {
        std::ofstream lastSceneFile(LastEditorScenePath, std::ios::trunc);
        if (lastSceneFile) lastSceneFile << _sceneDocument.activeFilename << '\n';
    }
    _autoExposure.snap = true;
    fmt::print("Loaded editor scene: {}\n", path.string());
    return true;
}

bool VulkanEngine::load_editor_scene_named(std::string_view sceneName)
{
    const std::optional<std::string> filename = normalize_scene_filename(sceneName);
    if (!filename.has_value()) {
        return false;
    }
    const std::string previousFilename = _sceneDocument.activeFilename;
    _sceneDocument.activeFilename = *filename;
    if (load_editor_scene()) {
        return true;
    }
    _sceneDocument.activeFilename = previousFilename;
    return false;
}

void VulkanEngine::build_sandbox_scene()
{
    const auto root = _scene.create_object("Sandbox");
    _sandboxRoot = root.persistentId;
    const auto floor = _scene.create_object("Floor", root);
    _floorObject = floor.persistentId;
    Transform transform; transform.scale = glm::vec3(50.f, 1.f, 50.f);
    _scene.set_local_transform(floor, transform);
    _scene.set(floor, Collider{true, CollisionShape::GroundPlane});
    assign_scene_asset(floor, SceneAssetKind::FloorQuad);
    struct WallDescription { const char* name; glm::vec3 position; glm::vec3 scale; };
    const std::array<WallDescription, 7> descriptions{{
        {"North Wall", {0.f, 1.5f, -24.75f}, {50.f, 3.f, 0.5f}},
        {"South Wall", {0.f, 1.5f, 24.75f}, {50.f, 3.f, 0.5f}},
        {"West Wall", {-24.75f, 1.5f, 0.f}, {0.5f, 3.f, 50.f}},
        {"East Wall", {24.75f, 1.5f, 0.f}, {0.5f, 3.f, 50.f}},
        {"Test North Panel", {0.f, 1.5f, -5.f}, {6.f, 3.f, 0.5f}},
        {"Test South Panel", {0.f, 1.5f, 5.f}, {6.f, 3.f, 0.5f}},
        {"Test East Panel", {5.f, 1.5f, 0.f}, {0.5f, 3.f, 6.f}},
    }};
    for (const auto& description : descriptions) {
        const auto wall = _scene.create_object(description.name, root);
        transform = {}; transform.position = description.position; transform.scale = description.scale;
        _scene.set_local_transform(wall, transform);
        _scene.set(wall, Collider{true, CollisionShape::Box});
        _scene.set(wall, PortalPlaceable{});
        assign_scene_asset(wall, SceneAssetKind::UnitCube);
    }
    create_runtime_scene_objects();
    rebuild_collision_from_scene();
}
