#include "vk_engine.h"
#include "vk_engine_render_helpers.h"
#include "physics_world.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <functional>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"

namespace {
constexpr const char* EditorSceneDirectory = "../../assets/scenes";
std::string EditorEditMessage;

bool draw_hierarchy_node(Scene& scene, PersistentID id, PersistentID& selected)
{
    const auto ref = scene.get(id);
    if (!scene.valid(ref)) return false;
    const auto* hierarchy = scene.read<Hierarchy>(ref);
    const auto* name = scene.read<Name>(ref);
    const auto children = hierarchy->children;
    const bool hadChildren = !children.empty();
    auto flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (!hadChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (id == selected) flags |= ImGuiTreeNodeFlags_Selected;
    const bool opened = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(id)), flags, "%s", name->value.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) selected = id;
    bool changed = false;
    if (!scene.runtime_only(ref) && !scene.pending(ref) && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("SCENE_OBJECT", &id, sizeof(id));
        ImGui::Text("Parent %s", name->value.c_str()); ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload("SCENE_OBJECT")) {
            if (payload->DataSize == sizeof(PersistentID)) {
                const auto child = scene.get(*static_cast<const PersistentID*>(payload->Data));
                if (scene.valid(child)) {
                    if (scene.set_parent(child, ref)) { selected = child.persistentId; changed = true; EditorEditMessage.clear(); }
                    else EditorEditMessage = scene.last_error();
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (opened && hadChildren) {
        for (const auto child : children) changed |= draw_hierarchy_node(scene, child, selected);
        ImGui::TreePop();
    }
    return changed;
}
} // namespace

void VulkanEngine::draw_hierarchy_panel()
{
    if (ImGui::Begin("Hierarchy")) {
        ImGui::BeginDisabled(!_editorMode);
        const auto roots = _scene.roots();
        bool changed = false;
        for (const auto id : roots) changed |= draw_hierarchy_node(_scene, id, _selectedSceneObject);
        if (changed) { _sceneDocument.dirty = true; rebuild_collision_from_scene(); }
        ImGui::EndDisabled();
        if (!EditorEditMessage.empty()) ImGui::TextWrapped("Edit refused: %s", EditorEditMessage.c_str());
    }
    ImGui::End();
}

void VulkanEngine::draw_inspector_panel()
{
    if (ImGui::Begin("Inspector")) {
        const EntityRef ref = _scene.get(_selectedSceneObject);
        if (!_scene.valid(ref)) {
            ImGui::TextUnformatted("No object selected.");
        } else {
            const auto name = *_scene.read<Name>(ref);
            auto transform = *_scene.read<Transform>(ref);
            auto visibility = *_scene.read<Visibility>(ref);
            auto collider = *_scene.read<Collider>(ref);
            auto material = *_scene.read<SceneMaterial>(ref);
            const auto mesh = *_scene.read<SceneMeshAsset>(ref);
            const auto role = *_scene.read<TimeTrial>(ref);
            bool portalPlaceable = _scene.has<PortalPlaceable>(ref);
            const bool runtimeOwned = _scene.runtime_only(ref);
            const auto existingBody = _scene.read<RigidBody>(ref);
            bool hasBody = existingBody != nullptr;
            RigidBody body = existingBody ? *existingBody : RigidBody{};
            bool bodyChanged = false;
            ImGui::BeginDisabled(!_editorMode || runtimeOwned || _scene.pending(ref));
            ImGui::Text("Name: %s", name.value.c_str());
            ImGui::Separator();

            // Transforms written by physics or portal placement would be
            // overwritten again next frame, so show them read-only.
            const bool driven = _scene.has<TransformDrivenExternally>(ref);
            bool changed = false;
            if (driven) {
                ImGui::TextUnformatted("Transform is driven by the simulation.");
            }
            ImGui::BeginDisabled(driven);
            changed |= ImGui::DragFloat3(
                "Position", &transform.position.x, 0.05f);

            glm::vec3 rotationDegrees = glm::degrees(transform.rotation);
            if (ImGui::DragFloat3("Rotation", &rotationDegrees.x, 0.5f)) {
                transform.rotation = glm::radians(rotationDegrees);
                changed = true;
            }

            changed |= ImGui::DragFloat3(
                "Scale", &transform.scale.x, 0.05f);
            ImGui::EndDisabled();

            ImGui::Separator();
            changed |= ImGui::Checkbox("Visible", &visibility.visible);
            changed |= ImGui::Checkbox("Has Collision", &collider.hasCollision);
            changed |= ImGui::Checkbox("Portal Placeable", &portalPlaceable);
            ImGui::SeparatorText("Rigid Body");
            bodyChanged |= ImGui::Checkbox("Rigid Body Settings", &hasBody);
            if (hasBody) {
                int motion = body.motion == BodyMotion::Dynamic ? 1 : 0;
                if (ImGui::Combo("Motion", &motion, "Static\0Dynamic\0")) {
                    body.motion = motion == 1 ? BodyMotion::Dynamic : BodyMotion::Static;
                    bodyChanged = true;
                }
                bodyChanged |= ImGui::DragFloat("Mass", &body.mass, 0.05f);
                bodyChanged |= ImGui::DragFloat("Friction", &body.friction, 0.01f);
                bodyChanged |= ImGui::DragFloat("Restitution", &body.restitution, 0.01f, 0.f, 1.f);
                bodyChanged |= ImGui::DragFloat("Gravity Factor", &body.gravityFactor, 0.01f);
                ImGui::TextDisabled("Dynamic requires a root entity with an enabled Box collider.");
            } else ImGui::TextDisabled("Enabled colliders use default Static body settings.");
            changed |= bodyChanged;
            const auto dynamic = _scene.dynamic_ancestor(ref);
            if (dynamic && dynamic != ref) {
                ImGui::TextWrapped("Visual-only under Dynamic entity %llu. Collider and Static settings are saved but create no body or player collision.", static_cast<unsigned long long>(dynamic.persistentId));
            } else if (collider.hasCollision && !dynamic && mirabilis::world_transform_has_shear(_scene.world_matrix(ref))) {
                ImGui::TextWrapped("Static collider has shear. Physics uses a best-fit oriented shape.");
            }
            if (portalPlaceable && dynamic) ImGui::TextWrapped("Portal Placeable is inactive inside a Dynamic subtree.");
            if (_scene.read<Hierarchy>(ref)->parent && ImGui::Button("Move to scene root")) {
                const auto world = _scene.world_matrix(ref);
                if (_scene.set_parent(ref, std::nullopt) && _scene.set_world_transform(ref, world)) {
                    transform = *_scene.read<Transform>(ref);
                    _sceneDocument.dirty = true;
                    rebuild_collision_from_scene();
                    EditorEditMessage.clear();
                } else EditorEditMessage = _scene.last_error();
            }
            if (role.role != TimeTrialRole::None) {
                const char* roleLabel = role.role == TimeTrialRole::SpawnPoint
                    ? "Spawn Point"
                    : (role.role == TimeTrialRole::StartTrigger
                        ? "Start Timer Trigger"
                        : "Finish Timer Trigger");
                ImGui::TextDisabled("Time-trial role: %s", roleLabel);
            }
            const bool needsEditableBox = collider.hasCollision ||
                role.role == TimeTrialRole::StartTrigger ||
                role.role == TimeTrialRole::FinishTrigger;
            if (needsEditableBox &&
                collider.collisionShape == CollisionShape::Box) {
                ImGui::SeparatorText("Box Collider");
                changed |= ImGui::DragFloat3(
                    "Center", &collider.colliderCenter.x, 0.05f);
                if (ImGui::DragFloat3(
                        "Half Extents",
                        &collider.colliderHalfExtents.x,
                        0.05f,
                        0.01f,
                        1000.0f)) {
                    collider.colliderHalfExtents = glm::max(
                        collider.colliderHalfExtents, glm::vec3(0.01f));
                    changed = true;
                }
                ImGui::TextDisabled(
                    "Center/size are local to this actor. Full size = half extents x 2.");
            } else if (collider.hasCollision &&
                       collider.collisionShape == CollisionShape::SurfRamp) {
                ImGui::SeparatorText("Surf Ramp Collider");
                ImGui::TextDisabled("Local +Z rises from the low edge to the high edge.");
                ImGui::TextDisabled("Scale X/Y/Z controls width, height, and length.");
                ImGui::TextDisabled("Steep ramps use surf physics; do not use a zero scale.");
            }

            const bool supportsMaterialOverride =
                mesh.assetKind == SceneAssetKind::FloorQuad ||
                mesh.assetKind == SceneAssetKind::UnitCube ||
                mesh.assetKind == SceneAssetKind::SurfRamp;
            if (supportsMaterialOverride) {
                ImGui::SeparatorText("Material");
                changed |= ImGui::Checkbox(
                    "Material Override", &material.enabled);
                ImGui::BeginDisabled(!material.enabled);
                changed |= ImGui::ColorEdit4(
                    "Tint", &material.colorTint.x);
                if(_rendererMode==RendererMode::SoftwarePathTrace) {
                    changed |= ImGui::ColorEdit3("Emission color",&material.emissionColor.x);
                    changed |= ImGui::DragFloat("Emission strength",&material.emissionStrength,0.05f,0,10000);
                    changed |= ImGui::SliderFloat("Dielectric transmission",&material.transmission,0,1);
                    changed |= ImGui::SliderFloat("Index of refraction",&material.ior,1,3);
                }
                changed |= ImGui::Checkbox(
                    "Debug Checker Grid", &material.debugChecker);
                changed |= ImGui::DragFloat(
                    "Metallic", &material.metallic,
                    0.01f, 0.0f, 1.0f, "%.2f");
                changed |= ImGui::DragFloat(
                    "Roughness", &material.roughness,
                    0.01f, 0.0f, 1.0f, "%.2f");
                if (ImGui::DragFloat2(
                        "UV Tiling", &material.uvScale.x,
                        0.05f, 0.01f, 100.0f, "%.2f")) {
                    material.uvScale = glm::max(
                        material.uvScale, glm::vec2(0.01f));
                    changed = true;
                }
                // One row per texture role.  The popup is opened outside the
                // pushed ID so its name matches the modal drawn below.
                const auto texturePicker = [&](const char* label,
                                               const char* popupName,
                                               std::string& path,
                                               const char* fallback) {
                    ImGui::TextWrapped(
                        "%s: %s", label, path.empty() ? fallback : path.c_str());
                    ImGui::PushID(popupName);
                    const bool choose = ImGui::Button("Choose...");
                    ImGui::SameLine();
                    if (ImGui::Button("Clear")) {
                        path.clear();
                        changed = true;
                    }
                    ImGui::PopID();
                    if (choose) {
                        _materialEditor.object = ref.persistentId;
                        std::fill(
                            _materialEditor.texturePathInput.begin(),
                            _materialEditor.texturePathInput.end(),
                            '\0');
                        std::copy_n(
                            path.data(),
                            std::min(
                                path.size(),
                                _materialEditor.texturePathInput.size() - 1),
                            _materialEditor.texturePathInput.data());
                        ImGui::OpenPopup(popupName);
                    }
                };
                texturePicker("Base color", "Base Color Texture",
                    material.baseColorTexturePath, "(white)");
                texturePicker("Normal map", "Normal Map Texture",
                    material.normalTexturePath, "(flat)");
                texturePicker("Metallic/roughness", "Metallic Roughness Texture",
                    material.metalRoughTexturePath, "(factors only)");
                ImGui::TextDisabled(
                    "Project example: ../../assets/textures/portal_concrete.png");
                ImGui::TextDisabled(
                    "Normal and metallic/roughness maps are linear data;");
                ImGui::TextDisabled(
                    "glTF packing: G roughness, B metallic.");
                ImGui::EndDisabled();
            } else if (mesh.assetKind == SceneAssetKind::ImportedGLTF) {
                ImGui::SeparatorText("Material");
                ImGui::TextDisabled(
                    "Imported glTF materials come from the model file.");
            }

            const std::array<std::pair<const char*, std::string SceneMaterial::*>, 3>
                texturePopups{{
                    {"Base Color Texture", &SceneMaterial::baseColorTexturePath},
                    {"Normal Map Texture", &SceneMaterial::normalTexturePath},
                    {"Metallic Roughness Texture",
                        &SceneMaterial::metalRoughTexturePath}}};
            for (const auto& [popupName, texturePath] : texturePopups) {
                if (!ImGui::BeginPopupModal(
                        popupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    continue;
                }
                ImGui::TextUnformatted(
                    "Enter a PNG/JPG path relative to bin/Debug.");
                ImGui::InputText(
                    "##TexturePath",
                    _materialEditor.texturePathInput.data(),
                    _materialEditor.texturePathInput.size());
                const auto materialRef = _scene.get(_materialEditor.object);
                ImGui::BeginDisabled(!_editorMode || !_scene.valid(materialRef) || _scene.runtime_only(materialRef) || _scene.pending(materialRef));
                if (ImGui::Button("Apply")) {
                    auto edited = *_scene.read<SceneMaterial>(materialRef);
                    edited.enabled = true;
                    edited.*texturePath = _materialEditor.texturePathInput.data();
                    _scene.set(materialRef, edited);
                    if (materialRef == ref) material = edited;
                    _sceneDocument.dirty = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (changed && _editorMode && _scene.mutable_ref(ref)) {
                const bool authoredHost = std::any_of(_authoredPortals.pairs.begin(), _authoredPortals.pairs.end(), [&](const AuthoredPortalPair& pair) {
                    return pair.first.hostWallObject == ref.persistentId || pair.second.hostWallObject == ref.persistentId;
                });
                if (!valid_component(transform) || !valid_component(visibility) || !valid_component(collider) || !valid_component(material) ||
                    (authoredHost && (!portalPlaceable || !collider.hasCollision || collider.collisionShape != CollisionShape::Box))) {
                    EditorEditMessage = "Invalid edit or authored portal host settings";
                } else if (!_scene.validate_physics_edit(ref, hasBody ? &body : nullptr, &collider, &transform)) {
                    EditorEditMessage = _scene.last_error();
                } else {
                    // Remove the old motion constraint before applying the validated combination.
                    if (bodyChanged && existingBody) _scene.remove<RigidBody>(ref);
                    if (!driven) _scene.set_local_transform(ref, transform);
                    _scene.set(ref, visibility); _scene.set(ref, collider); _scene.set(ref, material);
                    if (hasBody && bodyChanged) _scene.set(ref, body);
                    if (portalPlaceable) _scene.set(ref, PortalPlaceable{}); else _scene.remove<PortalPlaceable>(ref);
                    _sceneDocument.dirty = true;
                    rebuild_collision_from_scene();
                    EditorEditMessage.clear();
                }
            }
            if (!EditorEditMessage.empty()) ImGui::TextWrapped("Edit refused: %s", EditorEditMessage.c_str());

            if ((_bluePortal.placed && _bluePortal.hostWallObject == ref.persistentId) ||
                (_orangePortal.placed && _orangePortal.hostWallObject == ref.persistentId)) {
                ImGui::TextDisabled(
                    "Translation gizmo is disabled while a portal is attached.");
            }

            if (collider.hasCollision &&
                collider.collisionShape != CollisionShape::GroundPlane) {
                const AABB collider = collider_from_object(
                    _scene, ref.persistentId);
                ImGui::Text(
                    "Collider min %.2f %.2f %.2f",
                    collider.min.x, collider.min.y, collider.min.z);
                ImGui::Text(
                    "Collider max %.2f %.2f %.2f",
                    collider.max.x, collider.max.y, collider.max.z);
            }

            ImGui::EndDisabled();
            const bool isRequiredSandboxObject = ref.persistentId == _sandboxRoot || runtimeOwned;
            const bool hostsPlacedPortal =
                (_bluePortal.placed && _bluePortal.hostWallObject == ref.persistentId) ||
                (_orangePortal.placed && _orangePortal.hostWallObject == ref.persistentId);
            ImGui::Separator();
            ImGui::BeginDisabled(!_editorMode || isRequiredSandboxObject || hostsPlacedPortal);
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
    if (!_editorMode) return;
    const auto ref = _scene.get(_selectedSceneObject);
    const bool hasObject = _scene.valid(ref);
    Portal* authoredPortal = nullptr;
    if (!hasObject && _authoredPortals.selectedPair >= 0 &&
        _authoredPortals.selectedPair < static_cast<int>(_authoredPortals.pairs.size())) {
        AuthoredPortalPair& pair = _authoredPortals.pairs[_authoredPortals.selectedPair];
        authoredPortal = _authoredPortals.selectedSecond ? &pair.second : &pair.first;
    }
    if ((!hasObject && authoredPortal == nullptr) ||
        (hasObject && (_scene.runtime_only(ref) || _scene.pending(ref) || _scene.has<TransformDrivenExternally>(ref)))) {
        return;
    }

    // A portal's placement data is still owned by the portal system. Moving
    // its host wall while it has a placed portal would leave that portal off
    // the wall, so block the gizmo until portal parenting is introduced.
    if (hasObject &&
        ((_bluePortal.placed && _bluePortal.hostWallObject == ref.persistentId) ||
        (_orangePortal.placed && _orangePortal.hostWallObject == ref.persistentId))) {
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
    glm::mat4 worldTransform = authoredPortal != nullptr
        ? get_portal_frame(*authoredPortal)
        : _scene.world_matrix(ref.persistentId);

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
    ImGuizmo::SetRect(0.0f, 0.0f, displaySize.x, displaySize.y);

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    float snapValues[3]{
        _editorGizmo.translationSnap,
        _editorGizmo.translationSnap,
        _editorGizmo.translationSnap};
    switch (_editorGizmo.operation) {
    case EditorGizmoOperation::Translate:
        operation = ImGuizmo::TRANSLATE;
        break;
    case EditorGizmoOperation::Rotate:
        operation = ImGuizmo::ROTATE;
        snapValues[0] = _editorGizmo.rotationSnapDegrees;
        break;
    case EditorGizmoOperation::Scale:
        operation = ImGuizmo::SCALE;
        snapValues[0] = _editorGizmo.scaleSnap;
        break;
    }

    if (!ImGuizmo::Manipulate(
            glm::value_ptr(camera.getViewMatrix()),
            glm::value_ptr(gizmoProjection),
            operation,
            _editorGizmo.localSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
            glm::value_ptr(worldTransform),
            nullptr,
            _editorGizmo.snapping ? snapValues : nullptr)) {
        return;
    }

    if (authoredPortal != nullptr) {
        // Freestanding portal actors use their frame as the gizmo transform.
        // Scaling X/Y changes the opening dimensions; Z is deliberately ignored
        // because an aperture has no depth.
        authoredPortal->position = glm::vec3(worldTransform[3]);
        orient_portal(*authoredPortal, glm::vec3(worldTransform[2]));
        if (_editorGizmo.operation == EditorGizmoOperation::Scale) {
            authoredPortal->halfWidth = std::max(
                0.1f, authoredPortal->halfWidth * glm::length(glm::vec3(worldTransform[0])));
            authoredPortal->halfHeight = std::max(
                0.1f, authoredPortal->halfHeight * glm::length(glm::vec3(worldTransform[1])));
        }
        _sceneDocument.dirty = true;
        return;
    }

    // ImGuizmo edits a world matrix. Scene objects store local components, so
    // convert back through the parent's world matrix before saving the change.
    if (!_scene.set_world_transform(ref, worldTransform)) { EditorEditMessage = _scene.last_error(); return; }
    EditorEditMessage.clear();

    // Keep collision and portal raycasts coherent while the arrow is dragged.
    _sceneDocument.dirty = true;
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
    for (const auto ref : _scene.creation_order()) {
        const auto* visibility = _scene.read<Visibility>(ref);
        const auto* collider = _scene.read<Collider>(ref);
        // Version 1 selects the scene's axis-aligned editable primitives.
        // Imported models get mesh picking later, once their bounds are
        // represented in the scene system.
        if (!visibility || !collider || _scene.pending(ref) || !visibility->visible || !collider->hasCollision ||
            collider->collisionShape == CollisionShape::GroundPlane) {
            continue;
        }

        const std::optional<RaycastHit> hit = raycast_aabb(
            rayOrigin,
            rayDirection,
            collider_from_object(_scene, ref.persistentId));
        if (hit.has_value() &&
            (!closestHit.has_value() || hit->distance < closestHit->distance)) {
            closestHit = hit;
            closestObject = ref.persistentId;
        }
    }

    float closestDistance = closestHit.has_value()
        ? closestHit->distance
        : std::numeric_limits<float>::infinity();
    int closestPortalPair = -1;
    bool closestPortalSecond = false;
    const auto testPortal = [&](const Portal& portal, int pairIndex, bool second) {
        const float denominator = glm::dot(rayDirection, portal.normal);
        if (std::abs(denominator) < 0.00001f) return;
        const float distance = glm::dot(portal.position - rayOrigin, portal.normal) / denominator;
        if (distance <= 0.0f || distance >= closestDistance) return;
        const glm::vec3 hitPoint = rayOrigin + rayDirection * distance;
        const glm::vec3 localPoint = glm::vec3(
            glm::inverse(get_portal_frame(portal)) * glm::vec4(hitPoint, 1.0f));
        if (std::abs(localPoint.x) <= portal.halfWidth &&
            std::abs(localPoint.y) <= portal.halfHeight) {
            closestDistance = distance;
            closestPortalPair = pairIndex;
            closestPortalSecond = second;
        }
    };
    for (size_t pairIndex = 0; pairIndex < _authoredPortals.pairs.size(); ++pairIndex) {
        testPortal(_authoredPortals.pairs[pairIndex].first, static_cast<int>(pairIndex), false);
        testPortal(_authoredPortals.pairs[pairIndex].second, static_cast<int>(pairIndex), true);
    }

    if (closestPortalPair >= 0) {
        _selectedSceneObject = InvalidSceneObject;
        _authoredPortals.selectedPair = closestPortalPair;
        _authoredPortals.selectedSecond = closestPortalSecond;
    } else if (closestObject != InvalidSceneObject) {
        _selectedSceneObject = closestObject;
        _authoredPortals.selectedPair = -1;
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
            _editorInputs.sceneName.fill('\0');
            const size_t copyLength = std::min(
                _sceneDocument.activeFilename.size(), _editorInputs.sceneName.size() - 1);
            std::memcpy(
                _editorInputs.sceneName.data(),
                _sceneDocument.activeFilename.data(),
                copyLength);
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
                        filename == _sceneDocument.activeFilename)) {
                    load_editor_scene_named(filename);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::TextDisabled("../../assets/scenes/%s%s",
            _sceneDocument.activeFilename.c_str(),
            _sceneDocument.dirty ? "  (unsaved changes)" : "");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Create")) {
        const auto createPortraitProp = [&](const char* name,
                                            const char* texturePath,
                                            const glm::vec3& scale) {
            const SceneObjectID id = create_editor_actor(
                name, SceneAssetKind::UnitCube, false);
            const auto portrait = _scene.get(id);
            if (_scene.valid(portrait)) {
                auto transform = *_scene.read<Transform>(portrait); transform.scale = scale;
                auto material = *_scene.read<SceneMaterial>(portrait);
                material.enabled = true; material.baseColorTexturePath = texturePath;
                material.metallic = 0.f; material.roughness = 0.9f;
                _scene.set_local_transform(portrait, transform); _scene.set(portrait, material);
            }
        };
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
        ImGui::SeparatorText("Prefabs");
        if (ImGui::MenuItem("Room with Center Pole")) {
            create_room_with_center_pole_prefab();
        }
        if (ImGui::MenuItem("Long Closed Room")) {
            create_closed_long_room_prefab();
        }
        ImGui::SeparatorText("Portrait Props");
        if (ImGui::MenuItem("Jayden Standee")) {
            createPortraitProp(
                "Jayden Standee", "../../assets/textures/jayden.png",
                glm::vec3(1.25f, 2.2f, 0.08f));
        }
        if (ImGui::MenuItem("Tamely Swag Standee")) {
            createPortraitProp(
                "Tamely Swag Standee", "../../assets/textures/tamely_swag.png",
                glm::vec3(1.6f, 1.6f, 0.08f));
        }
        if (ImGui::MenuItem("Ricky Standee")) {
            createPortraitProp(
                "Ricky Standee", "../../assets/textures/ricky.png",
                glm::vec3(1.35f, 1.8f, 0.08f));
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Spawn Point")) {
            // This first time-trial loop uses one respawn marker. Replacing
            // it leaves the old marker as an ordinary editable cube.
            for (const auto ref : _scene.creation_order()) {
                const auto* role = _scene.read<TimeTrial>(ref);
                if (role && role->role == TimeTrialRole::SpawnPoint) _scene.set(ref, TimeTrial{});
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
            _editorInputs.gltfPath.fill('\0');
            ImGui::OpenPopup("Import GLB/glTF");
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        const bool hasSelection = _scene.valid(_scene.get(_selectedSceneObject));
        if (ImGui::BeginMenu("Gizmo")) {
            if (ImGui::MenuItem(
                    "Move", "W",
                    _editorGizmo.operation == EditorGizmoOperation::Translate)) {
                _editorGizmo.operation = EditorGizmoOperation::Translate;
            }
            if (ImGui::MenuItem(
                    "Rotate", "E",
                    _editorGizmo.operation == EditorGizmoOperation::Rotate)) {
                _editorGizmo.operation = EditorGizmoOperation::Rotate;
            }
            if (ImGui::MenuItem(
                    "Scale", "R",
                    _editorGizmo.operation == EditorGizmoOperation::Scale)) {
                _editorGizmo.operation = EditorGizmoOperation::Scale;
            }
            ImGui::Separator();
            ImGui::MenuItem("Local space", nullptr, &_editorGizmo.localSpace);
            ImGui::MenuItem("Enable snapping", "S", &_editorGizmo.snapping);
            if (_editorGizmo.operation == EditorGizmoOperation::Translate) {
                ImGui::DragFloat(
                    "Move snap", &_editorGizmo.translationSnap, 0.05f, 0.05f, 10.0f);
            } else if (_editorGizmo.operation == EditorGizmoOperation::Rotate) {
                ImGui::DragFloat(
                    "Rotation snap", &_editorGizmo.rotationSnapDegrees, 1.0f, 1.0f, 180.0f);
            } else {
                ImGui::DragFloat(
                    "Scale snap", &_editorGizmo.scaleSnap, 0.05f, 0.01f, 10.0f);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Duplicate Selected Hierarchy", "Ctrl+D", false, hasSelection)) {
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
        ImGui::SeparatorText("Authored Portal Links");
        ImGui::TextDisabled("Endpoints are freestanding; they appear five metres ahead of the editor camera.");
        ImGui::BeginDisabled(_authoredPortals.pairs.size() >= MaxAuthoredPortalPairs);
        if (ImGui::MenuItem(
                _authoredPortals.draft.has_value()
                    ? "Place Second Endpoint"
                    : "Place First Endpoint")) {
            place_authored_portal_endpoint();
        }
        ImGui::EndDisabled();
        if (ImGui::MenuItem("Clear Authored Links", nullptr, false,
                            !_authoredPortals.pairs.empty() || _authoredPortals.draft.has_value())) {
            clear_authored_portals();
        }
        if (ImGui::MenuItem("Set Up Three-Room Pole Chain", nullptr, false,
                            _authoredPortals.pairs.empty())) {
            create_three_room_pole_chain();
        }
        ImGui::TextDisabled("Creates Room 1 -> 2 -> 3 links at their center poles.");
        ImGui::TextDisabled("Links: %zu / %zu%s", _authoredPortals.pairs.size(),
                            MaxAuthoredPortalPairs,
                            _authoredPortals.draft.has_value() ? " (one endpoint waiting)" : "");
        for (size_t pairIndex = 0; pairIndex < _authoredPortals.pairs.size(); ++pairIndex) {
            AuthoredPortalPair& pair = _authoredPortals.pairs[pairIndex];
            ImGui::PushID(static_cast<int>(pairIndex));
            if (_authoredPortals.selectedPair == static_cast<int>(pairIndex)) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            }
            if (ImGui::TreeNode("Link", "Link %zu", pairIndex + 1)) {
                const auto editEndpoint = [&](const char* label, Portal& portal) {
                    ImGui::TextUnformatted(label);
                    ImGui::PushID(label);
                    bool changed = ImGui::DragFloat3("Position", &portal.position.x, 0.1f);
                    glm::vec3 normal = portal.normal;
                    if (ImGui::DragFloat3("Facing", &normal.x, 0.02f, -1.0f, 1.0f) &&
                        glm::length(normal) > 0.01f) {
                        orient_portal(portal, normal);
                        changed = true;
                    }
                    changed |= ImGui::DragFloat("Half Width", &portal.halfWidth, 0.02f, 0.1f, 20.0f);
                    changed |= ImGui::DragFloat("Half Height", &portal.halfHeight, 0.02f, 0.1f, 20.0f);
                    if (changed) _sceneDocument.dirty = true;
                    ImGui::TextDisabled("Ctrl-click a numeric field to type an exact value.");
                    const glm::vec3 right = glm::normalize(
                        glm::cross(portal.up, portal.normal));
                    const glm::vec3 bottomLeft = portal.position -
                        right * portal.halfWidth - portal.up * portal.halfHeight;
                    const glm::vec3 bottomRight = portal.position +
                        right * portal.halfWidth - portal.up * portal.halfHeight;
                    const glm::vec3 topLeft = portal.position -
                        right * portal.halfWidth + portal.up * portal.halfHeight;
                    const glm::vec3 topRight = portal.position +
                        right * portal.halfWidth + portal.up * portal.halfHeight;
                    ImGui::SeparatorText("World-space corners");
                    ImGui::Text("Top Left     %.3f, %.3f, %.3f",
                                topLeft.x, topLeft.y, topLeft.z);
                    ImGui::Text("Top Right    %.3f, %.3f, %.3f",
                                topRight.x, topRight.y, topRight.z);
                    ImGui::Text("Bottom Left  %.3f, %.3f, %.3f",
                                bottomLeft.x, bottomLeft.y, bottomLeft.z);
                    ImGui::Text("Bottom Right %.3f, %.3f, %.3f",
                                bottomRight.x, bottomRight.y, bottomRight.z);
                    ImGui::PopID();
                };
                editEndpoint("First endpoint", pair.first);
                ImGui::Separator();
                editEndpoint("Second endpoint", pair.second);
                if (_authoredPortals.selectedPair == static_cast<int>(pairIndex)) {
                    ImGui::TextDisabled("Selected: %s endpoint",
                        _authoredPortals.selectedSecond ? "second" : "first");
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::MenuItem(
            "Use Offscreen Camera Experiment",
            nullptr,
            &_portalCameras.useOffscreen);
        ImGui::BeginDisabled(_portalCameras.useOffscreen);
        ImGui::MenuItem(
            "Direct Stencil Recursion",
            nullptr,
            &_portalRender.recursionEnabled);
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Time Trial")) {
        if (ImGui::MenuItem("Reset Play", nullptr, false, !_editorMode)) reset_play();
        if (ImGui::MenuItem("Reset Run")) {
            reset_time_trial();
        }
        ImGui::TextDisabled(
            "Create Spawn Point, Start Timer Trigger, and Finish Timer Trigger.");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window")) {
        ImGui::MenuItem("Show Debug Panels", nullptr, &_editorUi.showDebugPanels);
        if (ImGui::MenuItem("Reset Editor Layout")) {
            _editorUi.resetLayoutRequested = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem(
            "Show Collider Bounds",
            nullptr,
            &_debugViews.showColliderBounds,
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
        ImGui::InputText(
            "Filename", _editorInputs.sceneName.data(), _editorInputs.sceneName.size());
        ImGui::TextDisabled(".json is added automatically.");
        if (ImGui::Button("Save")) {
            if (save_editor_scene_as(_editorInputs.sceneName.data())) {
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
        ImGui::InputText(
            "Asset path", _editorInputs.gltfPath.data(), _editorInputs.gltfPath.size());
        if (ImGui::Button("Import")) {
            if (import_gltf_actor(_editorInputs.gltfPath.data()) != InvalidSceneObject) {
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
    if (!_editorUi.resetLayoutRequested && ImGui::DockBuilderGetNode(dockspaceID) != nullptr) {
        return;
    }

    _editorUi.resetLayoutRequested = false;
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
    const auto ref = _scene.get(_selectedSceneObject);
    if (!_editorMode || !_scene.valid(ref) || ref.persistentId == _sandboxRoot || _scene.runtime_only(ref)) return false;
    const bool hostsPlacedPortal = (_bluePortal.placed && _bluePortal.hostWallObject == ref.persistentId) || (_orangePortal.placed && _orangePortal.hostWallObject == ref.persistentId);
    if (hostsPlacedPortal || !_scene.destroy_object(ref)) return false;
    drain_scene_destruction();
    _selectedSceneObject = InvalidSceneObject;
    _sceneDocument.dirty = true;
    rebuild_collision_from_scene();
    return true;
}

bool VulkanEngine::duplicate_selected_scene_object()
{
    const auto source = _scene.get(_selectedSceneObject);
    if (!_editorMode || !_scene.valid(source) || source.persistentId == _sandboxRoot || _scene.runtime_only(source)) return false;
    const auto branchOrder = [&](EntityRef root) {
        std::vector<PersistentID> ids;
        const auto walk = [&](auto&& self, EntityRef ref) -> void {
            if (!_scene.valid(ref) || _scene.runtime_only(ref) || _scene.pending(ref)) return;
            ids.push_back(ref.persistentId);
            for (const auto id : _scene.read<Hierarchy>(ref)->children) self(self, _scene.get(id));
        };
        walk(walk, root); return ids;
    };
    const auto sources = branchOrder(source);
    const auto duplicate = _scene.duplicate_subtree(source);
    if (!_scene.valid(duplicate)) return false;
    const auto copies = branchOrder(duplicate);
    _scene.set(duplicate, Name{_scene.read<Name>(source)->value + " Prefab " + std::to_string(_nextCreatedActorNumber++)});
    auto transform = *_scene.read<Transform>(duplicate);
    transform.position += glm::vec3(0.5f, 0.f, 0.5f);
    _scene.set_local_transform(duplicate, transform);
    for (size_t index = 0; index < copies.size(); ++index) {
        const auto ref = _scene.get(copies[index]);
        if (index < sources.size()) {
            const auto runtime = _sceneRenderRuntimes.find(sources[index]);
            if (runtime != _sceneRenderRuntimes.end()) {
                const auto resource = runtime->second;
                _sceneRenderRuntimes[copies[index]] = resource;
                continue;
            }
        }
        assign_scene_asset(ref, _scene.read<SceneMeshAsset>(ref)->assetKind);
    }
    _selectedSceneObject = duplicate.persistentId;
    _sceneDocument.dirty = true;
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
    if (!_editorMode) return InvalidSceneObject;
    const std::string name = std::string(baseName) + " " +
        std::to_string(_nextCreatedActorNumber++);
    const auto ref = _scene.create_object(name, _scene.get(_sandboxRoot));
    const auto id = ref.persistentId;
    if (!_scene.valid(ref)) {
        return InvalidSceneObject;
    }
    Transform transform;
    Collider collider;
    _scene.set(ref, TimeTrial{timeTrialRole});

    const Camera& camera = render_camera();
    const glm::vec3 forward = glm::normalize(glm::vec3(
        camera.getRotationMatrix() * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
    const glm::vec3 spawnWorldPosition = camera.position + forward * 5.0f;
    const glm::mat4 parentWorld = _sandboxRoot == InvalidSceneObject
        ? glm::mat4(1.0f)
        : _scene.world_matrix(_sandboxRoot);
    transform.position = glm::vec3(
        glm::inverse(parentWorld) * glm::vec4(spawnWorldPosition, 1.0f));

    if (assetKind != SceneAssetKind::None) {
        assign_scene_asset(ref, assetKind);
    }
    if (assetKind == SceneAssetKind::UnitCube) {
        transform.scale = collidable
            ? glm::vec3(4.0f, 3.0f, 0.5f)
            : glm::vec3(1.0f);
    } else if (assetKind == SceneAssetKind::FloorQuad) {
        transform.scale = glm::vec3(10.0f, 1.0f, 10.0f);
        collider.collisionShape = CollisionShape::GroundPlane;
    } else if (assetKind == SceneAssetKind::SurfRamp) {
        transform.scale = glm::vec3(8.0f, 12.0f, 12.0f);
        collider.collisionShape = CollisionShape::SurfRamp;
        // Matches the wedge's local bounding box, for editor picking/debug.
        collider.colliderCenter = glm::vec3(0.0f, 0.5f, 0.0f);
        collider.colliderHalfExtents = glm::vec3(0.5f);
    }

    if (timeTrialRole == TimeTrialRole::SpawnPoint) {
        // A low marker shows the exact feet position used by fall reset.
        transform.scale = glm::vec3(0.6f, 0.12f, 0.6f);
    } else if (timeTrialRole == TimeTrialRole::StartTrigger ||
               timeTrialRole == TimeTrialRole::FinishTrigger) {
        // A trigger is a non-solid volume. Its visible cube and editable
        // local box use the same dimensions, so what you see is what starts
        // or finishes the run.
        transform.scale = glm::vec3(2.0f, 1.0f, 2.0f);
        collider.colliderCenter = glm::vec3(0.0f);
        collider.colliderHalfExtents = glm::vec3(0.5f);
    }

    collider.hasCollision = collidable;
    _scene.set_local_transform(ref, transform); _scene.set(ref, collider);
    if (portalPlaceable) _scene.set(ref, PortalPlaceable{});
    _selectedSceneObject = id;
    _sceneDocument.dirty = true;
    rebuild_collision_from_scene();
    return id;
}

SceneObjectID VulkanEngine::create_room_with_center_pole_prefab()
{
    if (!_editorMode) return InvalidSceneObject;
    // Keep every part local to one empty root, so the room is movable,
    // duplicable, and saveable as a single hierarchy prefab.
    const auto root = _scene.create_object(
        "Room with Center Pole " + std::to_string(_nextCreatedActorNumber++),
        _scene.get(_sandboxRoot));
    if (!_scene.valid(root)) return InvalidSceneObject;
    const auto rootID = root.persistentId;
    Transform rootTransform;

    const Camera& camera = render_camera();
    const glm::vec3 forward = glm::normalize(glm::vec3(
        camera.getRotationMatrix() * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
    glm::vec3 spawnWorldPosition = camera.position + forward * 5.0f;
    // A room is authored from its floor upward. Keep it on the world ground
    // instead of centring it at the editor camera's height.
    spawnWorldPosition.y = 0.0f;
    const glm::mat4 parentWorld = _sandboxRoot == InvalidSceneObject
        ? glm::mat4(1.0f)
        : _scene.world_matrix(_sandboxRoot);
    rootTransform.position = glm::vec3(
        glm::inverse(parentWorld) * glm::vec4(spawnWorldPosition, 1.0f));

    _scene.set_local_transform(root, rootTransform);
    const auto addPart = [&](const char* name, SceneAssetKind assetKind,
                             const glm::vec3& position,
                             const glm::vec3& scale, bool collidable,
                             bool portalPlaceable) {
        const auto part = _scene.create_object(name, root);
        if (!_scene.valid(part)) return;
        Transform transform; Collider collider; SceneMaterial material;
        assign_scene_asset(part, assetKind);
        transform.position = position;
        transform.scale = scale;
        collider.hasCollision = collidable;
        if (portalPlaceable) _scene.set(part, PortalPlaceable{});
        if (assetKind == SceneAssetKind::FloorQuad) {
            // FloorQuad is a walkable horizontal platform, not a paper-thin
            // box collider. Without this the player falls through a prefab
            // room's floor and immediately hits the respawn safeguard.
            collider.collisionShape = CollisionShape::GroundPlane;
        }
        if (assetKind == SceneAssetKind::UnitCube) {
            material.enabled = true;
            material.colorTint = glm::vec4(1.0f);
            material.baseColorTexturePath =
                "../../assets/textures/room_concrete.png";
            material.uvScale = glm::vec2(4.0f);
            material.metallic = 0.0f;
            material.roughness = 0.9f;
        }
        _scene.set_local_transform(part, transform); _scene.set(part, collider); _scene.set(part, material);
    };

    // Twelve-metre square interior, six metres high. UnitCube spans one
    // metre at scale 1, so these are full dimensions (not half extents).
    addPart("Floor", SceneAssetKind::FloorQuad,
            glm::vec3(0.0f), glm::vec3(12.0f, 1.0f, 12.0f), true, false);
    addPart("Ceiling", SceneAssetKind::FloorQuad,
            glm::vec3(0.0f, 6.0f, 0.0f), glm::vec3(12.0f, 1.0f, 12.0f), false, false);
    addPart("North Wall", SceneAssetKind::UnitCube,
            glm::vec3(0.0f, 3.0f, -6.0f), glm::vec3(12.0f, 6.0f, 0.3f), true, true);
    // Leave a generous central doorway in the south wall so this prefab can
    // be entered normally instead of requiring a portal to get inside.
    addPart("South Wall Left", SceneAssetKind::UnitCube,
            glm::vec3(-3.625f, 3.0f, 6.0f), glm::vec3(4.75f, 6.0f, 0.3f), true, true);
    addPart("South Wall Right", SceneAssetKind::UnitCube,
            glm::vec3(3.625f, 3.0f, 6.0f), glm::vec3(4.75f, 6.0f, 0.3f), true, true);
    addPart("Doorway Header", SceneAssetKind::UnitCube,
            glm::vec3(0.0f, 4.5f, 6.0f), glm::vec3(2.5f, 3.0f, 0.3f), true, true);
    addPart("West Wall", SceneAssetKind::UnitCube,
            glm::vec3(-6.0f, 3.0f, 0.0f), glm::vec3(0.3f, 6.0f, 12.0f), true, true);
    addPart("East Wall", SceneAssetKind::UnitCube,
            glm::vec3( 6.0f, 3.0f, 0.0f), glm::vec3(0.3f, 6.0f, 12.0f), true, true);
    addPart("Center Pole", SceneAssetKind::UnitCube,
            glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.5f, 6.0f, 0.5f), true, true);

    // The prefab is immediately playable: it has its own active spawn just
    // inside the room. Keep the engine rule of one active spawn point.
    for (const auto ref : _scene.creation_order()) {
        const auto* role = _scene.read<TimeTrial>(ref);
        if (role && role->role == TimeTrialRole::SpawnPoint) _scene.set(ref, TimeTrial{});
    }
    const auto spawn = _scene.create_object("Room Spawn Point", root);
    if (_scene.valid(spawn)) {
        Transform transform;
        _scene.set(spawn, TimeTrial{TimeTrialRole::SpawnPoint});
        assign_scene_asset(spawn, SceneAssetKind::UnitCube);
        transform.position = glm::vec3(0.0f, 0.05f, -3.0f);
        transform.scale = glm::vec3(0.6f, 0.12f, 0.6f);
        _scene.set_local_transform(spawn, transform);
    }

    _selectedSceneObject = rootID;
    _sceneDocument.dirty = true;
    rebuild_collision_from_scene();
    return rootID;
}

SceneObjectID VulkanEngine::create_closed_long_room_prefab()
{
    if (!_editorMode) return InvalidSceneObject;
    const auto root = _scene.create_object(
        "Long Closed Room " + std::to_string(_nextCreatedActorNumber++),
        _scene.get(_sandboxRoot));
    if (!_scene.valid(root)) return InvalidSceneObject;
    const auto rootID = root.persistentId;
    Transform rootTransform;

    const Camera& camera = render_camera();
    const glm::vec3 forward = glm::normalize(glm::vec3(
        camera.getRotationMatrix() * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
    glm::vec3 spawnWorldPosition = camera.position + forward * 8.0f;
    spawnWorldPosition.y = 0.0f;
    const glm::mat4 parentWorld = _sandboxRoot == InvalidSceneObject
        ? glm::mat4(1.0f)
        : _scene.world_matrix(_sandboxRoot);
    rootTransform.position = glm::vec3(
        glm::inverse(parentWorld) * glm::vec4(spawnWorldPosition, 1.0f));

    _scene.set_local_transform(root, rootTransform);
    const auto addPart = [&](const char* name, SceneAssetKind assetKind,
                             const glm::vec3& position,
                             const glm::vec3& scale, bool collidable,
                             bool portalPlaceable) {
        const auto part = _scene.create_object(name, root);
        if (!_scene.valid(part)) return;
        Transform transform; Collider collider; SceneMaterial material;
        assign_scene_asset(part, assetKind);
        transform.position = position;
        transform.scale = scale;
        collider.hasCollision = collidable;
        if (portalPlaceable) _scene.set(part, PortalPlaceable{});
        if (assetKind == SceneAssetKind::FloorQuad) {
            collider.collisionShape = CollisionShape::GroundPlane;
        } else {
            material.enabled = true;
            material.baseColorTexturePath =
                "../../assets/textures/room_concrete.png";
            material.uvScale = glm::vec2(5.0f, 4.0f);
            material.metallic = 0.0f;
            material.roughness = 0.9f;
        }
        _scene.set_local_transform(part, transform); _scene.set(part, collider); _scene.set(part, material);
    };

    // A sealed 12 x 20 x 6 metre room for portal/non-Euclidean experiments.
    addPart("Floor", SceneAssetKind::FloorQuad,
            glm::vec3(0.0f), glm::vec3(12.0f, 1.0f, 20.0f), true, false);
    addPart("Ceiling", SceneAssetKind::FloorQuad,
            glm::vec3(0.0f, 6.0f, 0.0f), glm::vec3(12.0f, 1.0f, 20.0f), false, false);
    addPart("North Wall", SceneAssetKind::UnitCube,
            glm::vec3(0.0f, 3.0f, -10.0f), glm::vec3(12.0f, 6.0f, 0.3f), true, true);
    addPart("South Wall", SceneAssetKind::UnitCube,
            glm::vec3(0.0f, 3.0f, 10.0f), glm::vec3(12.0f, 6.0f, 0.3f), true, true);
    addPart("West Wall", SceneAssetKind::UnitCube,
            glm::vec3(-6.0f, 3.0f, 0.0f), glm::vec3(0.3f, 6.0f, 20.0f), true, true);
    addPart("East Wall", SceneAssetKind::UnitCube,
            glm::vec3(6.0f, 3.0f, 0.0f), glm::vec3(0.3f, 6.0f, 20.0f), true, true);

    for (const auto ref : _scene.creation_order()) {
        const auto* role = _scene.read<TimeTrial>(ref);
        if (role && role->role == TimeTrialRole::SpawnPoint) _scene.set(ref, TimeTrial{});
    }
    const auto spawn = _scene.create_object("Long Room Spawn Point", root);
    if (_scene.valid(spawn)) {
        Transform transform;
        _scene.set(spawn, TimeTrial{TimeTrialRole::SpawnPoint});
        assign_scene_asset(spawn, SceneAssetKind::UnitCube);
        transform.position = glm::vec3(0.0f, 0.05f, -7.0f);
        transform.scale = glm::vec3(0.6f, 0.12f, 0.6f);
        _scene.set_local_transform(spawn, transform);
    }

    _selectedSceneObject = rootID;
    _sceneDocument.dirty = true;
    rebuild_collision_from_scene();
    return rootID;
}

SceneObjectID VulkanEngine::import_gltf_actor(std::string_view modelPath)
{
    if (!_editorMode || modelPath.empty()) {
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
    const auto ref = _scene.create_object(
        baseName + " " + std::to_string(_nextCreatedActorNumber++),
        _scene.get(_sandboxRoot));
    const auto id = ref.persistentId;
    if (!_scene.valid(ref)) {
        return InvalidSceneObject;
    }

    const Camera& camera = render_camera();
    const glm::vec3 forward = glm::normalize(glm::vec3(
        camera.getRotationMatrix() * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
    const glm::mat4 parentWorld = _scene.world_matrix(_sandboxRoot);
    const glm::vec3 spawnWorldPosition = camera.position + forward * 5.0f;
    Transform transform; transform.position = glm::vec3(
        glm::inverse(parentWorld) * glm::vec4(spawnWorldPosition, 1.0f));
    _scene.set_local_transform(ref, transform);
    _scene.set(ref, SceneMeshAsset{SceneAssetKind::ImportedGLTF, std::string(modelPath)});
    _sceneRenderRuntimes[id].model = *model;

    _selectedSceneObject = id;
    _sceneDocument.dirty = true;
    return id;
}

void VulkanEngine::assign_scene_asset(
    EntityRef ref,
    SceneAssetKind assetKind)
{
    if (!_scene.valid(ref)) return;
    auto mesh = *_scene.read<SceneMeshAsset>(ref);
    mesh.assetKind = assetKind;
    if (!_scene.set(ref, mesh, true)) return;
    auto& runtime = _sceneRenderRuntimes[ref.persistentId];
    runtime.primitive = {}; runtime.model.reset();
    const auto* role = _scene.read<TimeTrial>(ref);
    const auto* name = _scene.read<Name>(ref);

    switch (assetKind) {
    case SceneAssetKind::FloorQuad:
        runtime.primitive = MeshPrimitive{
            .indexCount = 6,
            .firstIndex = 0,
            .indexBuffer = _floorMesh.indexBuffer.buffer,
            .vertexBufferAddress = _floorMesh.vertexBufferAddress,
            .bounds = _floorBounds,
            .material = &_floorMaterial};
        break;
    case SceneAssetKind::UnitCube:
        runtime.primitive = MeshPrimitive{
            .indexCount = 36,
            .firstIndex = 0,
            .indexBuffer = _wallMesh.indexBuffer.buffer,
            .vertexBufferAddress = _wallMesh.vertexBufferAddress,
            .bounds = _wallBounds,
            .material = &_wallMaterial};
        break;
    case SceneAssetKind::SurfRamp:
        runtime.primitive = MeshPrimitive{
            .indexCount = 24,
            .firstIndex = 0,
            .indexBuffer = _rampMesh.indexBuffer.buffer,
            .vertexBufferAddress = _rampMesh.vertexBufferAddress,
            .bounds = _rampBounds,
            .material = &_wallMaterial};
        break;
    case SceneAssetKind::ImportedGLTF: {
        if (mesh.modelPath.empty()) {
            fmt::print("Scene glTF actor '{}' has no model path.\n", name->value);
            break;
        }
        auto model = loadGltf(this, mesh.modelPath);
        if (model) {
            runtime.model = *model;
        } else {
            fmt::print(
                "Failed to load scene glTF '{}' for actor '{}'.\n",
                mesh.modelPath,
                name->value);
        }
        break;
    }
    case SceneAssetKind::None:
        break;
    }

    // Reuse the bright portal/player materials for level-authoring markers.
    // Material pointers are rebuilt from the saved role every time a scene is
    // loaded, just like the primitive itself.
    if (runtime.primitive.valid()) {
        if (role->role == TimeTrialRole::SpawnPoint) {
            runtime.primitive.material = &_playerMaterial;
        } else if (role->role == TimeTrialRole::StartTrigger) {
            runtime.primitive.material = &_bluePortalMaterial;
        } else if (role->role == TimeTrialRole::FinishTrigger) {
            runtime.primitive.material = &_orangePortalMaterial;
        }
    }
}

void VulkanEngine::draw_sun_shadow_settings()
{
    if (ImGui::CollapsingHeader("Sun & Shadows")) {
        // These are saved with the scene, so editing one is an edit to the
        // level rather than a session preference.
        bool lightingEdited = false;
        lightingEdited |= ImGui::Checkbox("Cast Shadows", &_shadow.enabled);
        // The direction points from a surface towards the sun.
        if (ImGui::SliderFloat3(
                "Sun Direction", &_shadow.sunlightDirection.x, -1.0f, 1.0f)) {
            lightingEdited = true;
            if (glm::dot(_shadow.sunlightDirection, _shadow.sunlightDirection) <
                0.000001f) {
                // A zero direction cannot define a light camera.
                _shadow.sunlightDirection = glm::vec3(0.0f, 1.0f, 0.5f);
            }
        }
        lightingEdited |= ImGui::SliderFloat(
            "Shadow Radius", &_shadow.radius, 10.0f, 200.0f);
        ImGui::Checkbox("Show Shadow Bounds", &_shadow.showBounds);
        // Too little bias and surfaces shadow themselves; too much and a
        // shadow detaches from the object casting it.  The R4.41 candidate
        // derives both from the shadow texel instead (build_scene_data());
        // the authored values apply only under the same-build control.
        const bool authoredShadowBias =
            SDL_getenv("MIRABILIS_R4_AUTHORED_SHADOW_CONTROL") != nullptr;
        ImGui::BeginDisabled(!authoredShadowBias);
        lightingEdited |= ImGui::SliderFloat(
            "Depth Bias", &_shadow.depthBias, 0.0f, 0.005f, "%.5f");
        lightingEdited |= ImGui::SliderFloat(
            "Normal Bias", &_shadow.normalBias, 0.0f, 0.5f, "%.3f");
        ImGui::EndDisabled();
        if (!authoredShadowBias) {
            ImGui::TextDisabled("Bias derived from shadow texel size (R4.41)");
        }
        ImGui::SliderFloat(
            "Shadow Softness", &_shadow.filterRadius,
            0.0f, 12.0f, "%.2f texels");
        if (lightingEdited) {
            _sceneDocument.dirty = true;
        }
    }
}

void VulkanEngine::draw_material_shading_settings()
{
    if (ImGui::CollapsingHeader("Material Shading")) {
        // Session preferences, so none of this marks the level dirty.
        ImGui::Checkbox("Normal maps", &_materialShading.normalMaps);
        ImGui::Checkbox(
            "Metallic/roughness textures", &_materialShading.metalRoughTextures);
        ImGui::Checkbox(
            "Specular anti-aliasing", &_materialShading.specularAntiAliasing);
        ImGui::Checkbox("Specular", &_materialShading.specular);
        ImGui::Checkbox(
            "Image-based lighting", &_materialShading.imageBasedLighting);
        if (!_materialShading.imageBasedLighting) {
            ImGui::TextDisabled("Flat ambient colour instead of the skybox.");
        }
        if (!_materialShading.specular) {
            ImGui::TextDisabled("Lambert only, as before the GGX model.");
        }
        // The path tracer has no normal maps, texture roughness, or specular
        // anti-aliasing, so only with all three off should its direct image
        // and the raster one agree.
        if (ImGui::Button("Match reference renderer")) {
            _materialShading.normalMaps = false;
            _materialShading.metalRoughTextures = false;
            _materialShading.specularAntiAliasing = false;
        }
    }
}

void VulkanEngine::draw_ambient_occlusion_settings()
{
    if (ImGui::CollapsingHeader("Ambient Occlusion")) {
        if (_ssao.format == VK_FORMAT_UNDEFINED) {
            ImGui::TextDisabled(
                "Unavailable: no storage-capable occlusion format.");
        } else {
            // Radius, bias, intensity and power describe how this level is lit
            // and travel with the scene.  Quality describes what the machine
            // can afford and does not.
            bool preferencesEdited =
                ImGui::Checkbox("Globally Enabled", &_ssao.globalEnabled);
            if (ImGui::Checkbox("Override For This Scene", &_ssao.sceneOverride)) {
                if (!_ssao.sceneOverride) _ssao.settings = SSAOSettings{};
                _sceneDocument.dirty = true;
            }
            ImGui::BeginDisabled(!_ssao.sceneOverride);
            bool occlusionEdited = false;
            occlusionEdited |=
                ImGui::Checkbox("Scene Enabled", &_ssao.settings.enabled);
            occlusionEdited |= ImGui::SliderFloat(
                "Radius", &_ssao.settings.radius, 0.05f, 5.0f, "%.3f");
            occlusionEdited |= ImGui::SliderFloat(
                "Bias", &_ssao.settings.bias, 0.0f, 0.1f, "%.4f");
            occlusionEdited |= ImGui::SliderFloat(
                "Intensity", &_ssao.settings.intensity, 0.0f, 2.0f);
            occlusionEdited |= ImGui::SliderFloat(
                "Power", &_ssao.settings.power, 0.25f, 4.0f);
            if (occlusionEdited) {
                _sceneDocument.dirty = true;
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Reset to Project Defaults")) {
                _ssao.settings = SSAOSettings{};
                _ssao.sceneOverride = false;
                _sceneDocument.dirty = true;
            }

            const char* qualityNames[] = {"Low", "Medium", "High"};
            if (ImGui::Combo(
                    "Quality",
                    &_ssao.quality,
                    qualityNames,
                    IM_ARRAYSIZE(qualityNames))) {
                preferencesEdited = true;
            }
            ImGui::TextDisabled("%d samples", SSAOKernelSizes[_ssao.quality]);

            // How readily the blur accepts a neighbour as being on the same
            // surface.  Both depend on world scale, but they are filter tuning
            // rather than lighting.
            preferencesEdited |= ImGui::SliderFloat(
                "Blur Depth Falloff", &_ssao.depthFalloff, 5.0f, 120.0f);
            preferencesEdited |= ImGui::SliderFloat(
                "Blur Normal Falloff", &_ssao.normalFalloff, 1.0f, 48.0f);
            if (preferencesEdited) save_ao_preferences();

            // With the sun off, occlusion is the only thing shaping the image.
            // Setting ambient to zero as well should then produce no visible
            // difference at all, which is the check that it touches nothing
            // else.
            ImGui::Checkbox(
                "Ambient Only (occlusion check)", &_ssao.ambientOnly);
        }
    }
}

void VulkanEngine::draw_tonemap_settings()
{
    if (ImGui::CollapsingHeader("Tonemapping")) {
        // Exposure is a session preference; white balance is scene-authored.
        ImGui::Checkbox("Enabled##Tonemapping", &_postProcess.tonemapEnabled);
        if (!_postProcess.tonemapEnabled) {
            ImGui::TextDisabled(
                "Linear HDR is written straight to an 8-bit\n"
                "buffer: midtones read dark and highlights clip.");
        }
        if (_postProcess.tonemapEnabled) {
            const char* operatorNames[] = {"ACES Filmic", "Reinhard"};
            ImGui::Combo(
                "Operator",
                &_postProcess.tonemapOperator,
                operatorNames,
                IM_ARRAYSIZE(operatorNames));
            // IQ3: metered from the frame's luminance; manual when off.
            if (ImGui::Checkbox("Auto Exposure", &_autoExposure.enabled) &&
                    _autoExposure.enabled) {
                _autoExposure.snap = true;
            }
            if (_autoExposure.enabled) {
                ImGui::Text("Exposure %.2f (EV %.2f)",
                    _postProcess.tonemapExposure, _autoExposure.ev);
                ImGui::SliderFloat("Compensation (EV)",
                    &_autoExposure.compensation, -4.0f, 4.0f, "%.1f");
                ImGui::SliderFloat("Min EV", &_autoExposure.minEv, -8.0f, 8.0f, "%.1f");
                ImGui::SliderFloat("Max EV", &_autoExposure.maxEv, -8.0f, 12.0f, "%.1f");
                ImGui::SliderFloat("Adaptation (1/s)",
                    &_autoExposure.speed, 0.1f, 10.0f, "%.1f");
            } else {
                ImGui::SliderFloat(
                    "Exposure", &_postProcess.tonemapExposure, 0.05f, 8.0f, "%.2f");
            }
            // Separates the two things this pass does, so a frame that looks
            // wrong can be blamed on the curve or on the transfer function
            // rather than on both at once.
            ImGui::Checkbox(
                "Bypass Curve (encode only)", &_postProcess.tonemapBypassCurve);
            ImGui::SeparatorText("White balance");
            bool balanceEdited = ImGui::SliderFloat("Cool / warm", &_postProcess.whiteBalance.x, -1, 1, "%.2f");
            balanceEdited |= ImGui::SliderFloat("Green / magenta", &_postProcess.whiteBalance.y, -1, 1, "%.2f");
            if (ImGui::Button("Reset white balance")) { _postProcess.whiteBalance = glm::vec2(0); balanceEdited = true; }
            if (balanceEdited) _sceneDocument.dirty = true;
            if (_debugViews.view != RenderDebugView::None) {
                ImGui::TextDisabled("Inactive while a debug view is shown.");
            }
        }
    }
}

void VulkanEngine::draw_antialiasing_settings()
{
    if (ImGui::CollapsingHeader("Anti-Aliasing")) {
        // A session preference rather than a scene property, so none of this
        // marks the level dirty.
        const char* modeNames[] = {"Off", "FXAA"};
        int mode = _postProcess.fxaaEnabled ? 1 : 0;
        if (ImGui::Combo("Mode", &mode, modeNames, IM_ARRAYSIZE(modeNames))) {
            _postProcess.fxaaEnabled = mode == 1;
        }
        if (_postProcess.fxaaEnabled) {
            // Lower catches more edges; too low and the filter starts
            // softening texture detail that never aliased.
            ImGui::SliderFloat(
                "Edge Threshold", &_postProcess.fxaaEdgeThreshold, 0.03f, 0.25f, "%.3f");
            ImGui::SliderFloat(
                "Subpixel Strength", &_postProcess.fxaaSubpixelStrength, 0.0f, 1.0f, "%.2f");
            // White marks every pixel the threshold accepted. Tuning against
            // this is far easier than judging the threshold from the finished
            // image.
            ImGui::Checkbox("Debug Edges", &_postProcess.fxaaShowEdges);
            if (!_postProcess.tonemapEnabled) {
                ImGui::TextDisabled(
                    "Inactive: FXAA reads the tonemapped image.");
            }
            if (_debugViews.view != RenderDebugView::None) {
                ImGui::TextDisabled(
                    "Suspended while a debug view is shown.");
            }
        }
    }
}

void VulkanEngine::draw_screen_buffer_settings()
{
    if (ImGui::CollapsingHeader("Screen-Space Buffers")) {
        ImGui::Checkbox("Depth/Normal Prepass", &_prepass.enabled);
        // Reading a half-finished buffer directly is far more informative than
        // trying to infer a projection or orientation mistake from a finished
        // effect.
        int debugView = static_cast<int>(_debugViews.view);
        if (ImGui::Combo(
                "Debug View",
                &debugView,
                RenderDebugViewNames.data(),
                static_cast<int>(RenderDebugViewNames.size()))) {
            _debugViews.view = static_cast<RenderDebugView>(debugView);
        }
        if (_debugViews.view == RenderDebugView::SDFTrace) {
            draw_sdf_settings();
        }
        if (_debugViews.view == RenderDebugView::SurfaceCache) {
            draw_surface_cache_settings();
        }
        // Easy to misread otherwise: these buffers hold the light arriving at
        // a surface, not the colour it reflects.  A white wall and a red one
        // under the same bounce now look identical here, and differ only after
        // the composite.
        if (is_ssgi_indirect_radiance_view(_debugViews.view)) {
            ImGui::TextDisabled(
                "SSGI buffers hold incident radiance; albedo is");
            ImGui::TextDisabled(
                "applied in the composite, not in the trace.");
        }
        if (_debugViews.view == RenderDebugView::Depth) {
            ImGui::SliderFloat(
                "Depth View Range", &_debugViews.depthRange, 5.0f, 500.0f);
        }
        ImGui::SeparatorText("SSGI Milestone 5");
        ImGui::Checkbox("Run SSGI", &_ssgi.enabled);
        ImGui::Checkbox("Lumen-lite World Fallback", &_ssgi.lumenEnabled);
        if (_ssgi.lumenEnabled && ImGui::Checkbox("Screen Trace", &_ssgi.screenTraceEnabled))
            _ssgi.historyValid = false;
        ImGui::Checkbox("Screen Probes", &_ssgi.probesEnabled);
        ImGui::Checkbox("Hierarchical Screen Trace", &_ssgi.hzbEnabled);
        if (_ssgi.lumenEnabled && !lumen_lite_ready()) {
            ImGui::TextDisabled("Waiting for the scene field and surface cache;");
            ImGui::TextDisabled("bake assets/sdf/queue if meshes are missing.");
        }
        const char* ssgiPresets[] = {
            "Validation (full resolution)",
            "High (half resolution)",
            "Balanced (half resolution)",
            "Performance (half resolution)",
            "Peak (full resolution, 8 rays)"};
        int ssgiPreset = _ssgi.qualityPreset;
        if (ImGui::Combo("Quality Preset", &ssgiPreset,
                ssgiPresets, IM_ARRAYSIZE(ssgiPresets))) {
            apply_ssgi_quality_preset(ssgiPreset);
        }
        ImGui::SliderInt("Ray Steps", &_ssgi.stepCount, 8, 96);
        ImGui::SliderInt("Rays Per Pixel", &_ssgi.raysPerPixel, 1, 8);
        ImGui::SliderFloat("Ray Length", &_ssgi.rayLength, 1.0f, 40.0f, "%.2f");
        ImGui::SliderFloat("Thickness", &_ssgi.thickness, 0.01f, 2.0f, "%.3f");
        ImGui::SliderFloat(
            "Start Offset", &_ssgi.startOffset, 0.001f, 0.5f, "%.3f");
        ImGui::SliderFloat(
            "History Weight", &_ssgi.historyWeight, 0.0f, 0.98f, "%.3f");
        ImGui::SliderFloat(
            "History Relative Depth Reject", &_ssgi.depthRejection,
            0.0001f, 0.02f, "%.4f");
        ImGui::SliderFloat(
            "History Normal Reject", &_ssgi.normalRejection,
            0.0f, 1.0f, "%.3f");
        ImGui::SliderFloat(
            "History Velocity Reject", &_ssgi.velocityRejection,
            0.005f, 0.5f, "%.3f");
        ImGui::SeparatorText("SSGI Milestone 6");
        ImGui::Checkbox("Spatial Filter", &_ssgi.spatialFilterEnabled);
        ImGui::SliderInt("Filter Radius", &_ssgi.filterRadius, 1, 8);
        ImGui::SliderFloat(
            "Filter Plane Falloff", &_ssgi.filterDepthFalloff,
            10.0f, 4000.0f, "%.1f");
        ImGui::SliderFloat(
            "Filter Normal Power", &_ssgi.filterNormalPower,
            1.0f, 128.0f, "%.1f");
        ImGui::SeparatorText("SSGI Milestone 7");
        ImGui::SliderFloat(
            "Indirect Intensity", &_ssgi.intensity, 0.0f, 2.0f, "%.2f");
        if (ImGui::SliderFloat("GI Color Saturation", &_ssgi.colorSaturation, 0.0f, 1.0f, "%.2f"))
            _sceneDocument.dirty = true;
        ImGui::TextWrapped("Lower this to soften colored GI and sky fill. Surface colors and direct lights stay colored.");
        ImGui::TextDisabled("SSGI owns diffuse environment light while it runs.");
        // A traced ray that leaves the depth buffer has to be filled from
        // somewhere.  The analytic gradient is what the software path tracer
        // still uses, so it stays reachable for reference comparisons.
        ImGui::Checkbox("Miss Rays Sample Skybox", &_ssgi.traceEnvironmentMap);
        if (_ssgi.traceEnvironmentMap) {
            ImGui::TextDisabled(
                "Misses read %s at mip %.1f.",
                SkyboxDisplayNames[_skyboxSelection],
                _skyboxEnvironmentLod);
            ImGui::TextDisabled(
                "Sky/sun split at %.2f; above it is the sun,",
                _skyboxIndirectClamp);
            ImGui::TextDisabled(
                "which the direct term already delivers.");
        } else {
            ImGui::TextDisabled("Misses use the analytic gradient, matching");
            ImGui::TextDisabled(
                "the software path tracer's environment.");
        }
        if (stats.ssgi_time_is_gpu) {
            const VkExtent2D ssgiExtent = active_ssgi_extent();
            ImGui::Text(
                "SSGI GPU %.3f ms (%ux%u)",
                stats.ssgi_total_time,
                ssgiExtent.width,
                ssgiExtent.height);
            ImGui::TextDisabled(
                "Trace %.3f | temporal %.3f | filter %.3f | composite %.3f ms",
                stats.ssgi_raw_time,
                stats.ssgi_temporal_time,
                stats.ssgi_filter_time,
                stats.ssgi_composite_time);
        }
        ImGui::TextDisabled("Green rejection view pixels accepted history.");
    }
}

void VulkanEngine::draw_background_effect_settings()
{
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

void VulkanEngine::draw_movement_tuning_panel()
{
    if (ImGui::Begin("Movement Tuning")) {
        PlayerMovementSettings& movement = _playerMovement.settings;
        ImGui::SliderFloat("Gravity", &movement.gravity, 1.0f, 60.0f);
        ImGui::SliderFloat("Jump Speed", &movement.jumpSpeed, 1.0f, 20.0f);
        ImGui::SliderFloat(
            "Ground Speed", &movement.maxGroundSpeed, 1.0f, 20.0f);
        ImGui::SliderFloat(
            "Ground Accel", &movement.groundAcceleration, 1.0f, 100.0f);
        ImGui::SliderFloat(
            "Ground Friction", &movement.groundFriction, 0.0f, 20.0f);
        ImGui::SliderFloat("Air Accel", &movement.airAcceleration, 0.0f, 50.0f);
        ImGui::SliderFloat(
            "Air Wish Cap", &movement.airWishSpeedCap, 0.1f, 20.0f);
        ImGui::SliderFloat(
            "Jump Buffer", &movement.jumpBufferSeconds, 0.0f, 0.25f);
    }
    ImGui::End();
}

void VulkanEngine::draw_statistics_panel(float horizontalSpeed)
{
    if (ImGui::Begin("Statistics")) {
        ImGui::Text("Speed %.2f", horizontalSpeed);
        ImGui::Text(
            "FPS %.1f (%.3f ms)",
            stats.frametime > 0.0f ? 1000.0f / stats.frametime : 0.0f,
            stats.frametime);
        ImGui::Text("Scene update %.3f ms", stats.scene_update_time);
        ImGui::Text("Mesh draw %.3f ms", stats.mesh_draw_time);
        ImGui::Text("Triangles %d", stats.triangle_count);
        ImGui::Separator();
        ImGui::Text("World draw calls %d", stats.world_drawcall_count);
        ImGui::Text("Portal draw calls %d", stats.portal_drawcall_count);
        ImGui::Text("Total draw calls %d", stats.drawcall_count);
        ImGui::Separator();
        // The shadow and prepass passes walk the scene again with their own
        // culling, so none of their cost appears in the counters above.
        ImGui::Text(
            "Shadow map %ux%u %s",
            ShadowMapResolution,
            ShadowMapResolution,
            _shadow.formatName);
        ImGui::Text(
            "Shadow draws %d (%d tris)",
            stats.shadow_drawcall_count,
            stats.shadow_triangle_count);
        ImGui::Text("Shadow record %.3f ms", stats.shadow_record_time);
        ImGui::Text(
            "Prepass draws %d (%d tris)",
            stats.prepass_drawcall_count,
            stats.prepass_triangle_count);
        ImGui::Text("Prepass record %.3f ms", stats.prepass_record_time);
        ImGui::Separator();
        if (stats.ssao_kernel_samples == 0) {
            ImGui::Text("Ambient occlusion: off");
        } else {
            ImGui::Text(
                "AO %dx%d %s, %d samples",
                stats.ssao_width,
                stats.ssao_height,
                _ssao.formatName,
                stats.ssao_kernel_samples);
            // Three dispatches take microseconds to record and milliseconds to
            // run, so a CPU number here would be actively misleading.  It is
            // labelled when that is all the device can provide.
            const char* unit = stats.ssao_time_is_gpu ? "ms" : "ms CPU";
            ImGui::Text("  sample  %.3f %s", stats.ssao_raw_time, unit);
            ImGui::Text(
                "  blur H  %.3f %s", stats.ssao_blur_horizontal_time, unit);
            ImGui::Text(
                "  blur V  %.3f %s", stats.ssao_blur_vertical_time, unit);
            ImGui::Text("  total   %.3f %s", stats.ssao_total_time, unit);
        }
        ImGui::Separator();
        ImGui::Text(
            "Portal mode: %s",
            _portalCameras.useOffscreen
                ? "Offscreen camera (primary only)"
                : (_portalRender.recursionEnabled
                    ? "Direct stencil (one recursive level)"
                    : "Direct stencil (primary only)"));
    }
    ImGui::End();
}

void VulkanEngine::draw_play_overlay(float horizontalSpeed)
{
    ImGui::SetNextWindowPos(
        ImVec2(ImGui::GetIO().DisplaySize.x - 18.0f, 18.0f),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.78f);
    if (ImGui::Begin("Play Controls", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::Button("Reset Play")) reset_play();
        ImGui::SameLine();
        if (ImGui::Button("Respawn (F1)")) respawn_player();
        ImGui::SameLine();
        if (ImGui::Button(_noClip.enabled
                ? "Disable No Clip (F2)"
                : "Enable No Clip (F2)")) {
            _noClip.enabled = !_noClip.enabled;
            _noClip.up = false;
            _noClip.down = false;
            _playerMovement.velocity = glm::vec3(0.0f);
        }
        if (_noClip.enabled) {
            ImGui::SliderFloat("No Clip Speed", &_noClip.speed, 2.0f, 40.0f);
            ImGui::TextDisabled("WASD move, Space up, Ctrl down");
            ImGui::TextDisabled("Hold Alt to free the cursor and drag this.");
        }
    }
    ImGui::End();

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImDrawList* crosshair = ImGui::GetForegroundDrawList();
    // Portal-style status reticle: blue is left mouse and orange is right
    // mouse. Bright means the portal is placed; dim means it is available and
    // the next click will create it.
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
    const std::string fpsLabel = fmt::format(
        "{:.0f} FPS",
        stats.frametime > 0.0f ? 1000.0f / stats.frametime : 0.0f);
    crosshair->AddText(
        ImVec2(18.0f, 18.0f),
        IM_COL32(220, 230, 245, 255),
        fpsLabel.c_str());
    crosshair->AddText(
        ImVec2(18.0f, ImGui::GetIO().DisplaySize.y - 56.0f),
        IM_COL32(150, 165, 185, 210),
        "LMB Blue | RMB Orange | R Retract | F1 Respawn | "
        "F2 No Clip | Hold Alt: Menus");

    const char* timerState = _timeTrial.running
        ? "RUNNING"
        : (_timeTrial.finished ? "FINISHED" : "READY");
    const std::string timerLabel = fmt::format(
        "{}  {:02}:{:06.3f}",
        timerState,
        static_cast<int>(_timeTrial.seconds / 60.0f),
        std::fmod(_timeTrial.seconds, 60.0f));
    const ImVec2 timerSize = ImGui::CalcTextSize(timerLabel.c_str());
    crosshair->AddText(
        ImVec2(center.x - timerSize.x * 0.5f, 26.0f),
        _timeTrial.finished
            ? IM_COL32(100, 245, 155, 255)
            : IM_COL32(235, 240, 250, 255),
        timerLabel.c_str());
    if (_timeTrial.bestSeconds >= 0.0f) {
        const std::string bestLabel = fmt::format(
            "BEST {:02}:{:06.3f}",
            static_cast<int>(_timeTrial.bestSeconds / 60.0f),
            std::fmod(_timeTrial.bestSeconds, 60.0f));
        const ImVec2 bestSize = ImGui::CalcTextSize(bestLabel.c_str());
        crosshair->AddText(
            ImVec2(center.x - bestSize.x * 0.5f, 48.0f),
            IM_COL32(255, 210, 90, 255),
            bestLabel.c_str());
    }
}

void VulkanEngine::draw_frame_ui(float deltaTime)
{
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
    
        if (_editorUi.showDebugPanels) {
            if (ImGui::Begin("Render Settings")) {
                draw_path_trace_ui();
                ImGui::SliderFloat("Resolution Scale", &renderScale, 0.3f, 1.0f);
                int skyboxSelection = _skyboxSelection;
                if (ImGui::Combo(
                        "Skybox",
                        &skyboxSelection,
                        SkyboxDisplayNames.data(),
                        static_cast<int>(SkyboxDisplayNames.size()))) {
                    if (set_skybox(skyboxSelection)) {
                        _authoredSkyboxId = SkyboxIds[skyboxSelection];
                        _sceneDocument.dirty = true;
                    }
                }
                if (_skyboxImage.imageFormat ==
                    VK_FORMAT_R16G16B16A16_SFLOAT) {
                    ImGui::TextDisabled(
                        "True HDR source (RGBA16F on GPU)");
                }
                if (ImGui::Button("Apply Maximum Fidelity")) {
                    apply_max_fidelity_settings();
                }
                ImGui::SameLine();
                ImGui::TextDisabled(
                    "4K shadows, full-res 8-ray SSGI, high SSAO sampling, quality FXAA");
                if (ImGui::Button("Apply Performance (60 FPS Target)")) {
                    apply_performance_settings();
                }
                ImGui::SameLine();
                ImGui::TextDisabled(
                    "No shadows, half-res SSGI, no SSAO, frame rate capped");
                ImGui::SeparatorText("Frame Rate");
                ImGui::Checkbox("Cap Frame Rate", &_frameRateCapEnabled);
                if (_frameRateCapEnabled) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::SliderFloat(
                        "Target FPS", &_targetFrameRate, 30.0f, 240.0f, "%.0f");
                }
                ImGui::Text(
                    "%.1f FPS (%.3f ms)",
                    stats.frametime > 0.0f ? 1000.0f / stats.frametime : 0.0f,
                    stats.frametime);
                ImGui::SeparatorText("Cinematic Capture");
                if (_cinematic.recording) {
                    ImGui::ProgressBar(
                        float(_cinematic.frameIndex) /
                            float(std::max(1, _cinematic.totalFrames - 1)),
                        ImVec2(-1, 0));
                    ImGui::TextDisabled(
                        "Recording frame %d / %d to '%s'",
                        _cinematic.frameIndex, _cinematic.totalFrames,
                        _cinematic.outputDirectory.c_str());
                } else {
                    if (ImGui::Button("Record Cinematic Shot (6s)")) {
                        begin_cinematic_recording(180, "cinematic");
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled(
                        "Yaw sweep + push-in from the current camera pose");
                    if (!_cinematic.lastCompletedDirectory.empty()) {
                        std::error_code errorCode;
                        const auto absolutePath = std::filesystem::absolute(
                            _cinematic.lastCompletedDirectory, errorCode);
                        ImGui::TextColored(
                            ImVec4(0.55f, 0.95f, 0.6f, 1.0f),
                            "Saved %d frames to %s",
                            _cinematic.lastCompletedFrames,
                            errorCode
                                ? _cinematic.lastCompletedDirectory.c_str()
                                : absolutePath.string().c_str());
                        ImGui::TextDisabled(
                            "Encode: ffmpeg -framerate 30 -i "
                            "frame_%%04d.ppm -c:v libx264 -pix_fmt yuv420p "
                            "out.mp4");
                    } else {
                        ImGui::TextDisabled(
                            "Writes cinematic/frame_%%04d.ppm; encode with");
                        ImGui::TextDisabled(
                            "ffmpeg once it finishes (path shown here then).");
                    }
                }
                draw_sun_shadow_settings();
                draw_material_shading_settings();
                draw_ambient_occlusion_settings();
                draw_tonemap_settings();
                draw_antialiasing_settings();
                draw_screen_buffer_settings();
                draw_background_effect_settings();
            }
            ImGui::End();
    
            draw_movement_tuning_panel();
        }
    }
    
    if (_editorMode) {
        draw_editor_gizmo();
    }
    
    stats.frametime = deltaTime * 1000.0f;
    const float horizontalSpeed = glm::length(glm::vec2(
        _playerMovement.velocity.x,
        _playerMovement.velocity.z));
    if (_editorMode && _editorUi.showDebugPanels) {
        draw_statistics_panel(horizontalSpeed);
    }
    
    if (!_editorMode) {
        draw_play_overlay(horizontalSpeed);
    }
}
