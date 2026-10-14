#pragma once
#include "scene_ecs.h"
#include "portal.h"
#include <filesystem>
#include <span>

struct SceneSettings {
    uint32_t nextActor{1};
    std::string skybox{"legacy"};
    glm::vec3 sunDirection{0,1,0.5f};
    bool shadowsEnabled{true};
    float shadowRadius{60};
    float shadowDepthBias{0.0006f};
    float shadowNormalBias{0.08f};
    glm::vec3 sunRadiance{1};
    float environmentIntensity{1};
    glm::vec3 environmentTint{1};
    bool blackEnvironment{false};
    float warmth{0}, tint{0}, giSaturation{1};
    struct SSAO {
        bool enabled{false};
        float radius{0.75f}, bias{0.075f}, intensity{1}, power{1.5f};
    };
    std::optional<SSAO> ssao;
};
struct StagedScene {
    mirabilis::Scene scene;
    SceneSettings settings;
    std::vector<AuthoredPortalPair> authoredPortals;
    std::vector<std::string> warnings;
};
struct SceneDocumentSnapshot {
    SceneSnapshot entities;
    SceneSettings settings;
    std::vector<AuthoredPortalPair> authoredPortals;
};
constexpr size_t MaxSavedAuthoredPortalPairs = 7;
bool parse_scene_json(std::string_view json, std::string_view sourceFilename,
    const SceneSettings& defaults, StagedScene& out, std::string& error);
bool validate_authored_scene(const mirabilis::Scene&, const SceneSettings&,
    std::span<const AuthoredPortalPair>, std::string& error);
bool serialize_scene_json(const mirabilis::Scene&, const SceneSettings&,
    std::span<const AuthoredPortalPair>, std::string& json, std::string& error);
bool save_scene_atomic(const std::filesystem::path&, const mirabilis::Scene&,
    const SceneSettings&, std::span<const AuthoredPortalPair>, std::string& error);
