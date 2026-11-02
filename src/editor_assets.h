#pragma once
#include "scene_ecs.h"
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

enum class EditorCommand {
    OpenScene, ImportModel, AssignBaseColor, ReimportModel, Save, CreateCube,
    Delete, Duplicate, TogglePlay, ToggleDebug, Select, SetName, SetTransform
};
struct EditorRequest {
    EditorCommand command;
    std::filesystem::path path;
    EntityRef target{};
    uint64_t epoch{};
    bool discard{};
    int field{};
    float value{};
    std::string text;
};
struct EditorEntityRow {
    EntityRef reference;
    std::optional<PersistentID> parent;
    std::string name;
};
struct EditorSnapshot {
    uint64_t epoch{};
    bool editing{}, dirty{};
    std::string scene, status, selectedName;
    EntityRef selected{};
    std::array<float, 9> transform{};
    std::vector<EditorEntityRow> entities;
};
