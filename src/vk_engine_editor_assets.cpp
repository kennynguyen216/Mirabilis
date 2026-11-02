#include "vk_engine.h"
#include <SDL.h>
#include <algorithm>
#include <cctype>
#include <cmath>

std::filesystem::path VulkanEngine::editor_asset_root() const {
    if (const char* root = SDL_getenv("MIRABILIS_ASSET_ROOT"); root && *root)
        return std::filesystem::weakly_canonical(root);
    char* base = SDL_GetBasePath();
    const auto root = std::filesystem::weakly_canonical(std::filesystem::path(base ? base : ".") / "../../assets");
    SDL_free(base); return root;
}
EditorSnapshot VulkanEngine::editor_snapshot() const {
    EditorSnapshot result;
    result.epoch = _scene.epoch(); result.editing = _editorMode; result.dirty = _sceneDocument.dirty;
    result.scene = _sceneDocument.activeFilename; result.status = _editorAssetStatus;
    for (const auto ref : _scene.hierarchy_order()) {
        const auto* name = _scene.read<Name>(ref); const auto* hierarchy = _scene.read<Hierarchy>(ref);
        result.entities.push_back({ref, hierarchy->parent, name->value});
    }
    result.rendering={_postProcess.celShading?1.f:0.f,_postProcess.celBands,_postProcess.celEdge,
        _postProcess.bloomEnabled?1.f:0.f,_postProcess.bloomStrength,_postProcess.bloomThreshold,_postProcess.bloomScatter,
        float(_postProcess.tonemapOperator),float(_ssgi.qualityPreset),renderScale,_meshVisibility.enabled?1.f:0.f,_reflections.enabled?1.f:0.f,_reflections.roughnessLimit,_ssgi.lumenEnabled?1.f:0.f,_ssgi.probesEnabled?1.f:0.f};
    result.selected = _scene.get(_selectedSceneObject);
    if (_scene.valid(result.selected)) {
        result.selectedName = _scene.read<Name>(result.selected)->value;
        if(const auto* light=_scene.read<Light>(result.selected))result.light=*light;
        const auto kind=_scene.read<SceneMeshAsset>(result.selected)->assetKind;
        if(kind!=SceneAssetKind::None)result.material=*_scene.read<SceneMaterial>(result.selected);
        result.importedModel=kind==SceneAssetKind::ImportedGLTF;
        const auto& transform = *_scene.read<Transform>(result.selected);
        for (int i = 0; i < 3; ++i) {
            result.transform[i] = transform.position[i];
            result.transform[i + 3] = glm::degrees(transform.rotation[i]);
            result.transform[i + 6] = transform.scale[i];
        }
    }
    return result;
}
void VulkanEngine::queue_editor_request(EditorRequest request) {
    if (!request.epoch) request.epoch = _scene.epoch();
    if (!request.target) request.target = _scene.get(_selectedSceneObject);
    _editorRequests.push_back(std::move(request));
}
bool VulkanEngine::save_from_editor() {
    if (!_editorMode) { _editorAssetStatus = "Save is available in Edit mode"; return false; }
    const bool saved = save_editor_scene();
    _editorAssetStatus = saved ? "Scene saved" : "Could not save the current scene";
    return saved;
}
void VulkanEngine::release_editor_input() {
    set_mouse_capture(false); _editorCamera.velocity = {}; _playerInput = {};
    if (_scriptHost) _scriptHost->input().focus_lost();
}
void VulkanEngine::request_editor_quit() {
    _editorQuitRequested = true;
    SDL_Event event{}; event.type = SDL_QUIT; SDL_PushEvent(&event);
}
void VulkanEngine::apply_editor_requests() {
    for (size_t count = 0; count < 32 && !_editorRequests.empty() && !_editorQuitRequested; ++count) {
        auto request = std::move(_editorRequests.front()); _editorRequests.pop_front();
        try {
            if (request.epoch != _scene.epoch()) { _editorAssetStatus = "Scene changed; retry the action"; continue; }
            if (request.command == EditorCommand::ToggleDebug) { _editorUi.showDebugPanels = !_editorUi.showDebugPanels; continue; }
            if (request.command == EditorCommand::TogglePlay) { set_editor_mode(!_editorMode); release_editor_input(); continue; }
            if (request.command == EditorCommand::Select) {
                if (_scene.valid(request.target)) _selectedSceneObject = request.target.persistentId;
                continue;
            }
            if (!_editorMode) { _editorAssetStatus = "Return to Edit mode to change authored content"; continue; }
            if (request.command == EditorCommand::CreatePointLight || request.command == EditorCommand::CreateSpotLight) {
                const bool spot=request.command==EditorCommand::CreateSpotLight;
                const auto ref=_scene.create_object(spot?"Spot Light":"Point Light",_scene.get(_sandboxRoot));
                Light light;light.type=spot?LightType::Spot:LightType::Point;
                Transform transform;transform.position=render_camera().position+glm::vec3(render_camera().getRotationMatrix()*glm::vec4(0,0,-4,0));
                _scene.set_local_transform(ref,transform);_scene.set(ref,light);_selectedSceneObject=ref.persistentId;
                _sceneDocument.dirty=true;_ssgi.historyValid=false;_editorAssetStatus="Light created";continue;
            }
            if(request.command==EditorCommand::SetRendering) {
                if(!std::isfinite(request.value)){_editorAssetStatus="Invalid rendering value";continue;}
                const float v=request.value;bool valid=true;
                switch(request.field) {
                case 0:_postProcess.celShading=v>0.5f;break;
                case 1:valid=v>=2&&v<=16;if(valid)_postProcess.celBands=v;break;
                case 2:valid=v>=0&&v<=1;if(valid)_postProcess.celEdge=v;break;
                case 3:_postProcess.bloomEnabled=v>0.5f;break;
                case 4:valid=v>=0&&v<=2;if(valid)_postProcess.bloomStrength=v;break;
                case 5:valid=v>=0&&v<=100;if(valid)_postProcess.bloomThreshold=v;break;
                case 6:valid=v>=0&&v<=1;if(valid)_postProcess.bloomScatter=v;break;
                case 7:valid=v>=0&&v<=2&&v==std::floor(v);if(valid)_postProcess.tonemapOperator=int(v);break;
                case 8:valid=v>=0&&v<=5&&v==std::floor(v);if(valid)apply_ssgi_quality_preset(int(v));break;
                case 9:valid=v>=0.25f&&v<=1;if(valid){renderScale=v;_ssgi.historyValid=false;}break;
                case 10:_meshVisibility.enabled=v>0.5f&&_meshVisibility.supported;break;
                case 11:_reflections.enabled=v>0.5f;_reflections.historyValid=false;break;
                case 12:valid=v>=0&&v<=1;if(valid){_reflections.roughnessLimit=v;_reflections.historyValid=false;}break;
                case 13:_ssgi.lumenEnabled=v>0.5f;_ssgi.historyValid=false;break;
                case 14:_ssgi.probesEnabled=v>0.5f;_ssgi.historyValid=false;break;
                default:valid=false;break;
                }
                if(valid&&(request.field<8||(request.field>=11&&request.field<=13)))_sceneDocument.dirty=true;
                _editorAssetStatus=valid?"Rendering updated":"Invalid rendering value";continue;
            }
            if(request.command==EditorCommand::SetLight || request.command==EditorCommand::SetMaterial) {
                bool done=false;
                if(_scene.valid(request.target)&&std::isfinite(request.value)) {
                    const float v=request.value;
                    if(request.command==EditorCommand::SetLight) {
                        if(const auto* current=_scene.read<Light>(request.target)) {
                            auto light=*current;
                            switch(request.field) {
                            case 0:light.type=v>0.5f?LightType::Spot:LightType::Point;break;
                            case 1:light.enabled=v>0.5f;break;
                            case 2:light.intensity=v;break;case 3:light.range=v;break;
                            case 4:light.innerCone=glm::radians(v);break;case 5:light.outerCone=glm::radians(v);break;
                            case 6:light.castShadows=v>0.5f;break;
                            case 7:case 8:case 9:light.color[request.field-7]=v;break;
                            default:continue;
                            }
                            done=_scene.set(request.target,light);
                        }
                    } else if(_scene.read<SceneMeshAsset>(request.target)->assetKind!=SceneAssetKind::ImportedGLTF) {
                        auto material=*_scene.read<SceneMaterial>(request.target);
                        switch(request.field){
                        case 0:material.enabled=v>0.5f;break;case 1:material.doubleSided=v>0.5f;break;case 2:material.celShading=v>0.5f;break;
                        case 3:material.metallic=v;break;case 4:material.roughness=v;break;case 5:material.specular=v;break;
                        case 6:material.subsurface=v;break;case 7:material.transmission=v;break;case 8:material.ior=v;break;
                        case 9:material.thickness=v;break;case 10:material.occlusionStrength=v;break;
                        default:continue;
                        }
                        done=_scene.set(request.target,material);
                    }
                }
                if(done){_sceneDocument.dirty=true;_ssgi.historyValid=false;}
                _editorAssetStatus=done?"Property updated":"Property rejected";continue;
            }
            if (request.command == EditorCommand::Save) { save_from_editor(); continue; }
            if (request.command == EditorCommand::CreateCube) { create_editor_actor("Cube", SceneAssetKind::UnitCube, false); continue; }
            if (request.command == EditorCommand::Delete || request.command == EditorCommand::Duplicate) {
                if (!_scene.valid(request.target)) { _editorAssetStatus = "Selection is no longer valid"; continue; }
                _selectedSceneObject = request.target.persistentId;
                const bool done = request.command == EditorCommand::Delete ? delete_selected_scene_object() : duplicate_selected_scene_object();
                _editorAssetStatus = done ? "Scene updated" : _scene.last_error(); continue;
            }
            if (request.command == EditorCommand::SetName || request.command == EditorCommand::SetTransform) {
                bool done = false;
                if (_scene.valid(request.target)) {
                    if (request.command == EditorCommand::SetName) done = _scene.set(request.target, Name{request.text});
                    else if (request.field >= 0 && request.field < 9) {
                        auto transform = *_scene.read<Transform>(request.target);
                        if (request.field < 3) transform.position[request.field] = request.value;
                        else if (request.field < 6) transform.rotation[request.field - 3] = glm::radians(request.value);
                        else transform.scale[request.field - 6] = request.value;
                        done = _scene.set_local_transform(request.target, transform);
                    }
                }
                if (done) _sceneDocument.dirty = true;
                _editorAssetStatus = done ? "Property updated" : "Property rejected: " + _scene.last_error(); continue;
            }
            const auto root = editor_asset_root();
            const auto path = std::filesystem::weakly_canonical(request.path);
            const auto relative = path.lexically_relative(root);
            if (relative.empty() || *relative.begin() == ".." || !std::filesystem::is_regular_file(path)) {
                _editorAssetStatus = "Choose an existing file inside the asset root"; continue;
            }
            auto extension = path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            const auto stored = std::filesystem::relative(path, std::filesystem::current_path()).generic_string();
            if (request.command == EditorCommand::OpenScene) {
                const char* scratch = SDL_getenv("MIRABILIS_TEST_SCENE_DIRECTORY");
                const auto sceneRoot = std::filesystem::weakly_canonical(scratch && *scratch ? scratch : "../../assets/scenes");
                if (extension != ".json" || path.parent_path() != sceneRoot) {
                    _editorAssetStatus = "Select a scene JSON in the scene directory"; continue;
                }
                if (_sceneDocument.dirty && !request.discard) { _editorAssetStatus = "Save or explicitly discard unsaved edits first"; continue; }
                _editorAssetStatus = load_editor_scene_named(path.filename().string()) ? "Scene opened" : "Scene load failed; current scene retained";
            } else if (request.command == EditorCommand::ImportModel) {
                if (extension != ".glb" && extension != ".gltf") { _editorAssetStatus = "Select a glTF or GLB model"; continue; }
                _editorAssetStatus = import_gltf_actor(stored) != InvalidSceneObject ? "Model imported" : "Model import failed";
            } else if (request.command == EditorCommand::AssignBaseColor) {
                if (!_scene.valid(request.target) || _scene.pending(request.target)) { _editorAssetStatus = "Select an editable entity first"; continue; }
                _failedSceneTextures.erase(stored);
                const auto& image = load_scene_texture(stored);
                if (image.image == _errorCheckerboardImage.image || _editorQuitRequested) { _editorAssetStatus = "Texture could not be decoded; material retained"; continue; }
                auto material = *_scene.read<SceneMaterial>(request.target);
                material.enabled = true; material.baseColorTexturePath = stored;
                if (_scene.set(request.target, material)) { _sceneDocument.dirty = true; _editorAssetStatus = "Base color assigned"; }
                else _editorAssetStatus = "Assignment rejected: " + _scene.last_error();
            } else if (request.command == EditorCommand::ReimportModel) {
                if (extension != ".glb" && extension != ".gltf") { _editorAssetStatus = "Reimport currently supports models"; continue; }
                auto replacement = loadGltf(this, path);
                if (!replacement || (*replacement)->hasMissingTextures || _editorQuitRequested) { _editorAssetStatus = "Reimport failed; previous model retained"; continue; }
                VK_CHECK(vkDeviceWaitIdle(_device)); size_t replaced = 0;
                for (const auto ref : _scene.hierarchy_order()) {
                    const auto* mesh = _scene.read<SceneMeshAsset>(ref);
                    if (mesh->assetKind == SceneAssetKind::ImportedGLTF && std::filesystem::weakly_canonical(mesh->modelPath) == path) {
                        _sceneRenderRuntimes[ref.persistentId].model = *replacement;
                        _previousSceneModelTransforms.erase(ref.persistentId); ++replaced;
                    }
                }
                _sceneSdf.rebuildRequested = _surfaceCache.rebuildRequested = true;
                _traceDrawHash = _sunCasters.hash = 0;
                _editorAssetStatus = "Reimported model for " + std::to_string(replaced) + " entities";
            }
        } catch (const std::exception& error) { _editorAssetStatus = error.what(); }
    }
}
