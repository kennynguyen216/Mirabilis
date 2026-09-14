#include "vk_engine.h"

#include <VkBootstrap.h>
#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_initializers.h>
#include <vk_images.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
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

namespace {

// Interpolates between two yaw angles along the shorter arc.  Yaw is
// accumulated from mouse motion and is never wrapped, so the raw difference
// between two samples can be several turns' worth even when the player barely
// moved; folding it into (-pi, pi] first keeps the step small and in the
// direction they actually turned.
float lerp_yaw(float from, float to, float t)
{
    constexpr float Pi = 3.14159265358979323846f;
    constexpr float TwoPi = 2.0f * Pi;
    float delta = std::fmod(to - from + Pi, TwoPi);
    if (delta < 0.0f) {
        delta += TwoPi;
    }
    return from + (delta - Pi) * t;
}

} // namespace

VulkanEngine* loadedEngine = nullptr;

VulkanEngine& VulkanEngine::Get() {return *loadedEngine;}

void VulkanEngine::apply_max_fidelity_settings()
{
    renderScale = 1.0f;
    _shadow.enabled = true;
    _shadow.showBounds = false;
    _shadow.depthBias = 0.0012f;
    _shadow.normalBias = 0.15f;
    _shadow.filterRadius = 6.0f;

    // SSAO is deliberately not run beside SSGI: its ambient contribution is
    // bypassed by the SSGI composite, so enabling it would spend GPU time
    // without changing the final image. Keep its best sampling preset ready
    // for comparison if SSGI is later disabled.
    _ssao.settings.enabled = false;
    _ssao.quality = static_cast<int>(SSAOKernelSizes.size()) - 1;
    _ssao.depthFalloff = 24.0f;
    _ssao.normalFalloff = 24.0f;
    _ssao.ambientOnly = false;

    _postProcess.fxaaEnabled = true;
    _postProcess.fxaaEdgeThreshold = 0.063f;
    _postProcess.fxaaSubpixelStrength = 0.25f;
    _postProcess.fxaaShowEdges = false;

    _prepass.enabled = true;
    _ssgi.enabled = true;
    apply_ssgi_quality_preset(4);
    _ssgi.intensity = 0.35f;
    _debugViews.view = RenderDebugView::None;
}

void VulkanEngine::init() 
{
    //only one engine init is allowed with the application
    assert(loadedEngine == nullptr);
    loadedEngine = this;

    // initializedSDL and create a window with it

    SDL_Init(SDL_INIT_VIDEO);
    // Bounded test runs use the project defaults, so a machine where someone
    // switched occlusion off in Render Settings cannot silently skip the pass
    // under the validation harness.
    if (SDL_getenv("MIRABILIS_TEST_FRAMES")) {
        fmt::print("GI test: per-user ambient occlusion preferences ignored\n");
    } else {
        load_ao_preferences();
    }

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
    // The scene descriptor set holds the shadow map, so the image and its
    // comparison sampler have to exist before those sets are written.
    init_shadow_resources();
    // Sized from the draw image, and referenced by a descriptor set that
    // init_descriptors() writes, so both have to exist by this point.
    init_depth_normal_resources();
    init_ssgi_resources();
    // Same reason: the anti-aliasing pass needs its sampler before the set
    // that pairs it with the draw image can be written.
    init_post_process_resources();
    // And again: the occlusion images and their sampler are named by the
    // scene descriptor set, which init_descriptors() writes.
    init_ssao_resources();
    init_gpu_timestamps();
    init_descriptors();
    init_pipelines();
    init_default_data();
    init_imgui();
    init_path_trace();

    // Automation and capture runs can open directly on one intermediate
    // buffer without synthesizing editor UI input. Values match
    // RenderDebugView; ordinary launches remain on final lighting.
    if (const char* debugView = SDL_getenv("MIRABILIS_RENDER_DEBUG_VIEW")) {
        const int value = std::clamp(std::atoi(debugView),
            static_cast<int>(RenderDebugView::None),
            static_cast<int>(RenderDebugView::SSGIReferenceDifference));
        _debugViews.view = static_cast<RenderDebugView>(value);
    }
    if (const char* preset = SDL_getenv("MIRABILIS_SSGI_PRESET")) {
        apply_ssgi_quality_preset(std::atoi(preset));
    }
    if (SDL_getenv("MIRABILIS_MAX_FIDELITY")) {
        apply_max_fidelity_settings();
    }

    apply_scene_spawn_point();

    mainCamera.velocity = glm::vec3(0.0f);
    //mainCamera.position = glm::vec3(30.0f, 0.0f, -85.0f);
    //mainCamera.pitch = 0.0f;
    mainCamera.position = glm::vec3(0.0f, 5.0f, 12.0f);
    mainCamera.pitch = glm::radians(-20.0f);
    mainCamera.yaw = 0.0f;
    _previousPlayerYaw = mainCamera.yaw;
    _targetPlayerYaw = mainCamera.yaw;

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
    if (enabled) {
        _noClip.enabled = false;
        _noClip.up = false;
        _noClip.down = false;
    }
    _editorCameraLooking = false;
    _physicsAccumulator = 0.0f;
    _timeTrial.playerInsideStartTrigger = false;
    _timeTrial.playerInsideFinishTrigger = false;

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
        // Resume the player where gameplay paused. Scene loading and F1 own
        // respawning; opening the settings must not teleport the player.
        _playerMovement.previousPosition = _playerMovement.position;
        _playerMovement.velocity = glm::vec3(0.0f);
        _playerMovement.jumpBufferRemaining = 0.0f;
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
    // The occlusion passes declare their output image without a format
    // qualifier, so that the engine can pick the occlusion format from what
    // the device advertises instead of baking one choice into the shader.
    features10.shaderStorageImageWriteWithoutFormat = VK_TRUE;

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

    // Anisotropic filtering is requested rather than required: it is a large
    // quality win on Sponza's oblique floors and arches, but it is an optional
    // Vulkan feature and a device that lacks it should still run with plain
    // trilinear filtering instead of failing selection.  The samplers read
    // material_anisotropy(), which collapses to 1.0 when this does not take.
    VkPhysicalDeviceFeatures anisotropyFeature{};
    anisotropyFeature.samplerAnisotropy = VK_TRUE;
    _samplerAnisotropySupported =
        physicalDevice.enable_features_if_present(anisotropyFeature);

    // create the vulkan device now

    vkb::DeviceBuilder deviceBuilder {physicalDevice};
    vkb::Device vkbDevice = deviceBuilder.build().value();

    // get the VkDevice handle used in the rest of the vulan apllication 
    _device = vkbDevice.device;
    _chosenGPU = physicalDevice.physical_device;
    _graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    _graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

    // maxSamplerAnisotropy is only meaningful once the feature is on; a device
    // that reports 16 while the feature is off still rejects any sampler that
    // asks for more than 1.
    VkPhysicalDeviceProperties deviceProperties{};
    vkGetPhysicalDeviceProperties(_chosenGPU, &deviceProperties);
    _maxSamplerAnisotropy = _samplerAnisotropySupported
        ? deviceProperties.limits.maxSamplerAnisotropy
        : 1.0f;
    if (_samplerAnisotropySupported) {
        fmt::print(
            "GPU: {} (anisotropy up to {}x)\n",
            deviceProperties.deviceName,
            _maxSamplerAnisotropy);
    } else {
        fmt::print(
            "GPU: {} (anisotropy unsupported, using trilinear)\n",
            deviceProperties.deviceName);
    }

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
	}
    VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_immFence));
    _mainDeletionQueue.push_function([=]() {vkDestroyFence(_device, _immFence, nullptr); });
}

void VulkanEngine::cleanup()
{
    if(_isInitialized) {
        vkDeviceWaitIdle(_device);
        destroy_path_trace();

        // The explicit File > Save Scene command is still useful for named
        // checkpoints, but closing the editor should not discard unsaved
        // level construction work.
        if (_sceneDirty && !SDL_getenv("MIRABILIS_TEST_FRAMES")) {
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
    // The fence has passed, so any timestamps this slot recorded belong to a
    // submission that has finished and can be read without stalling.
    read_gpu_timestamps(_frameNumber % FRAME_OVERLAP);
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
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        resize_requested = true;
        return;
    }
    // SUBOPTIMAL still acquires an image and signals the semaphore. Consume
    // it in this submission before rebuilding; returning would reuse a
    // signaled acquire semaphore on the next frame.
    if (acquireResult == VK_SUBOPTIMAL_KHR) resize_requested = true;
    else VK_CHECK(acquireResult);
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

	// Timestamps have to be reset before they are written again, and this
	// slot's results were read above.
	if (_gpuTiming.supported) {
		vkCmdResetQueryPool(
			cmd,
			_gpuTiming.timestampPool,
			(_frameNumber % FRAME_OVERLAP) * TimestampsPerFrame,
			TimestampsPerFrame);
		vkCmdResetQueryPool(
			cmd,
			_ssgi.timestampPool,
			(_frameNumber % FRAME_OVERLAP) * SSGITimestampsPerFrame,
			SSGITimestampsPerFrame);
		_ssgi.timingWritten[_frameNumber % FRAME_OVERLAP] = false;
	}

	// The shadow and prepass counters are recorded before the main pass
	// resets its own, so they are cleared here instead.
	stats.shadow_drawcall_count = 0;
	stats.shadow_triangle_count = 0;
	stats.prepass_drawcall_count = 0;
	stats.prepass_triangle_count = 0;
	stats.prepass_record_time = 0.0f;

    VkImage presentSource = _drawImage.image;
    if (_rendererMode == RendererMode::SoftwarePathTrace && _traceSupported) {
        draw_path_trace(cmd);
    } else {
    _traceWasActive=false;
	// Render the sunlight depth map first: every later pass, main camera and
	// portal cameras alike, samples it while shading.
	draw_shadow_map(cmd);

	// Camera depth and view-space normals for the screen-space passes.  A
	// debug view samples those images, so it also forces the pass to run, and
	// so does ambient occlusion, which is built entirely out of them.
	const bool showRenderDebugView = _debugViews.view != RenderDebugView::None;
	const bool occlusionActive = ssao_active();
	if (_prepass.enabled || showRenderDebugView || occlusionActive ||
        _ssgi.enabled) {
		draw_depth_normal_prepass(cmd);
	}

	// Ambient occlusion reads the prepass and is finished before any shading
	// starts, because every lit surface in the frame samples its result.
	// Skipped entirely when disabled: the images keep the neutral 1.0 they
	// were cleared to, so nothing downstream needs a second code path.
	if (occlusionActive) {
		draw_ssao(cmd);
	} else {
		stats.ssao_width = 0;
		stats.ssao_height = 0;
		stats.ssao_kernel_samples = 0;
		stats.ssao_raw_time = 0.0f;
		stats.ssao_blur_horizontal_time = 0.0f;
		stats.ssao_blur_vertical_time = 0.0f;
		stats.ssao_total_time = 0.0f;
	}

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
	vkutil::transition_image(cmd, _depthImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    vkutil::transition_image(cmd, _sceneTargets.gbufferAlbedo.image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkutil::transition_image(cmd, _sceneTargets.gbufferVelocity.image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkutil::transition_image(cmd, _sceneTargets.directLighting.image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
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
        true,
        nullptr,
        nullptr,
        0,
        true,
        0xff,
        true);
	stats.world_drawcall_count = stats.drawcall_count;
    draw_ssgi_portal_mask(cmd);
    draw_ssgi(cmd);
    draw_ssgi_composite(cmd);
    // The portal view remains live all the way to the crossing plane.  Hiding
    // it for a frame-rate-sized safety band exposed the solid host wall before
    // physics teleported the player, causing the black flash.
    draw_portal_masks(cmd);
    if (_portalCameras.useOffscreen && _authoredPortals.pairs.empty()) {
        draw_offscreen_portal_views(cmd);
    } else {
        draw_portal_views(cmd);
    }
	stats.portal_drawcall_count = stats.drawcall_count - stats.world_drawcall_count;

    // Replaces the shaded image with one of the buffers behind it.  Editor
    // overlays are still drawn on top, so shadow and collider bounds can be
    // read against the depth or normals they were built from.
    if (showRenderDebugView) {
        draw_render_debug(cmd);
    }

    // Collider bounds are an editor-only overlay.  They are intentionally
    // drawn after portal composition, so they never affect playable portal
    // views or the saved scene itself.
    if (_editorMode && (_debugViews.showColliderBounds || _shadow.showBounds)) {
        draw_collider_debug_bounds(cmd);
    }

    // Everything above this point works in unbounded linear radiance.  The
    // tonemap is what turns that into something a display can show, and it has
    // to run before anti-aliasing: FXAA thresholds luma differences against
    // fixed constants, which only describe visible contrast once the values
    // are in display range.
    //
    // A debug view skips both.  Those images carry per-pixel data - normals,
    // depth, velocity - whose whole value is that nothing has reshaped it, and
    // a tone curve would do exactly that.  They present from the draw image on
    // the untouched path below, as they always have.
    const bool tonemapping = _postProcess.tonemapEnabled && !showRenderDebugView &&
        _postProcess.tonemapPipeline.pipeline != VK_NULL_HANDLE;
    if (tonemapping) {
        draw_tonemap(cmd);
        presentSource = _postProcess.tonemapImage.image;
    }

    // Anti-aliasing sees the whole composed frame, so it smooths world
    // silhouettes, portal contents, and the overlay lines in one pass.  ImGui
    // is drawn after the copy, straight into the swapchain, and stays sharp.
    // It reads the tonemap's output, so it is only available when that ran.
    if (tonemapping && _postProcess.fxaaEnabled &&
        _postProcess.fxaaPipeline.pipeline != VK_NULL_HANDLE) {
        draw_fxaa(cmd);
        presentSource = _postProcess.image.image;
    } else if (tonemapping) {
        vkutil::transition_image(
            cmd,
            _postProcess.tonemapImage.image,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    } else {
        vkutil::transition_image(cmd, _drawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }

    }
	// transition the swapchain image into its correct transfer layout
	vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	// execute a copy from whichever image finished the frame into the swapchain
	vkutil::copy_image_to_image(cmd, presentSource, _swapchainImages[swapchainImageIndex], _drawExtent, _swapchainExtent);

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
	// Indexed by image, not by frame: presentation holds this semaphore
	// until the image comes back around, which need not happen within the
	// two frames a per-frame semaphore would allow.
	VkSemaphore renderSemaphore = _swapchainRenderSemaphores[swapchainImageIndex];
	VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		renderSemaphore);

	VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo, &signalInfo, &waitInfo);

	//submit command buffer to the queue and execute it
	VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit, get_current_frame()._renderFence));

    //prepare present
	// this will put the image we just rendered to into the visible window.
	// we want to wait on this image's render semaphore for that, 
	// as its necessary that drawing commands have finished before the image is displayed to the user
    VkPresentInfoKHR presentInfo = {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.pNext = nullptr;
	presentInfo.pSwapchains = &_swapchain;
	presentInfo.swapchainCount = 1;

	presentInfo.pWaitSemaphores = &renderSemaphore;
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
    bool testRestored=false;
    bool testMinimized=false;
    double benchmarkMilliseconds = 0.0;
    double benchmarkSsgiMilliseconds = 0.0;
    uint32_t benchmarkFrames = 0;
    // Opt-in unattended validation; ordinary interactive sessions are unchanged.
    const char* frameLimitText = std::getenv("MIRABILIS_TEST_FRAMES");
    const int frameLimit = frameLimitText ? std::max(1, std::atoi(frameLimitText)) : 0;
    if (frameLimit) {
        if (SDL_getenv("MIRABILIS_TEST_EDITOR_RESUME")) {
            const auto require = [](bool condition, const char* message) {
                if (!condition) {fmt::print("GI TEST FAIL: {}\n", message); std::abort();}
            };
            const auto key = [&](SDL_Keycode code) {
                SDL_Event event{}; event.type = SDL_KEYDOWN;
                event.key.keysym.sym = code;
                process_event(event);
            };
            const std::string originalScene = _activeSceneFilename;
            respawn_player();
            _playerMovement.position.x += 0.4f;
            _playerMovement.previousPosition = _playerMovement.position;
            mainCamera.position = _playerMovement.position + glm::vec3(0, 1.7f, 0);
            const glm::vec3 pausedPosition = _playerMovement.position;
            const Camera pausedCamera = mainCamera;
            for (int cycle = 0; cycle < 3; ++cycle) {
                key(SDLK_TAB);
                require(_editorMode, "Tab did not enter editor");
                require(glm::length(_editorCamera.position - pausedCamera.position) < 1e-6f,
                    "Editor did not start at gameplay camera");
                _editorCamera.position += glm::vec3(2, 1, 0);
                key(SDLK_TAB);
                require(!_editorMode, "Tab did not return to gameplay");
                require(glm::length(_playerMovement.position - pausedPosition) < 1e-6f,
                    "Tab respawned or moved the player");
                require(glm::length(mainCamera.position - pausedCamera.position) < 1e-6f &&
                    mainCamera.pitch == pausedCamera.pitch && mainCamera.yaw == pausedCamera.yaw,
                    "Tab changed the gameplay camera");
            }
            for (int tick = 0; tick < 240; ++tick) update_physics(1.0f / 120.0f);
            require(_playerMovement.grounded &&
                glm::length(_playerMovement.position - pausedPosition) < 1e-4f,
                "Resumed player did not remain supported by the floor");
            key(SDLK_F1);
            require(glm::length(_playerMovement.position - _playerMovement.settings.spawnPosition) < 1e-6f,
                "F1 did not respawn the player");
            key(SDLK_TAB);
            _playerMovement.position = glm::vec3(50, 50, 50);
            require(load_editor_scene_named("gi_cornell_box_dark.json"), "Scene load failed during Tab test");
            key(SDLK_TAB);
            require(glm::length(_playerMovement.position - glm::vec3(0, 0, 2.5f)) < 1e-6f,
                "New scene did not use its authored spawn");
            require(load_editor_scene_named(originalScene), "Could not restore test scene");
            fmt::print("GI editor resume: three Tab cycles, camera, floor support, F1 and new-scene spawn PASS\n");
        }
        set_editor_mode(true);
        set_mouse_capture(false);
        ImGui::GetIO().IniFilename = nullptr;
        if(const char* camera=SDL_getenv("MIRABILIS_TEST_CAMERA")) {
            std::sscanf(camera,"%f %f %f %f %f",&_editorCamera.position.x,&_editorCamera.position.y,&_editorCamera.position.z,&_editorCamera.pitch,&_editorCamera.yaw);
        }
        if (std::getenv("MIRABILIS_TEST_TRACE")) {
            _rendererMode = _traceSupported ? RendererMode::SoftwarePathTrace : RendererMode::Raster;
            if(_traceSupported) validate_path_trace();
            else if(!std::getenv("MIRABILIS_TEST_DISABLE_TRACE")) std::abort();
        }
        if (SDL_getenv("MIRABILIS_TEST_REQUIRE_SSAO")) {
            if (!ssao_active()) {
                fmt::print("GI TEST FAIL: SSAO was required but inactive\n");
                std::abort();
            }
            fmt::print("GI test: ssao=active\n");
        }
    }
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
            if(frameLimit&&std::getenv("MIRABILIS_TEST_CYCLE")&&testMinimized&&!testRestored) {
                SDL_RestoreWindow(_window); testRestored=true;
                fmt::print("GI lifecycle: minimized and restore requested\n");
            }
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
            // Leaving the editor should not make the first tick back in play
            // sweep through however far the editor camera was turned.
            _previousPlayerYaw = mainCamera.yaw;
        } else {
            _physicsAccumulator = std::min(
                _physicsAccumulator + deltaTime,
                PhysicsDt * static_cast<float>(MaxPhysicsSteps));
            // Where this frame's turn ends.  update_physics() may overwrite
            // both ends of it partway through if the player crosses a portal,
            // which is why the loop reads the members rather than a local.
            _targetPlayerYaw = mainCamera.yaw;
            // Clamped to at least one: the accumulator can sit a hair above
            // PhysicsDt while the division truncates to zero, and dividing by
            // that below would hand the first tick an infinite fraction.
            const int stepCount = std::max(
                1, static_cast<int>(_physicsAccumulator / PhysicsDt));
            int stepIndex = 0;
            while (_physicsAccumulator >= PhysicsDt) {
                ++stepIndex;
                _playerInput.yaw = lerp_yaw(
                    _previousPlayerYaw,
                    _targetPlayerYaw,
                    static_cast<float>(stepIndex) /
                        static_cast<float>(stepCount));
                update_physics(PhysicsDt);
                _physicsAccumulator -= PhysicsDt;
            }
            _previousPlayerYaw = _targetPlayerYaw;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        draw_frame_ui(deltaTime);

        ImGui::Render();
        if (frameLimit && std::getenv("MIRABILIS_TEST_CYCLE")) {
            if (_frameNumber == 4) _rendererMode = RendererMode::Raster;
            if (_frameNumber == 8) _rendererMode = RendererMode::SoftwarePathTrace;
            if (_frameNumber == 10) SDL_SetWindowSize(_window, 803, 457);
            if (_frameNumber == 12) renderScale = 0.7f;
            if (_frameNumber >= 14&&!testMinimized) {SDL_MinimizeWindow(_window);testMinimized=true;}
        }
        const bool testInvalidation=frameLimit&&SDL_getenv("MIRABILIS_TEST_INVALIDATION");
        const int testFrame=_frameNumber;
        if(testInvalidation) {
            if(testFrame==2) _traceExposure+=1;
            if(testFrame==3) ++_traceBaseSeed;
            if(testFrame==4) _editorCamera.position.x+=0.1f;
            if(testFrame==5) _traceMaxDepth=1;
            if(testFrame>=6&&testFrame<=8) {
                for(auto& object:_scene.objects) if(object.alive&&object.visible&&object.material.enabled) {
                    if(testFrame==6) object.material.colorTint.r*=0.5f;
                    if(testFrame==7) object.visible=false;
                    if(testFrame==8) object.localTransform.position.x+=0.125f;
                    break;
                }
            }
            if(testFrame==9) renderScale=0.5f;
            if(testFrame==10) _traceWasActive=false;
            if(testFrame==11) _traceLighting.sunRadiance.x+=1;
            if(testFrame==12) _traceLighting.environment.x+=0.25f;
            if(testFrame==13) _traceMaterialModel=1-_traceMaterialModel;
            if(testFrame==14) _tracePortalLimit=1;
            if(testFrame==15) _shadow.sunlightDirection.x+=0.25f;
            if(testFrame==16) {
                for(auto& object:_scene.objects) if(object.alive&&object.visible&&object.material.enabled) {
                    object.material.emissionColor=glm::vec3(1);
                    object.material.emissionStrength+=1; break;
                }
            }
            if(testFrame==17&&!_authoredPortals.pairs.empty()) _authoredPortals.pairs[0].first.position.x+=0.125f;
        }
        draw(deltaTime);
        if (frameLimit && SDL_getenv("MIRABILIS_SSGI_BENCHMARK") &&
            _frameNumber > 5) {
            benchmarkMilliseconds += static_cast<double>(deltaTime) * 1000.0;
            benchmarkSsgiMilliseconds += stats.ssgi_total_time;
            ++benchmarkFrames;
        }
        if(testInvalidation&&testFrame>=2&&testFrame<=17) {
            const uint32_t expected=testFrame==2?3:1;
            fmt::print("GI invalidation frame {}: samples={} expected={}\n",testFrame,_traceSamples,expected);
            if(_traceSamples!=expected) std::abort();
        }
        if (frameLimit && _frameNumber >= frameLimit) {
            if (benchmarkFrames > 0) {
                const VkExtent2D extent = active_ssgi_extent();
                fmt::print(
                    "SSGI benchmark: preset={} extent={}x{} average-frame-ms={:.3f} average-ssgi-gpu-ms={:.3f} samples={}\n",
                    _ssgi.qualityPreset, extent.width, extent.height,
                    benchmarkMilliseconds / benchmarkFrames,
                    benchmarkSsgiMilliseconds / benchmarkFrames,
                    benchmarkFrames);
            }
            if (const char* capture=SDL_getenv("MIRABILIS_CAPTURE")) capture_path_trace(capture);
            if (const char* capture=SDL_getenv("MIRABILIS_SSGI_CAPTURE")) capture_ssgi(capture);
            fmt::print("GI bounded run complete: frames={} renderer={}\n",_frameNumber,
                _rendererMode==RendererMode::SoftwarePathTrace&&_traceSupported?"software":"raster");
            bQuit = true;
        }
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

    // Created here rather than in init_sync_structures() because the count
    // is the swapchain's, and a resize rebuilds them with it.
    VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();
    _swapchainRenderSemaphores.resize(_swapchainImages.size());
    for (VkSemaphore& semaphore : _swapchainRenderSemaphores) {
        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &semaphore));
    }
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
    // The anti-aliasing pass reads the finished image as a texture.
    drawImageUsages |= VK_IMAGE_USAGE_SAMPLED_BIT;

    VkImageCreateInfo rimg_info = vkinit::image_create_info(_drawImage.imageFormat, drawImageUsages, drawImageExtent);

    // for the draw image, we want to allocate it from gpu local memory
    VmaAllocationCreateInfo rimg_allocinfo = {};
    rimg_allocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // allocate and create the image

    VK_CHECK(vmaCreateImage(_allocator, &rimg_info, &rimg_allocinfo, &_drawImage.image, &_drawImage.allocation, nullptr));

    // build a image-view for the draw image to use for rendering

    VkImageViewCreateInfo rview_info = vkinit::imageview_create_info(_drawImage.imageFormat, _drawImage.image, VK_IMAGE_ASPECT_COLOR_BIT);

    VK_CHECK(vkCreateImageView(_device, &rview_info, nullptr, &_drawImage.imageView));

    // Depth/stencil formats are not interchangeable across Vulkan devices.
    // Select one the active GPU explicitly advertises for attachment use
    // rather than assuming D32_SFLOAT_S8_UINT is always available.
    static constexpr std::array<VkFormat, 2> depthFormats{
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT};
    _depthImage.imageFormat = VK_FORMAT_UNDEFINED;
    for (VkFormat candidate : depthFormats) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(_chosenGPU, candidate, &properties);
        if ((properties.optimalTilingFeatures &
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
            _depthImage.imageFormat = candidate;
            break;
        }
    }
    if (_depthImage.imageFormat == VK_FORMAT_UNDEFINED) {
        fmt::print("No supported depth/stencil attachment format was found.\n");
        std::abort();
    }
    // Real portal rendering uses the stencil half of this image to mark each
    // portal opening. Depth still handles normal world visibility.
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

    for (VkSemaphore semaphore : _swapchainRenderSemaphores) {
        vkDestroySemaphore(_device, semaphore, nullptr);
    }
    _swapchainRenderSemaphores.clear();

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
        object.previousTransform = hasPreviousDrawTransform
            ? previousDrawTransform
            : nodeMatrix;
        object.vertexBufferAddress = mesh->meshBuffers.vertexBufferAddress;
        if (object.material != nullptr) {
            if (object.material->passType == MaterialPass::Transparent) {
                ctx.TransparentSurfaces.push_back(object);
            } else {
                ctx.OpaqueSurfaces.push_back(object);
            }
        }
    }
    previousDrawTransform = nodeMatrix;
    hasPreviousDrawTransform = true;
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
