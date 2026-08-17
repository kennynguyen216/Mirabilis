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
#include <thread>
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
#include <vk_pipelines.h>
#include <glm/gtx/transform.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/vec2.hpp>
#include <algorithm>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"

constexpr bool bUseValidationLayers = false;

VulkanEngine* loadedEngine = nullptr;

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

    rebuild_active_wall_colliders();
    _playerMovement.resolve_world_collision(_activeWallColliders);
    _playerInput.jumpPressed = false;
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

void VulkanEngine::rebuild_active_wall_colliders()
{
    _activeWallColliders.clear();
    _activeWallColliders.reserve(_levelWallColliders.size() + 12);

    for (size_t wallIndex = 0; wallIndex < _levelWallColliders.size(); ++wallIndex) {
        const AABB& wall = _levelWallColliders[wallIndex];
        std::vector<AABB> wallPieces{wall};
        if (_bluePortal.placed &&
            _bluePortal.hostWallIndex == static_cast<int>(wallIndex)) {
            carve_portal_opening(wallPieces, _bluePortal);
        }
        if (_orangePortal.placed &&
            _orangePortal.hostWallIndex == static_cast<int>(wallIndex)) {
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
    const AABB* closestWall = nullptr;
    int closestWallIndex = -1;
    for (size_t wallIndex = 0; wallIndex < _levelWallColliders.size(); ++wallIndex) {
        const AABB& wall = _levelWallColliders[wallIndex];
        const std::optional<RaycastHit> hit = raycast_aabb(
            rayOrigin, rayDirection, wall);
        if (hit.has_value() &&
            (!closestHit.has_value() || hit->distance < closestHit->distance)) {
            closestHit = hit;
            closestWall = &wall;
            closestWallIndex = static_cast<int>(wallIndex);
        }
    }

    if (!closestHit.has_value() || closestWall == nullptr) {
        return;
    }

    constexpr float PortalSurfaceOffset = 0.01f;
    Portal candidate = portal;
    candidate.placed = true;
    candidate.hostWallIndex = closestWallIndex;
    candidate.position = closestHit->position +
        closestHit->normal * PortalSurfaceOffset;
    orient_portal(candidate, closestHit->normal);

    if (!snap_portal_to_wall(candidate, *closestWall)) {
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
	stats.mesh_draw_time = 0.0f;
	draw_geometry(
        cmd,
        mainDrawContext,
        sceneData.viewproj,
        get_current_frame().sceneDescriptor,
        true);
    // The portal view remains live all the way to the crossing plane.  Hiding
    // it for a frame-rate-sized safety band exposed the solid host wall before
    // physics teleported the player, causing the black flash.
    draw_portal_masks(cmd);
    draw_portal_views(cmd);

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
                if(e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                    bQuit = true;
                }

                if (e.type == SDL_KEYDOWN &&
                    e.key.keysym.sym == SDLK_TAB &&
                    e.key.repeat == 0) {
                    set_mouse_capture(!_mouseCaptured);
                }

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
                    // SDL reports a normal scroll-down as a negative Y value.
                    // Some touchpads request the opposite convention through
                    // SDL_MOUSEWHEEL_FLIPPED, so normalize it before testing.
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

                // Send every SDL event to ImGui as well.
                ImGui_ImplSDL2_ProcessEvent(&e);
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

        _physicsAccumulator = std::min(
            _physicsAccumulator + deltaTime,
            PhysicsDt * static_cast<float>(MaxPhysicsSteps));
        _playerInput.yaw = mainCamera.yaw;
        while (_physicsAccumulator >= PhysicsDt) {
            update_physics(PhysicsDt);
            _physicsAccumulator -= PhysicsDt;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        if (ImGui::Begin("background")) {
            if (ImGui::Button(
                    _mouseCaptured ? "Release Mouse (Tab)" : "Capture Mouse (Tab)")) {
                set_mouse_capture(!_mouseCaptured);
            }
            ImGui::SliderFloat("Render Scale", &renderScale, 0.3f, 1.0f);
            if (!backgroundEffects.empty()) {
                ComputeEffect& selected = backgroundEffects[currentBackgroundEffect];

                ImGui::Text("Selected effect: %s", selected.name);
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

        if (ImGui::Begin("Movement")) {
            PlayerMovementSettings& movement = _playerMovement.settings;
            ImGui::SliderFloat("Gravity", &movement.gravity, 1.0f, 60.0f);
            ImGui::SliderFloat("Jump Speed", &movement.jumpSpeed, 1.0f, 20.0f);
            ImGui::SliderFloat("Ground Speed", &movement.maxGroundSpeed, 1.0f, 20.0f);
            ImGui::SliderFloat("Ground Accel", &movement.groundAcceleration, 1.0f, 100.0f);
            ImGui::SliderFloat("Ground Friction", &movement.groundFriction, 0.0f, 20.0f);
            ImGui::SliderFloat("Air Accel", &movement.airAcceleration, 0.0f, 50.0f);
            ImGui::SliderFloat("Air Wish Cap", &movement.airWishSpeedCap, 0.1f, 20.0f);
            ImGui::SliderFloat("Jump Buffer", &movement.jumpBufferSeconds, 0.0f, 0.25f);

            ImGui::SeparatorText("Portal Player Model");
            ImGui::SliderFloat("Model Scale", &_playerModelScale, 0.01f, 5.0f);
            ImGui::SliderFloat("Model Y Offset", &_playerModelYOffset, -2.0f, 2.0f);
            ImGui::SliderAngle("Model Facing Offset", &_playerModelYaw);
        }
        ImGui::End();

        stats.frametime = deltaTime * 1000.0f;
        if (ImGui::Begin("Stats")) {
            const float horizontalSpeed = glm::length(glm::vec2(
                _playerMovement.velocity.x,
                _playerMovement.velocity.z));
            ImGui::Text("speed %.2f", horizontalSpeed);
            ImGui::Text("frametime %.3f ms", stats.frametime);
            ImGui::Text("scene update %.3f ms", stats.scene_update_time);
            ImGui::Text("mesh draw %.3f ms", stats.mesh_draw_time);
            ImGui::Text("triangles %d", stats.triangle_count);
            ImGui::Text("draw calls %d", stats.drawcall_count);
        }
        ImGui::End();

        if (_mouseCaptured) {
            const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImDrawList* crosshair = ImGui::GetForegroundDrawList();
            crosshair->AddLine(
                ImVec2(center.x - 7.0f, center.y),
                ImVec2(center.x + 7.0f, center.y),
                IM_COL32(255, 255, 255, 255),
                2.0f);
            crosshair->AddLine(
                ImVec2(center.x, center.y - 7.0f),
                ImVec2(center.x, center.y + 7.0f),
                IM_COL32(255, 255, 255, 255),
                2.0f);
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

    draw_portal_sky(cmd, BluePrimaryStencil);
    draw_geometry(
        cmd,
        portalViewDrawContext,
        _portalSceneData[BluePortalView].viewproj,
        frame.portalSceneDescriptors[BluePortalView],
        false,
        &metalRoughMaterial.portalViewPipeline,
        BluePrimaryStencil,
        false);
    draw_recursive_portal_mask(
        cmd,
        _bluePortal,
        _bluePortalMaterial,
        frame.portalSceneDescriptors[BluePortalView],
        BluePrimaryStencil,
        BlueRecursiveStencil,
        BlueRecursiveBit);
    draw_portal_sky(cmd, BlueRecursiveStencil, BlueRecursiveStencil);
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

    draw_portal_sky(cmd, OrangePrimaryStencil);
    draw_geometry(
        cmd,
        portalViewDrawContext,
        _portalSceneData[OrangePortalView].viewproj,
        frame.portalSceneDescriptors[OrangePortalView],
        false,
        &metalRoughMaterial.portalViewPipeline,
        OrangePrimaryStencil,
        false);
    draw_recursive_portal_mask(
        cmd,
        _orangePortal,
        _orangePortalMaterial,
        frame.portalSceneDescriptors[OrangePortalView],
        OrangePrimaryStencil,
        OrangeRecursiveStencil,
        OrangeRecursiveBit);
    draw_portal_sky(cmd, OrangeRecursiveStencil, OrangeRecursiveStencil);
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

void VulkanEngine::draw_portal_sky(
    VkCommandBuffer cmd,
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
    VkShaderModule portalSkyVertexShader = VK_NULL_HANDLE;
    VkShaderModule portalSkyFragmentShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module("../../shaders/mesh.frag.spv", engine->_device, &fragmentShader) ||
        !vkutil::load_shader_module("../../shaders/mesh.vert.spv", engine->_device, &vertexShader) ||
        !vkutil::load_shader_module("../../shaders/portal_mask.vert.spv", engine->_device, &portalMaskVertexShader) ||
        !vkutil::load_shader_module("../../shaders/portal_view.vert.spv", engine->_device, &portalViewVertexShader) ||
        !vkutil::load_shader_module("../../shaders/portal_view.frag.spv", engine->_device, &portalViewFragmentShader) ||
        !vkutil::load_shader_module("../../shaders/portal_sky.vert.spv", engine->_device, &portalSkyVertexShader) ||
        !vkutil::load_shader_module("../../shaders/portal_sky.frag.spv", engine->_device, &portalSkyFragmentShader)) {
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
        if (portalSkyVertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(engine->_device, portalSkyVertexShader, nullptr);
        }
        if (portalSkyFragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(engine->_device, portalSkyFragmentShader, nullptr);
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

    VkPipelineLayoutCreateInfo portalSkyLayoutInfo = vkinit::pipeline_layout_create_info();
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

    vkDestroyShaderModule(engine->_device, fragmentShader, nullptr);
    vkDestroyShaderModule(engine->_device, vertexShader, nullptr);
    vkDestroyShaderModule(engine->_device, portalMaskVertexShader, nullptr);
    vkDestroyShaderModule(engine->_device, portalViewVertexShader, nullptr);
    vkDestroyShaderModule(engine->_device, portalViewFragmentShader, nullptr);
    vkDestroyShaderModule(engine->_device, portalSkyVertexShader, nullptr);
    vkDestroyShaderModule(engine->_device, portalSkyFragmentShader, nullptr);

    engine->_mainDeletionQueue.push_function([this, engine]() {
        vkDestroyPipeline(engine->_device, opaquePipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, transparentPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, portalStencilPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, portalRecursiveStencilPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, portalMaskPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, portalViewPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, engine->_portalSkyPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(engine->_device, opaquePipeline.layout, nullptr);
        vkDestroyPipelineLayout(engine->_device, engine->_portalSkyPipeline.layout, nullptr);
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
    for (size_t i = 0; i < _levelWalls.size(); i++) {
        _levelWallColliders[i] = get_aabb(_levelWalls[i]);
    }

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
    corners[0].position = glm::vec3(-25.f, 0.f, -25.f);
    corners[0].uv_x = 0.f;
    corners[0].uv_y = 0.f;
    corners[0].normal = glm::vec3(0.f, 1.f, 0.f);
    corners[0].color = glm::vec4(1.f);

    corners[1].position = glm::vec3(25.f, 0.f, -25.f);
    corners[1].uv_x = 1.f;
    corners[1].uv_y = 0.f;
    corners[1].normal = glm::vec3(0.f, 1.f, 0.f);
    corners[1].color = glm::vec4(1.f);

    corners[2].position = glm::vec3(25.f, 0.f, 25.f);
    corners[2].uv_x = 1.f;
    corners[2].uv_y = 1.f;
    corners[2].normal = glm::vec3(0.f, 1.f, 0.f);
    corners[2].color = glm::vec4(1.f);

    corners[3].position = glm::vec3(-25.f, 0.f, 25.f);
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
    _floorBounds.extents = glm::vec3(25.0f, 0.0f, 25.0f);
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

    _mainDeletionQueue.push_function([this]() {
        destroy_buffer(_orangePortalMaterialBuffer);
        destroy_buffer(_bluePortalMaterialBuffer);
        destroy_buffer(_portalMesh.vertexBuffer);
        destroy_buffer(_portalMesh.indexBuffer);
        destroy_buffer(_wallMaterialBuffer);
        destroy_buffer(_wallMesh.vertexBuffer);
        destroy_buffer(_wallMesh.indexBuffer);
        destroy_buffer(_playerMaterialBuffer);
        destroy_buffer(_floorMaterialBuffer);
        destroy_buffer(_floorMesh.vertexBuffer);
        destroy_buffer(_floorMesh.indexBuffer);
        vkDestroySampler(_device, _defaultSamplerNearest, nullptr);
        vkDestroySampler(_device, _defaultSamplerLinear, nullptr);
        destroy_image(_whiteImage);
        destroy_image(_greyImage);
        destroy_image(_blackImage);
        destroy_image(_errorCheckerboardImage);
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

void VulkanEngine::update_scene(float deltaTime)
{
    const auto startTime = std::chrono::steady_clock::now();
    mainCamera.position = _playerMovement.position + glm::vec3(0.0f, 1.7f, 0.0f);

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
    RenderObject floor{};
    floor.indexCount = 6;
    floor.firstIndex = 0;
    floor.indexBuffer = _floorMesh.indexBuffer.buffer;
    floor.material = &_floorMaterial;
    floor.bounds = _floorBounds;
    floor.transform = glm::mat4(1.0f);
    floor.vertexBufferAddress = _floorMesh.vertexBufferAddress;

    worldDrawContext.OpaqueSurfaces.push_back(floor);

    const auto addWall = [&](const Wall& sourceWall) {
        RenderObject wall{};
        wall.indexCount = 36;
        wall.firstIndex = 0;
        wall.indexBuffer = _wallMesh.indexBuffer.buffer;
        wall.material = &_wallMaterial;
        wall.bounds = _wallBounds;
        wall.transform = glm::translate(glm::mat4(1.0f), sourceWall.position) *
            glm::scale(glm::mat4(1.0f), sourceWall.halfExtents * 2.0f);
        wall.vertexBufferAddress = _wallMesh.vertexBufferAddress;
        worldDrawContext.OpaqueSurfaces.push_back(wall);
    };
    for (const Wall& wall : _levelWalls) {
        addWall(wall);
    }

    portalViewDrawContext = worldDrawContext;
    if (_playerModel) {
        const glm::mat4 playerTransform =
            glm::translate(
                glm::mat4(1.0f),
                _playerMovement.position + glm::vec3(0.0f, _playerModelYOffset, 0.0f)) *
            // Camera yaw uses a negative-Y rotation. Apply the matching
            // world-space turn first; the slider is only the imported asset's
            // fixed forward-axis correction.
            glm::rotate(
                glm::mat4(1.0f),
                -mainCamera.yaw + _playerModelYaw,
                glm::vec3(0.0f, 1.0f, 0.0f)) *
            glm::scale(glm::mat4(1.0f), glm::vec3(_playerModelScale));
        _playerModel->Draw(playerTransform, portalViewDrawContext);
    } else {
        // Keep the collision-sized box as a visible fallback if the asset
        // fails to load on another machine.
        RenderObject playerProxy{};
        playerProxy.indexCount = 36;
        playerProxy.firstIndex = 0;
        playerProxy.indexBuffer = _wallMesh.indexBuffer.buffer;
        playerProxy.material = &_playerMaterial;
        playerProxy.bounds = _playerBounds;
        const glm::vec3 playerScale{
            _playerMovement.settings.playerHalfWidth * 2.0f,
            _playerMovement.settings.playerHeight,
            _playerMovement.settings.playerHalfWidth * 2.0f};
        const glm::vec3 playerCenter = _playerMovement.position + glm::vec3(
            0.0f, _playerMovement.settings.playerHeight * 0.5f, 0.0f);
        playerProxy.transform = glm::translate(glm::mat4(1.0f), playerCenter) *
            glm::scale(glm::mat4(1.0f), playerScale);
        playerProxy.vertexBufferAddress = _wallMesh.vertexBufferAddress;
        portalViewDrawContext.OpaqueSurfaces.push_back(playerProxy);
    }

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

    sceneData = build_scene_data(mainCamera.getViewMatrix());

    std::memcpy(
        get_current_frame().sceneBuffer.info.pMappedData,
        &sceneData,
        sizeof(sceneData));

    if (_bluePortal.placed && _orangePortal.placed) {
        const glm::vec3 cameraForward = glm::normalize(glm::vec3(
            mainCamera.getRotationMatrix() * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
        const glm::vec3 cameraUp = glm::normalize(glm::vec3(
            mainCamera.getRotationMatrix() * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)));

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
                glm::vec3(transfer * glm::vec4(mainCamera.position, 1.0f)));
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

	// this initializes imgui for SDL
	ImGui_ImplSDL2_InitForVulkan(_window);

	// this initializes imgui for Vulkan
	ImGui_ImplVulkan_InitInfo init_info = {};
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
	init_info.PipelineRenderingCreateInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &_swapchainImageFormat;

	init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

	ImGui_ImplVulkan_Init(&init_info);

	ImGui_ImplVulkan_CreateFontsTexture();

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
