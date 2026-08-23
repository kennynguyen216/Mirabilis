#include "vk_engine.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"

namespace {

constexpr const char* EditorSceneDirectory = "../../assets/scenes";

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
}

