#include "vk_engine.h"

#include <chrono>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>

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
    for (SceneObject& object : _scene.objects) {
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
        renderObject.material = object.material.enabled
            ? resolve_scene_material(object)
            : object.primitive.material;
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
                _portalSceneData[viewIndex] = build_portal_scene_data(
                    glm::lookAt(virtualPosition, virtualPosition + virtualForward, virtualUp),
                    viewDestination);
                std::memcpy(
                    get_current_frame().portalSceneBuffers[viewIndex].info.pMappedData,
                    &_portalSceneData[viewIndex], sizeof(GPUSceneData));
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
        for (const AuthoredPortalPair& pair : _authoredPortalPairs) {
            updatePortalView(pair.first, pair.second, viewIndex++);
            updatePortalView(pair.second, pair.first, viewIndex++);
        }
    }

    stats.scene_update_time = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - startTime).count();
}
