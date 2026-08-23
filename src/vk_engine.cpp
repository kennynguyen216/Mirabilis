#include "vk_engine.h"

#include <VkBootstrap.h>
#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_initializers.h>
#include <vk_images.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

#include <vk_pipelines.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec2.hpp>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"
#include "ImGuizmo.h"

#ifdef NDEBUG
constexpr bool bUseValidationLayers = false;
#else
constexpr bool bUseValidationLayers = true;
#endif

VulkanEngine* loadedEngine = nullptr;

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

    apply_scene_spawn_point();

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

void VulkanEngine::set_editor_mode(bool enabled)
{
    if (_editorMode == enabled) {
        return;
    }

    _editorMode = enabled;
    _editorCameraLooking = false;
    _physicsAccumulator = 0.0f;
    _playerInsideStartTrigger = false;
    _playerInsideFinishTrigger = false;

    if (enabled) {
        // Start where the player was looking, then let the editor camera move
        // independently.  This feels much less disorienting than spawning a
        // second camera at an arbitrary point in the level.
        _editorCamera = mainCamera;
        _editorCamera.velocity = glm::vec3(0.0f);
        _editorCamera.moveSpeed = 8.0f;
        set_mouse_capture(false);
    } else {
        _editorCamera.velocity = glm::vec3(0.0f);
        apply_scene_spawn_point();
        _playerMovement.position = _playerMovement.settings.spawnPosition;
        _playerMovement.velocity = glm::vec3(0.0f);
        _playerMovement.grounded = false;
        reset_time_trial();
        set_mouse_capture(true);
    }
}

const Camera& VulkanEngine::render_camera() const
{
    return _editorMode ? _editorCamera : mainCamera;
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

        // The explicit File > Save Scene command is still useful for named
        // checkpoints, but closing the editor should not discard unsaved
        // level construction work.
        if (_sceneDirty) {
            save_editor_scene();
        }

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
        // Scene objects can hold the last reference to a loaded glTF, so
        // they must release it while the device is still alive.
        _scene.objects.clear();
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
	stats.world_drawcall_count = 0;
	stats.portal_drawcall_count = 0;
	stats.mesh_draw_time = 0.0f;
	draw_geometry(
        cmd,
        mainDrawContext,
        sceneData.viewproj,
        get_current_frame().sceneDescriptor,
        true);
	stats.world_drawcall_count = stats.drawcall_count;
    // The portal view remains live all the way to the crossing plane.  Hiding
    // it for a frame-rate-sized safety band exposed the solid host wall before
    // physics teleported the player, causing the black flash.
    draw_portal_masks(cmd);
    if (_useOffscreenPortalCameras) {
        draw_offscreen_portal_views(cmd);
    } else {
        draw_portal_views(cmd);
    }
	stats.portal_drawcall_count = stats.drawcall_count - stats.world_drawcall_count;

    // Collider bounds are an editor-only overlay.  They are intentionally
    // drawn after portal composition, so they never affect playable portal
    // views or the saved scene itself.
    if (_editorMode && _showColliderBounds) {
        draw_collider_debug_bounds(cmd);
    }

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
        while (SDL_PollEvent(&e) != 0) {
            bQuit = process_event(e) || bQuit;
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

        if (_editorMode) {
            _editorCamera.update(deltaTime);
            _physicsAccumulator = 0.0f;
        } else {
            _physicsAccumulator = std::min(
                _physicsAccumulator + deltaTime,
                PhysicsDt * static_cast<float>(MaxPhysicsSteps));
            _playerInput.yaw = mainCamera.yaw;
            while (_physicsAccumulator >= PhysicsDt) {
                update_physics(PhysicsDt);
                _physicsAccumulator -= PhysicsDt;
            }
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        draw_frame_ui(deltaTime);

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

// Draws one hierarchy row plus its children.  Selection is the only edit the
// first version of the panel performs.






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
