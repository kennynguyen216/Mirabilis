#include "vk_engine.h"
#include "scene_queries.h"

#include <algorithm>

#include <chrono>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>

void VulkanEngine::sync_scene_driven_objects()
{
    const EntityRef player = _scene.get(_playerObject);
    if (const Transform* current = _scene.read<Transform>(player)) {
        Transform transform = *current;
        transform.position = _playerMovement.position;
        // Camera yaw uses a negative-Y rotation.
        transform.rotation = glm::vec3(0.0f, -mainCamera.yaw, 0.0f);
        _scene.set_local_transform(player, transform, true);
    }

    const auto syncPortalObject = [&](SceneObjectID id, const Portal& portal) {
        const EntityRef object = _scene.get(id);
        const Transform* current = _scene.read<Transform>(object);
        Visibility* visibility = _scene.edit<Visibility>(object, true);
        if (current == nullptr || visibility == nullptr) return;
        visibility->visible = portal.placed;
        if (!portal.placed) return;
        Transform transform = *current;
        transform.position = portal.position;
        // Placed portals are upright, so their frame reduces to yaw.
        transform.rotation = glm::vec3(
            0.0f, std::atan2(portal.normal.x, portal.normal.z), 0.0f);
        transform.scale = glm::vec3(
            portal.halfWidth * 2.0f, portal.halfHeight * 2.0f, 1.0f);
        _scene.set_local_transform(object, transform, true);
    };
    syncPortalObject(_bluePortalObject, _bluePortal);
    syncPortalObject(_orangePortalObject, _orangePortal);
}

void VulkanEngine::emit_scene_render_objects(
    RenderLayer layer,
    DrawContext& drawContext)
{
    for (const EntityRef object : scene_emission_order(_scene)) {
        const Visibility* visibility = _scene.read<Visibility>(object);
        if (visibility == nullptr || !visibility->visible || visibility->layer != layer) continue;
        const auto runtime = _sceneRenderRuntimes.find(object.persistentId);
        if (runtime == _sceneRenderRuntimes.end()) continue;
        const glm::mat4 world = _physicsWorld.render_world(_scene, object, _physicsInterpolation);
        const bool cacheEligible = contributes_to_static_caches(_scene, object, _editorMode);
        if (runtime->second.model != nullptr) {
            const size_t opaqueBegin = drawContext.OpaqueSurfaces.size();
            const size_t transparentBegin = drawContext.TransparentSurfaces.size();
            runtime->second.model->Draw(world, drawContext);
            auto& previous = _previousSceneModelTransforms[object.persistentId];
            const size_t count = drawContext.OpaqueSurfaces.size() - opaqueBegin +
                drawContext.TransparentSurfaces.size() - transparentBegin;
            const bool historyValid = previous.size() == count;
            previous.resize(count);
            size_t index = 0;
            const auto updateDraw = [&](RenderObject& draw) {
                draw.contributesToStaticCaches = cacheEligible;
                draw.previousTransform = historyValid ? previous[index] : draw.transform;
                previous[index++] = draw.transform;
            };
            for (size_t i = opaqueBegin; i < drawContext.OpaqueSurfaces.size(); ++i) {
                updateDraw(drawContext.OpaqueSurfaces[i]);
            }
            for (size_t i = transparentBegin; i < drawContext.TransparentSurfaces.size(); ++i) {
                updateDraw(drawContext.TransparentSurfaces[i]);
            }
            continue;
        }
        const MeshPrimitive& primitive = runtime->second.primitive;
        if (!primitive.valid()) continue;
        const SceneMaterial* material = _scene.read<SceneMaterial>(object);
        RenderObject renderObject{};
        renderObject.indexCount = primitive.indexCount;
        renderObject.firstIndex = primitive.firstIndex;
        renderObject.indexBuffer = primitive.indexBuffer;
        renderObject.material = material != nullptr && material->enabled
            ? resolve_scene_material(object) : primitive.material;
        renderObject.bounds = primitive.bounds;
        renderObject.transform = world;
        if (const auto previous = _previousSceneObjectTransforms.find(object.persistentId);
            previous != _previousSceneObjectTransforms.end()) {
            renderObject.previousTransform = previous->second;
        } else {
            renderObject.previousTransform = world;
        }
        _previousSceneObjectTransforms[object.persistentId] = world;
        renderObject.vertexBufferAddress = primitive.vertexBufferAddress;
        renderObject.contributesToStaticCaches = cacheEligible;
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
        sky.data.environmentTint = _traceSettings.lighting.environmentTint;
        sky.data.data2 = glm::vec4(
            glm::normalize(glm::vec3(rotation * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f))),
            static_cast<float>(_drawExtent.width));
        sky.data.data3 = glm::vec4(
            glm::normalize(glm::vec3(rotation * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f))),
            static_cast<float>(_drawExtent.height));
        sky.data.data4 = glm::vec4(
            glm::normalize(glm::vec3(rotation * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f))),
            0.0f);
        // Background parity (R4.9): the environment source every other path
        // selects, with the same intensity and black policy.
        sky.data.data1 = glm::vec4(
            _ssgi.traceEnvironmentMap ? 1.0f : 0.0f,
            _traceSettings.lighting.environment.x,
            _traceSettings.lighting.environment.y,
            0.0f);
    }

    mainDrawContext.OpaqueSurfaces.clear();
    mainDrawContext.TransparentSurfaces.clear();
    worldDrawContext.OpaqueSurfaces.clear();
    worldDrawContext.TransparentSurfaces.clear();
    portalViewDrawContext.OpaqueSurfaces.clear();
    portalViewDrawContext.TransparentSurfaces.clear();

    std::vector<std::string> loadedSceneNames;
    loadedSceneNames.reserve(loadedScenes.size());
    for (const auto& [name, scene] : loadedScenes) loadedSceneNames.push_back(name);
    std::sort(loadedSceneNames.begin(), loadedSceneNames.end());
    for (const std::string& name : loadedSceneNames) {
        const auto& scene = loadedScenes.at(name);
        if (scene != nullptr) scene->Draw(glm::mat4(1.0f), worldDrawContext);
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

    // The shadowed box follows the active camera, so this has to be settled
    // before build_scene_data() copies it into any camera's buffer.
    _shadow.sunViewProjection = compute_sun_view_projection(camera.position);

    sceneData = build_scene_data(camera.getViewMatrix());
    sceneData.previousViewProjection = _previousMainViewProjectionValid
        ? _previousMainViewProjection
        : sceneData.viewproj;
    _previousMainViewProjection = sceneData.viewproj;
    _previousMainViewProjectionValid = true;

    std::memcpy(
        get_current_frame().sceneBuffer.info.pMappedData,
        &sceneData,
        sizeof(sceneData));

    // Each linked surface receives its own virtual camera and stencil value.
    // The first two slots remain reserved for the portal gun; subsequent
    // slots are editor-authored links stored with the scene.
    {
        const glm::vec3 cameraForward = glm::normalize(glm::vec3(
            camera.getRotationMatrix() * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
        const glm::vec3 cameraUp = glm::normalize(glm::vec3(
            camera.getRotationMatrix() * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)));

        const auto updatePortalView = [&](const Portal& source,
                                          const Portal& destination,
                                          uint32_t surfaceIndex) {
            // Freestanding portal actors are transparent from both sides. If
            // the camera is behind this endpoint, reverse both frames exactly
            // as traversal does. This keeps the linked image continuous while
            // walking around the pole instead of popping at the portal plane.
            Portal viewSource = source;
            Portal viewDestination = destination;
            if (portal_signed_distance(source, camera.position) < 0.0f) {
                orient_portal(viewSource, -source.normal);
                orient_portal(viewDestination, -destination.normal);
            }
            const glm::mat4 transfer = get_portal_transfer_transform(
                viewSource, viewDestination);
            // At the instant we cross a portal, the mathematically exact
            // virtual camera lies on the exit portal plane.  Keep it a tiny
            // distance behind that plane for rendering only so the clip and
            // depth passes never leave a one-frame black aperture.
            glm::vec3 virtualPosition = stabilize_portal_view_camera(
                viewDestination,
                glm::vec3(transfer * glm::vec4(camera.position, 1.0f)));
            glm::vec3 virtualForward = glm::normalize(glm::vec3(
                transfer * glm::vec4(cameraForward, 0.0f)));
            glm::vec3 virtualUp = glm::normalize(glm::vec3(
                transfer * glm::vec4(cameraUp, 0.0f)));

            for (uint32_t level = 0; level < PortalRecursionDepth; ++level) {
                const uint32_t viewIndex = surfaceIndex * PortalRecursionDepth + level;
                _portalRender.sceneData[viewIndex] = build_portal_scene_data(
                    glm::lookAt(virtualPosition, virtualPosition + virtualForward, virtualUp),
                    viewDestination);
                std::memcpy(
                    get_current_frame().portalSceneBuffers[viewIndex].info.pMappedData,
                    &_portalRender.sceneData[viewIndex], sizeof(GPUSceneData));
                virtualPosition = stabilize_portal_view_camera(viewDestination,
                    glm::vec3(transfer * glm::vec4(virtualPosition, 1.0f)));
                virtualForward = glm::normalize(glm::vec3(
                    transfer * glm::vec4(virtualForward, 0.0f)));
                virtualUp = glm::normalize(glm::vec3(
                    transfer * glm::vec4(virtualUp, 0.0f)));
            }

        };

        if (_bluePortal.placed && _orangePortal.placed) {
            updatePortalView(_bluePortal, _orangePortal, BluePortalView);
            updatePortalView(_orangePortal, _bluePortal, OrangePortalView);
        }
        uint32_t viewIndex = 2;
        for (const AuthoredPortalPair& pair : _authoredPortals.pairs) {
            updatePortalView(pair.first, pair.second, viewIndex++);
            updatePortalView(pair.second, pair.first, viewIndex++);
        }
    }

    stats.scene_update_time = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - startTime).count();
}
