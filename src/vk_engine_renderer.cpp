#include "vk_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/packing.hpp>

#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_loader.h>
#include <vk_pipelines.h>

namespace {

glm::vec3 normalized_sun_direction(const glm::vec3& direction)
{
    if (glm::dot(direction, direction) < 0.000001f) {
        return glm::normalize(glm::vec3(0.0f, 1.0f, 0.5f));
    }
    return glm::normalize(direction);
}

void set_previous_world_rows(
    GPUDrawPushConstants& pushConstants,
    const glm::mat4& previousWorld)
{
    pushConstants.previousWorldRow0 = glm::vec4(
        previousWorld[0][0], previousWorld[1][0],
        previousWorld[2][0], previousWorld[3][0]);
    pushConstants.previousWorldRow1 = glm::vec4(
        previousWorld[0][1], previousWorld[1][1],
        previousWorld[2][1], previousWorld[3][1]);
    pushConstants.previousWorldRow2 = glm::vec4(
        previousWorld[0][2], previousWorld[1][2],
        previousWorld[2][2], previousWorld[3][2]);
}

} // namespace

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
        // The gradient shader ignores this binding, while the panorama sky
        // shader samples it as a regular 2D equirectangular texture.
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _drawImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
    }

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _singleImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_FRAGMENT_BIT);

        // Both post-process passes want exactly that shape: one sampled
        // colour image.  Every image involved outlives every frame, so these
        // sets are written once here rather than rebuilt per frame.
        _tonemapInputDescriptor = globalDescriptorAllocator.allocate(
            _device, _singleImageDescriptorLayout);

        DescriptorWriter tonemapWriter;
        tonemapWriter.write_image(
            0,
            _drawImage.imageView,
            _postProcessSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        tonemapWriter.update_set(_device, _tonemapInputDescriptor);

        // Anti-aliasing reads the tonemap's output, not the draw image.  Its
        // edge search compares lumas against fixed thresholds, which only
        // mean anything once the values are in display range.
        _fxaaInputDescriptor = globalDescriptorAllocator.allocate(
            _device, _singleImageDescriptorLayout);

        DescriptorWriter fxaaWriter;
        fxaaWriter.write_image(
            0,
            _tonemapImage.imageView,
            _postProcessSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        fxaaWriter.update_set(_device, _fxaaInputDescriptor);
    }

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        // The sunlight depth map lives beside the scene data because every
        // lit pass needs it, the portal cameras included.
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        // So does the finished ambient occlusion.  Portal cameras are bound
        // it as well, and a flag in the block itself is what stops them
        // reading occlusion computed for a different view.
        builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        // Compute is here because both occlusion passes bind this same set
        // for the projection and its inverse rather than duplicating them.
        _gpuSceneDataDescriptorLayout = builder.build(
            _device,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                VK_SHADER_STAGE_COMPUTE_BIT);
    }

    if (_ssaoFormat != VK_FORMAT_UNDEFINED) {
        // The sampling pass: the two prepass buffers it reads, the rotation
        // noise, the image it writes, and the kernel it walks.
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        builder.add_binding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        _ssaoDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_COMPUTE_BIT);

        // The blur: occlusion in, the two buffers that tell it which
        // neighbours belong to the same surface, and occlusion out.
        builder.clear();
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        _ssaoBlurDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_COMPUTE_BIT);

        // None of these images is recreated at runtime, so every set below is
        // written once here and never revisited.
        _ssaoDescriptor = globalDescriptorAllocator.allocate(
            _device, _ssaoDescriptorLayout);
        DescriptorWriter ssaoWriter;
        ssaoWriter.write_image(
            0,
            _prepassDepthImage.imageView,
            _prepassSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssaoWriter.write_image(
            1,
            _prepassNormalImage.imageView,
            _prepassSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssaoWriter.write_image(
            2,
            _ssaoNoiseImage.imageView,
            _ssaoNoiseSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        ssaoWriter.write_image(
            3,
            _ssaoRawImage.imageView,
            VK_NULL_HANDLE,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        ssaoWriter.write_buffer(
            4,
            _ssaoKernelBuffer.buffer,
            sizeof(SSAOKernelBlock),
            0,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        ssaoWriter.update_set(_device, _ssaoDescriptor);

        // The two blur directions share a layout and a pipeline; only which
        // image they read and which they write differs.
        const auto writeBlurSet = [&](VkDescriptorSet set,
                                      const AllocatedImage& source,
                                      const AllocatedImage& target) {
            DescriptorWriter blurWriter;
            blurWriter.write_image(
                0,
                source.imageView,
                _prepassSampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            blurWriter.write_image(
                1,
                _prepassDepthImage.imageView,
                _prepassSampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            blurWriter.write_image(
                2,
                _prepassNormalImage.imageView,
                _prepassSampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            blurWriter.write_image(
                3,
                target.imageView,
                VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            blurWriter.update_set(_device, set);
        };
        _ssaoBlurHorizontalDescriptor = globalDescriptorAllocator.allocate(
            _device, _ssaoBlurDescriptorLayout);
        writeBlurSet(
            _ssaoBlurHorizontalDescriptor, _ssaoRawImage, _ssaoBlurImage);
        _ssaoBlurVerticalDescriptor = globalDescriptorAllocator.allocate(
            _device, _ssaoBlurDescriptorLayout);
        writeBlurSet(
            _ssaoBlurVerticalDescriptor, _ssaoBlurImage, _ssaoFinalImage);

    }

    {
        // Camera depth and view-space normals, the inputs every screen-space
        // pass shares.  Neither image is recreated at runtime, so one set
        // describes them for the lifetime of the engine.
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _prepassImageDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
        _prepassImageDescriptor = globalDescriptorAllocator.allocate(
            _device, _prepassImageDescriptorLayout);

        DescriptorWriter writer;
        writer.write_image(
            0,
            _prepassDepthImage.imageView,
            _prepassSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writer.write_image(
            1,
            _prepassNormalImage.imageView,
            _prepassSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writer.update_set(_device, _prepassImageDescriptor);
    }

    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        for (uint32_t binding = 2; binding <= 8; ++binding) {
            builder.add_binding(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        }
        builder.add_binding(9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        builder.add_binding(10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        builder.add_binding(11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        builder.add_binding(14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        _ssgiDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_COMPUTE_BIT);
        for (VkDescriptorSet& set : _ssgiDescriptors) {
            set = globalDescriptorAllocator.allocate(_device, _ssgiDescriptorLayout);
        }

        builder.clear();
        for (uint32_t binding = 0; binding < 11; ++binding) {
            builder.add_binding(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        }
        _ssgiDebugDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_FRAGMENT_BIT);
        for (uint32_t historyIndex = 0;
             historyIndex < _ssgiDebugDescriptors.size(); ++historyIndex) {
            _ssgiDebugDescriptors[historyIndex] =
                globalDescriptorAllocator.allocate(
                    _device, _ssgiDebugDescriptorLayout);
            DescriptorWriter debugWriter;
            debugWriter.write_image(0, _gbufferAlbedoImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(1, _gbufferVelocityImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(2, _portalMaskImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(3, _directLightingImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(4, _ssgiRawImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(5, _ssgiDebugImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(6,
                _ssgiTemporalHistory[historyIndex].imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(7,
                _ssgiTemporalDiagnosticImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(8, _ssgiFilteredImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(9, _ssgiReferenceImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.write_image(10, _ssgiFallbackImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            debugWriter.update_set(
                _device, _ssgiDebugDescriptors[historyIndex]);
        }

        builder.clear();
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _ssgiFilterDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_COMPUTE_BIT);
        for (VkDescriptorSet& set : _ssgiFilterDescriptors) {
            set = globalDescriptorAllocator.allocate(
                _device, _ssgiFilterDescriptorLayout);
        }
        for (uint32_t historyIndex = 0; historyIndex < 2; ++historyIndex) {
            DescriptorWriter filterWriter;
            filterWriter.write_image(0,
                _ssgiTemporalHistory[historyIndex].imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            filterWriter.write_image(1, _ssgiFilterScratchImage.imageView,
                VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            filterWriter.write_image(2, _prepassDepthImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            filterWriter.write_image(3, _prepassNormalImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            filterWriter.update_set(
                _device, _ssgiFilterDescriptors[historyIndex]);
        }
        DescriptorWriter verticalWriter;
        verticalWriter.write_image(0, _ssgiFilterScratchImage.imageView,
            _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        verticalWriter.write_image(1, _ssgiFilteredImage.imageView,
            VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        verticalWriter.write_image(2, _prepassDepthImage.imageView,
            _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        verticalWriter.write_image(3, _prepassNormalImage.imageView,
            _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        verticalWriter.update_set(_device, _ssgiFilterDescriptors[2]);

        builder.clear();
        // Five now, not four: the trace stores incident radiance, so the
        // composite is where the receiver's base colour is applied.
        for (uint32_t binding = 0; binding < 5; ++binding) {
            builder.add_binding(
                binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        }
        _ssgiCompositeDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_FRAGMENT_BIT);
        for (uint32_t historyIndex = 0; historyIndex < 2; ++historyIndex) {
            _ssgiCompositeDescriptors[historyIndex] =
                globalDescriptorAllocator.allocate(
                    _device, _ssgiCompositeDescriptorLayout);
            DescriptorWriter compositeWriter;
            compositeWriter.write_image(0, _ssgiFilteredImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            compositeWriter.write_image(1,
                _ssgiMetadataHistory[historyIndex].imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            compositeWriter.write_image(2, _prepassDepthImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            compositeWriter.write_image(3, _prepassNormalImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            // draw_ssgi() leaves the forward pass's base-colour target in
            // SHADER_READ_ONLY_OPTIMAL, and it still holds this frame's
            // albedo when the composite runs immediately afterwards.
            compositeWriter.write_image(4, _gbufferAlbedoImage.imageView,
                _prepassSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            compositeWriter.update_set(
                _device, _ssgiCompositeDescriptors[historyIndex]);
        }
    }

    // A device that could not provide an occlusion format still has to bind
    // something at binding 2.  The shadow map is the one image guaranteed to
    // exist by this point; the flag in the scene block keeps it unread.
    const VkImageView occlusionView = _ssaoFinalImage.imageView != VK_NULL_HANDLE
        ? _ssaoFinalImage.imageView
        : _shadowMapImage.imageView;

    {
        // All three occlusion stages at once, for the debug views.  The layout
        // exists whether or not occlusion does, because the render-debug
        // pipeline is built against it either way.
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _ssaoDebugDescriptorLayout = builder.build(
            _device, VK_SHADER_STAGE_FRAGMENT_BIT);
        _ssaoDebugDescriptor = globalDescriptorAllocator.allocate(
            _device, _ssaoDebugDescriptorLayout);

        // Nearest, because a debug view should show the stored texel rather
        // than a filtered version of it.
        const std::array<VkImageView, 3> stages{
            _ssaoRawImage.imageView != VK_NULL_HANDLE
                ? _ssaoRawImage.imageView : occlusionView,
            _ssaoBlurImage.imageView != VK_NULL_HANDLE
                ? _ssaoBlurImage.imageView : occlusionView,
            _ssaoFinalImage.imageView != VK_NULL_HANDLE
                ? _ssaoFinalImage.imageView : occlusionView};
        DescriptorWriter debugWriter;
        for (int binding = 0; binding < 3; ++binding) {
            debugWriter.write_image(
                binding,
                stages[binding],
                _prepassSampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        }
        debugWriter.update_set(_device, _ssaoDebugDescriptor);
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
        sceneWriter.write_image(
            1,
            _shadowMapImage.imageView,
            _shadowSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        sceneWriter.write_image(
            2,
            occlusionView,
            _ssaoSampler != VK_NULL_HANDLE ? _ssaoSampler : _prepassSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
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
            portalSceneWriter.write_image(
                1,
                _shadowMapImage.imageView,
                _shadowSampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            // Bound so the layout is satisfied, never read: a portal camera
            // sets the flag in its own scene block to zero.
            portalSceneWriter.write_image(
                2,
                occlusionView,
                _ssaoSampler != VK_NULL_HANDLE ? _ssaoSampler : _prepassSampler,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
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
        vkDestroyDescriptorSetLayout(_device, _prepassImageDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssaoDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssaoBlurDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssaoDebugDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssgiDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssgiDebugDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(_device, _ssgiFilterDescriptorLayout, nullptr);
        vkDestroyDescriptorSetLayout(
            _device, _ssgiCompositeDescriptorLayout, nullptr);
    });
}

void VulkanEngine::init_pipelines()
{
    init_background_pipelines();
    init_shadow_pipeline();
    init_depth_normal_pipeline();
    // Deferred to the end of this function: both alpha-tested pipelines take
    // metalRoughMaterial.materialLayout, which build_pipelines() creates.
    init_render_debug_pipeline();
    init_tonemap_pipeline();
    init_fxaa_pipeline();
    init_ssao_pipelines();
    metalRoughMaterial.build_pipelines(this);
    init_shadow_mask_pipeline();
    init_depth_normal_mask_pipeline();
    init_ssgi_pipelines();
}

void VulkanEngine::draw_geometry(
    VkCommandBuffer cmd,
    const DrawContext& drawContext,
    const glm::mat4& viewProjection,
    VkDescriptorSet sceneDescriptor,
    bool clearDepthAndStencil,
    MaterialPipeline* overridePipeline,
    MaterialPipeline* overrideMaskPipeline,
    uint32_t stencilReference,
    bool useFrustumCulling,
    uint32_t stencilCompareMask,
    bool writeGBuffer)
{
    const auto startTime = std::chrono::steady_clock::now();

    std::array<VkRenderingAttachmentInfo, 4> colorAttachments{};
    colorAttachments[0] = vkinit::attachment_info(
        _drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkClearValue clear{};
    if (writeGBuffer) {
        colorAttachments[1] = vkinit::attachment_info(
            _gbufferAlbedoImage.imageView, &clear,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        colorAttachments[2] = vkinit::attachment_info(
            _gbufferVelocityImage.imageView, &clear,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        colorAttachments[3] = vkinit::attachment_info(
            _directLightingImage.imageView, &clear,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    depthAttachment.loadOp = clearDepthAndStencil
        ? VK_ATTACHMENT_LOAD_OP_CLEAR
        : VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderingAttachmentInfo stencilAttachment = depthAttachment;

    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, colorAttachments.data(), &depthAttachment);
    renderInfo.colorAttachmentCount = writeGBuffer ? 4u : 1u;
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

        // An overriding pass still has to honour the alpha cutoff, or masked
        // geometry reappears as a solid rectangle the moment it is seen
        // through a portal.  Passes that write a stencil or a mask rather
        // than shading pass no mask pipeline and fall back to the one
        // override, which is what they want: the cutout's silhouette is not
        // what those passes are describing.
        MaterialPipeline* pipeline = renderObject.material->pipeline;
        if (overridePipeline != nullptr) {
            pipeline = overrideMaskPipeline != nullptr &&
                    renderObject.material->passType == MaterialPass::Mask
                ? overrideMaskPipeline
                : overridePipeline;
        }
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
        set_previous_world_rows(pushConstants, renderObject.previousTransform);
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
    // Portal cameras and the first main-camera frame default to zero motion.
    // update_scene() replaces this with the previous main view-projection for
    // the ordinary camera.
    data.previousViewProjection = data.viewproj;
    // Keep authored interiors readable even when a face is turned away from
    // the single directional sun light.
    data.ambientColor = glm::vec4(0.28f);
    data.sunlightDirection = glm::vec4(
        normalized_sun_direction(_sunlightDirection), 1.0f);
    data.sunlightColor = glm::vec4(1.0f);
    // update_scene() settles this before any camera's buffer is filled, so
    // the portal views inherit exactly the main camera's shadow map.
    data.sunViewProjection = _sunViewProjection;
    data.shadowSettings = glm::vec4(
        _shadowDepthBias,
        _shadowNormalBias,
        1.0f / static_cast<float>(ShadowMapResolution),
        _shadowsEnabled ? 1.0f : 0.0f);
    data.shadowFilterSettings = glm::vec4(
        _shadowFilterRadius, 0.0f, 0.0f, 0.0f);
    // Screen-space passes get a depth buffer rather than a position per
    // fragment; these are what turn one back into the other.
    data.inverseProjection = glm::inverse(data.proj);
    data.inverseViewProjection = glm::inverse(data.viewproj);

    // Only the main camera gets ambient occlusion.  build_portal_scene_data()
    // clears this again for every virtual camera, because the occlusion image
    // describes what is in front of the player, not what is through a portal.
    data.screenSpaceSettings = glm::vec4(
        ssao_active() ? 1.0f : 0.0f,
        _ssaoAmbientOnly ? 1.0f : 0.0f,
        _ssgiEnabled ? 1.0f : 0.0f,
        _ssgiHalfResolution ? 1.0f : 0.0f);
    // One multiply takes a fragment coordinate to its occlusion texel.  It
    // folds together the half resolution, the render scale, and the fact that
    // the image stays allocated at window size, none of which a fragment
    // shader can work out for itself.
    const VkExtent2D activeOcclusion = active_ssao_extent();
    const glm::vec2 occlusionAllocation(
        static_cast<float>(std::max(1u, _ssaoExtent.width)),
        static_cast<float>(std::max(1u, _ssaoExtent.height)));
    const glm::vec2 occlusionFraction(
        static_cast<float>(activeOcclusion.width) / occlusionAllocation.x,
        static_cast<float>(activeOcclusion.height) / occlusionAllocation.y);
    data.ambientOcclusionUV = glm::vec4(
        occlusionFraction.x / static_cast<float>(_drawExtent.width),
        occlusionFraction.y / static_cast<float>(_drawExtent.height),
        occlusionFraction.x - 0.5f / occlusionAllocation.x,
        occlusionFraction.y - 0.5f / occlusionAllocation.y);
    data.ssgiFallbackSettings = _traceLighting.environment;
    // Ambient and SSGI are two answers to one question, so how they divide it
    // travels with every camera rather than being decided in the shader.
    data.indirectSettings = glm::vec4(
        _ssgiAmbientRetention,
        _ssgiTraceEnvironmentMap ? 1.0f : 0.0f,
        _skyboxEnvironmentLod,
        0.0f);
    return data;
}

glm::mat4 VulkanEngine::compute_sun_view_projection(const glm::vec3& focusPoint) const
{
    const glm::vec3 toLight = normalized_sun_direction(_sunlightDirection);
    // glm::lookAt degenerates when its up vector lies along the view axis.
    const glm::vec3 up = std::abs(toLight.y) > 0.99f
        ? glm::vec3(0.0f, 0.0f, 1.0f)
        : glm::vec3(0.0f, 1.0f, 0.0f);
    // Rotation only.  Pinning the light's origin to the world origin is what
    // gives the snap below a texel grid that does not move with the player.
    const glm::mat4 lightRotation = glm::lookAt(
        glm::vec3(0.0f), -toLight, up);

    glm::vec3 focus = glm::vec3(lightRotation * glm::vec4(focusPoint, 1.0f));
    // Without this the grid slides continuously under the world as the player
    // walks, and every shadow edge crawls.
    const float texelWorldSize =
        (2.0f * _shadowRadius) / static_cast<float>(ShadowMapResolution);
    focus.x = std::floor(focus.x / texelWorldSize) * texelWorldSize;
    focus.y = std::floor(focus.y / texelWorldSize) * texelWorldSize;

    // The extra depth reaches casters standing outside the lit box; only the
    // x/y extent decides how much ground can receive a shadow.
    const float depthHalfRange = _shadowRadius + _shadowDepthMargin;
    glm::mat4 projection = glm::ortho(
        focus.x - _shadowRadius,
        focus.x + _shadowRadius,
        focus.y - _shadowRadius,
        focus.y + _shadowRadius,
        -focus.z - depthHalfRange,
        -focus.z + depthHalfRange);
    // Vulkan's framebuffer Y runs opposite glm's, exactly as the main camera
    // projection corrects it.
    projection[1][1] *= -1.0f;
    return projection * lightRotation;
}

void VulkanEngine::init_shadow_resources()
{
    // Comparison sampling is a format capability, not a guarantee of every
    // depth format. Prefer D32 with bilinear comparison filtering, then fall
    // back through other depth-only formats and nearest filtering if needed.
    const std::array<VkFormat, 3> candidates{
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D16_UNORM,
        VK_FORMAT_X8_D24_UNORM_PACK32};
    constexpr VkFormatFeatureFlags2 requiredFeatures =
        VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT;
    VkFormat shadowFormat = VK_FORMAT_UNDEFINED;
    VkFilter shadowFilter = VK_FILTER_NEAREST;
    VkFormat nearestFallback = VK_FORMAT_UNDEFINED;
    for (VkFormat candidate : candidates) {
        VkFormatProperties3 properties3{
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 properties2{
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
            .pNext = &properties3};
        vkGetPhysicalDeviceFormatProperties2(
            _chosenGPU, candidate, &properties2);
        if ((properties3.optimalTilingFeatures & requiredFeatures) !=
            requiredFeatures) {
            continue;
        }
        if (nearestFallback == VK_FORMAT_UNDEFINED) {
            nearestFallback = candidate;
        }
        if ((properties3.optimalTilingFeatures &
             VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0) {
            shadowFormat = candidate;
            shadowFilter = VK_FILTER_LINEAR;
            break;
        }
    }
    if (shadowFormat == VK_FORMAT_UNDEFINED) {
        shadowFormat = nearestFallback;
    }
    if (shadowFormat == VK_FORMAT_UNDEFINED) {
        fmt::print("No depth format supports sampled shadow comparison\n");
        abort();
    }
    _shadowFormatName = string_VkFormat(shadowFormat);

    _shadowMapImage = create_image(
        VkExtent3D{ShadowMapResolution, ShadowMapResolution, 1},
        shadowFormat,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    // A comparison sampler runs the depth test in hardware and filters the
    // results, which is what makes each PCF tap cheap.  The white border
    // leaves everything outside the shadowed box lit, instead of stamping a
    // dark square edge onto the world.
    VkSamplerCreateInfo samplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = shadowFilter;
    samplerInfo.minFilter = shadowFilter;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    samplerInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(_device, &samplerInfo, nullptr, &_shadowSampler));

    _mainDeletionQueue.push_function([this]() {
        vkDestroySampler(_device, _shadowSampler, nullptr);
        destroy_image(_shadowMapImage);
    });
}

void VulkanEngine::init_shadow_pipeline()
{
    VkShaderModule shadowVertexShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module(
            "../../shaders/shadow_depth.vert.spv", _device, &shadowVertexShader)) {
        fmt::print("Error loading shadow depth shader\n");
        return;
    }

    // No descriptor sets at all: the light matrix arrives already multiplied
    // into the push constant, and a depth-only pass reads no material.
    VkPushConstantRange pushRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(ShadowPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &layoutInfo, nullptr, &_shadowPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _shadowPipeline.layout;
    builder.set_vertex_only_shader(shadowVertexShader);
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    // The level's walls and floors are single-sided quads, so culling here
    // would simply delete their shadows.  Depth bias, not back-face culling,
    // is what stops a surface from shadowing itself.
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    builder.disable_blending();
    builder.disable_color_attachment();
    // Conventional depth, unlike the reversed-depth main camera: this pass
    // clears to 1 and keeps whatever lies nearest the sun.
    builder.enable_depthtest(true, VK_COMPARE_OP_LESS_OR_EQUAL);
    builder.enable_depth_bias(1.25f, 2.75f);
    builder.set_depth_format(_shadowMapImage.imageFormat);
    _shadowPipeline.pipeline = builder.build_pipeline(_device);

    vkDestroyShaderModule(_device, shadowVertexShader, nullptr);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _shadowPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _shadowPipeline.layout, nullptr);
    });
}

void VulkanEngine::init_shadow_mask_pipeline()
{
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module(
            "../../shaders/shadow_depth_mask.vert.spv", _device, &vertexShader) ||
        !vkutil::load_shader_module(
            "../../shaders/shadow_depth_mask.frag.spv", _device, &fragmentShader)) {
        fmt::print("Error loading alpha-tested shadow shaders\n");
        if (vertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, vertexShader, nullptr);
        }
        if (fragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, fragmentShader, nullptr);
        }
        return;
    }

    // One descriptor set, and it is the per-material set the forward pass
    // binds at index 1.  Nothing about the sun's camera is needed here - it
    // is still folded into the push constant - so the scene set the other
    // passes carry would be dead weight, and set 0 is the material instead.
    VkPushConstantRange pushRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(ShadowPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &metalRoughMaterial.materialLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &layoutInfo, nullptr, &_shadowMaskPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _shadowMaskPipeline.layout;
    builder.set_shaders(vertexShader, fragmentShader);
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    // Every state below matches the opaque shadow pipeline exactly.  Where a
    // masked surface and an opaque one meet, any disagreement here would show
    // up as a seam between the two shadows they cast.
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    builder.disable_blending();
    builder.disable_color_attachment();
    builder.enable_depthtest(true, VK_COMPARE_OP_LESS_OR_EQUAL);
    builder.enable_depth_bias(1.25f, 2.75f);
    builder.set_depth_format(_shadowMapImage.imageFormat);
    _shadowMaskPipeline.pipeline = builder.build_pipeline(_device);

    vkDestroyShaderModule(_device, fragmentShader, nullptr);
    vkDestroyShaderModule(_device, vertexShader, nullptr);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _shadowMaskPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _shadowMaskPipeline.layout, nullptr);
    });
}

void VulkanEngine::draw_shadow_map(VkCommandBuffer cmd)
{
    const auto startTime = std::chrono::steady_clock::now();

    vkutil::transition_image(
        cmd,
        _shadowMapImage.image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _shadowMapImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    // The main camera clears to 0 because its depth is reversed.  Here 1 is
    // the far value, and it means nothing stands between this texel and the
    // sun.
    depthAttachment.clearValue.depthStencil.depth = 1.0f;

    const VkExtent2D shadowExtent{ShadowMapResolution, ShadowMapResolution};
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        shadowExtent, nullptr, &depthAttachment);

    vkCmdBeginRendering(cmd, &renderInfo);

    VkViewport viewport{};
    viewport.width = static_cast<float>(shadowExtent.width);
    viewport.height = static_cast<float>(shadowExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = shadowExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    // This pass has no stencil attachment, but the shared pipeline builder
    // declares the stencil state dynamic on every pipeline it produces.
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0x00);

    if (_shadowsEnabled && _shadowPipeline.pipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(
            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _shadowPipeline.pipeline);

        VkPipeline lastPipeline = _shadowPipeline.pipeline;
        MaterialInstance* lastMaterial = nullptr;
        VkBuffer lastIndexBuffer = VK_NULL_HANDLE;
        // portalViewDrawContext is the world plus the player's body, which is
        // what actually exists in the level.  The main camera's context is
        // not usable here: it also carries the portal quads, and it omits the
        // body the first-person camera sits inside.
        // Transparent surfaces intentionally do not cast yet: a blended
        // surface has no single silhouette to cast.  Masked ones do, and they
        // go through _shadowMaskPipeline so foliage and grates cast their
        // cutout rather than the rectangle it is painted on.
        for (const RenderObject& renderObject :
                portalViewDrawContext.OpaqueSurfaces) {
            // Cull against the light, never against the player's camera: an
            // object behind the camera can still drop a shadow into view.
            if (!is_visible(renderObject, _sunViewProjection)) {
                continue;
            }

            const bool masked = renderObject.material != nullptr &&
                renderObject.material->passType == MaterialPass::Mask &&
                _shadowMaskPipeline.pipeline != VK_NULL_HANDLE;
            const MaterialPipeline& active = masked
                ? _shadowMaskPipeline
                : _shadowPipeline;
            if (active.pipeline != lastPipeline) {
                lastPipeline = active.pipeline;
                vkCmdBindPipeline(
                    cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, active.pipeline);
                // The opaque pipeline's layout declares no sets at all, so
                // the two are compatible for nothing: whatever was bound for
                // the mask pipeline does not survive a round trip through it.
                lastMaterial = nullptr;
            }
            if (masked && renderObject.material != lastMaterial) {
                lastMaterial = renderObject.material;
                // Set 0 here, not 1.  This pipeline carries the material set
                // alone, because the sun's camera is already folded into the
                // push constant and nothing else about the scene is read.
                vkCmdBindDescriptorSets(
                    cmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    active.layout,
                    0,
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

            ShadowPushConstants pushConstants{};
            pushConstants.lightMatrix = _sunViewProjection * renderObject.transform;
            pushConstants.vertexBuffer = renderObject.vertexBufferAddress;
            vkCmdPushConstants(
                cmd,
                active.layout,
                VK_SHADER_STAGE_VERTEX_BIT,
                0,
                sizeof(ShadowPushConstants),
                &pushConstants);
            vkCmdDrawIndexed(
                cmd, renderObject.indexCount, 1, renderObject.firstIndex, 0, 0);

            ++stats.shadow_drawcall_count;
            stats.shadow_triangle_count +=
                static_cast<int>(renderObject.indexCount / 3);
        }
    }

    vkCmdEndRendering(cmd);

    vkutil::transition_image(
        cmd,
        _shadowMapImage.image,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    stats.shadow_record_time = std::chrono::duration<float, std::milli>(
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

void VulkanEngine::draw_collider_debug_bounds(VkCommandBuffer cmd)
{
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
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

    vkCmdBindPipeline(
        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _colliderDebugPipeline.pipeline);
    for (const SceneObject& object : _scene.objects) {
        if (!_showColliderBounds || !object.alive || !object.hasCollision) {
            continue;
        }

        // Draw the final world AABB used by physics—not merely the object's
        // local box—so a rotated object still shows the conservative bounds
        // the player and portal raycast actually use.
        AABB collider{};
        if (object.collisionShape == CollisionShape::GroundPlane) {
            // Ground-plane physics is intentionally zero-thickness. Give it a
            // thin debug-only box so platform colliders are still visible.
            const glm::mat4 world = _scene.world_matrix(object.id);
            const glm::vec3 planeCenter = glm::vec3(world[3]);
            const glm::vec3 planeHalfExtents{
                glm::length(glm::vec3(world[0])) * 0.5f,
                0.01f,
                glm::length(glm::vec3(world[2])) * 0.5f};
            collider.min = planeCenter - planeHalfExtents;
            collider.max = planeCenter + planeHalfExtents;
        } else {
            collider = collider_from_object(_scene, object.id);
        }
        const glm::vec3 center = (collider.min + collider.max) * 0.5f;
        const glm::vec3 fullExtents = (collider.max - collider.min) * 1.005f;
        const glm::mat4 colliderTransform = glm::translate(
            glm::mat4(1.0f), center) * glm::scale(
            glm::mat4(1.0f), fullExtents);

        ColliderDebugPushConstants pushConstants{};
        pushConstants.viewProjection = sceneData.viewproj;
        pushConstants.model = colliderTransform;
        vkCmdPushConstants(
            cmd,
            _colliderDebugPipeline.layout,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(ColliderDebugPushConstants),
            &pushConstants);
        vkCmdDraw(cmd, 24, 1, 0, 0);
        ++stats.drawcall_count;
    }

    if (_showShadowBounds) {
        // Convert the unit debug cube into the exact orthographic volume used
        // by the shadow map: clip x/y are [-1, 1], while Vulkan depth is [0, 1].
        const glm::mat4 clipBox = glm::translate(
            glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.5f)) * glm::scale(
            glm::mat4(1.0f), glm::vec3(2.0f, 2.0f, 1.0f));
        ColliderDebugPushConstants pushConstants{};
        pushConstants.viewProjection = sceneData.viewproj;
        pushConstants.model = glm::inverse(_sunViewProjection) * clipBox;
        vkCmdPushConstants(
            cmd,
            _colliderDebugPipeline.layout,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(ColliderDebugPushConstants),
            &pushConstants);
        vkCmdDraw(cmd, 24, 1, 0, 0);
        ++stats.drawcall_count;
    }
    vkCmdEndRendering(cmd);
}

void VulkanEngine::init_depth_normal_resources()
{
    // Screen-space effects sample this depth, so an attachment-only format is
    // not enough. Ask the device which candidate supports both uses instead
    // of assuming, exactly as the shadow map and the main depth buffer do.
    static constexpr std::array<VkFormat, 2> depthCandidates{
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D16_UNORM};
    constexpr VkFormatFeatureFlags requiredDepthFeatures =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    VkFormat prepassDepthFormat = VK_FORMAT_UNDEFINED;
    for (VkFormat candidate : depthCandidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(_chosenGPU, candidate, &properties);
        if ((properties.optimalTilingFeatures & requiredDepthFeatures) ==
            requiredDepthFeatures) {
            prepassDepthFormat = candidate;
            break;
        }
    }
    if (prepassDepthFormat == VK_FORMAT_UNDEFINED) {
        fmt::print("No depth format supports a sampled depth prepass\n");
        abort();
    }

    // Allocated at the full window size and only partly written when the
    // resolution scale is below 1, which is what lets renderScale change
    // without recreating any of these images.
    const VkExtent3D prepassExtent = _drawImage.imageExtent;
    _prepassDepthImage = create_image(
        prepassExtent,
        prepassDepthFormat,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    // Wider than normals need, but trivial to read in the debug views. Two
    // signed 16-bit channels with octahedral encoding is the later saving.
    _prepassNormalImage = create_image(
        prepassExtent,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    // Nearest and clamped: these buffers are read per pixel, and filtering
    // across a silhouette would blend two unrelated surfaces into a position
    // that exists nowhere in the scene.
    VkSamplerCreateInfo samplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(_device, &samplerInfo, nullptr, &_prepassSampler));

    _mainDeletionQueue.push_function([this]() {
        vkDestroySampler(_device, _prepassSampler, nullptr);
        destroy_image(_prepassNormalImage);
        destroy_image(_prepassDepthImage);
    });
}

void VulkanEngine::init_ssgi_resources()
{
    const auto requireFormat = [&](VkFormat format, VkFormatFeatureFlags required,
                                   const char* label) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(_chosenGPU, format, &properties);
        if ((properties.optimalTilingFeatures & required) != required) {
            fmt::print("Required SSGI {} format is unsupported\n", label);
            std::abort();
        }
    };
    requireFormat(
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        "albedo");
    requireFormat(
        VK_FORMAT_R8_UNORM,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        "portal mask");
    requireFormat(
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
        "HDR/velocity");

    const VkExtent3D extent = _drawImage.imageExtent;
    _gbufferAlbedoImage = create_image(
        extent, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _gbufferVelocityImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _directLightingImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    _portalMaskImage = create_image(
        extent, VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _ssgiRawImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _ssgiDebugImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _ssgiFallbackImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    for (AllocatedImage& history : _ssgiTemporalHistory) {
        history = create_image(
            extent, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }
    for (AllocatedImage& history : _ssgiMetadataHistory) {
        history = create_image(
            extent, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }
    _ssgiTemporalDiagnosticImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _ssgiFilterScratchImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    _ssgiFilteredImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    _ssgiReferenceImage = create_image(
        extent, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

    AllocatedBuffer referenceUpload{};
    if (const char* referencePath = std::getenv("MIRABILIS_SSGI_REFERENCE")) {
        std::ifstream file(referencePath, std::ios::binary);
        std::string magic;
        uint32_t width = 0, height = 0;
        float scale = 0.0f;
        file >> magic >> width >> height >> scale;
        file.get();
        if (!file || magic != "PF" || width != extent.width ||
            height != extent.height || scale >= 0.0f) {
            fmt::print("SSGI reference must be a matching little-endian RGB PFM: {}\n",
                referencePath);
            std::abort();
        }
        std::vector<uint64_t> packed(size_t(width) * height);
        for (int y = int(height) - 1; y >= 0; --y) {
            for (uint32_t x = 0; x < width; ++x) {
                glm::vec3 rgb{};
                file.read(reinterpret_cast<char*>(&rgb), 3 * sizeof(float));
                const uint64_t rg = glm::packHalf2x16(glm::vec2(rgb));
                const uint64_t ba = glm::packHalf2x16(glm::vec2(rgb.z, 1.0f));
                packed[size_t(y) * width + x] = rg | (ba << 32u);
            }
        }
        if (!file) std::abort();
        referenceUpload = create_buffer(
            packed.size() * sizeof(uint64_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU);
        std::memcpy(referenceUpload.info.pMappedData, packed.data(),
            packed.size() * sizeof(uint64_t));
        _ssgiReferenceLoaded = true;
        fmt::print("Loaded SSGI reference: {} ({}x{})\n",
            referencePath, width, height);
    }
    for (AllocatedImage& history : _directLightingHistory) {
        history = create_image(
            extent, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }

    // Give persistent descriptors valid layouts before their first frame.
    // The history contents remain invalid until the first forward pass copies
    // into one side of the pair.
    immediate_submit([&](VkCommandBuffer cmd) {
        const std::array<AllocatedImage*, 6> sampledImages{
            &_gbufferAlbedoImage,
            &_gbufferVelocityImage,
            &_directLightingImage,
            &_portalMaskImage,
            &_directLightingHistory[0],
            &_directLightingHistory[1]};
        for (const AllocatedImage* image : sampledImages) {
            vkutil::transition_image(
                cmd, image->image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        vkutil::transition_image(
            cmd, _ssgiRawImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiDebugImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiFallbackImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
        for (AllocatedImage& history : _ssgiTemporalHistory) {
            vkutil::transition_image(
                cmd, history.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL);
        }
        for (AllocatedImage& history : _ssgiMetadataHistory) {
            vkutil::transition_image(
                cmd, history.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL);
        }
        vkutil::transition_image(
            cmd, _ssgiTemporalDiagnosticImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiFilterScratchImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiFilteredImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(cmd, _ssgiReferenceImage.image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange referenceRange{
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (_ssgiReferenceLoaded) {
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = extent;
            vkCmdCopyBufferToImage(cmd, referenceUpload.buffer,
                _ssgiReferenceImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &copy);
        } else {
            VkClearColorValue clear{};
            vkCmdClearColorImage(cmd, _ssgiReferenceImage.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                &referenceRange);
        }
        vkutil::transition_image(cmd, _ssgiReferenceImage.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    if (_ssgiReferenceLoaded) {
        destroy_buffer(referenceUpload);
    }

    _mainDeletionQueue.push_function([this]() {
        destroy_image(_gbufferAlbedoImage);
        destroy_image(_gbufferVelocityImage);
        destroy_image(_directLightingImage);
        destroy_image(_portalMaskImage);
        destroy_image(_ssgiRawImage);
        destroy_image(_ssgiDebugImage);
        destroy_image(_ssgiFallbackImage);
        for (const AllocatedImage& history : _ssgiTemporalHistory) {
            destroy_image(history);
        }
        for (const AllocatedImage& history : _ssgiMetadataHistory) {
            destroy_image(history);
        }
        destroy_image(_ssgiTemporalDiagnosticImage);
        destroy_image(_ssgiFilterScratchImage);
        destroy_image(_ssgiFilteredImage);
        destroy_image(_ssgiReferenceImage);
        for (const AllocatedImage& history : _directLightingHistory) {
            destroy_image(history);
        }
    });
}

void VulkanEngine::init_depth_normal_pipeline()
{
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module(
            "../../shaders/depth_normal.vert.spv", _device, &vertexShader) ||
        !vkutil::load_shader_module(
            "../../shaders/depth_normal.frag.spv", _device, &fragmentShader)) {
        fmt::print("Error loading depth/normal prepass shaders\n");
        if (vertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, vertexShader, nullptr);
        }
        if (fragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, fragmentShader, nullptr);
        }
        return;
    }

    // Scene data only. The prepass reads no material, and it takes the same
    // push constants as the main pass so both can share one draw loop.
    VkPushConstantRange matrixRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(GPUDrawPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &_gpuSceneDataDescriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &matrixRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &layoutInfo, nullptr, &_depthNormalPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _depthNormalPipeline.layout;
    builder.set_shaders(vertexShader, fragmentShader);
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    // Every rasterizer state below matches the main opaque pipeline. If the
    // two disagree, occlusion computed here lands on the wrong pixels of the
    // shaded image.
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    builder.disable_blending();
    builder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
    builder.set_color_attachment_format(_prepassNormalImage.imageFormat);
    builder.set_depth_format(_prepassDepthImage.imageFormat);
    _depthNormalPipeline.pipeline = builder.build_pipeline(_device);

    vkDestroyShaderModule(_device, fragmentShader, nullptr);
    vkDestroyShaderModule(_device, vertexShader, nullptr);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _depthNormalPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _depthNormalPipeline.layout, nullptr);
    });
}

void VulkanEngine::init_depth_normal_mask_pipeline()
{
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module(
            "../../shaders/depth_normal_mask.vert.spv", _device, &vertexShader) ||
        !vkutil::load_shader_module(
            "../../shaders/depth_normal_mask.frag.spv", _device, &fragmentShader)) {
        fmt::print("Error loading alpha-tested prepass shaders\n");
        if (vertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, vertexShader, nullptr);
        }
        if (fragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, fragmentShader, nullptr);
        }
        return;
    }

    // Scene data and the material, in the same order the forward pass uses
    // them, so a material set written for that pass binds here unchanged.
    VkDescriptorSetLayout layouts[] = {
        _gpuSceneDataDescriptorLayout,
        metalRoughMaterial.materialLayout};
    VkPushConstantRange matrixRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(GPUDrawPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &matrixRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &layoutInfo, nullptr, &_depthNormalMaskPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _depthNormalMaskPipeline.layout;
    builder.set_shaders(vertexShader, fragmentShader);
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    // Matching the opaque prepass for the same reason it matches the main
    // pass: occlusion computed here has to land on the shaded image's pixels.
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    builder.disable_blending();
    builder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
    builder.set_color_attachment_format(_prepassNormalImage.imageFormat);
    builder.set_depth_format(_prepassDepthImage.imageFormat);
    _depthNormalMaskPipeline.pipeline = builder.build_pipeline(_device);

    vkDestroyShaderModule(_device, fragmentShader, nullptr);
    vkDestroyShaderModule(_device, vertexShader, nullptr);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _depthNormalMaskPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _depthNormalMaskPipeline.layout, nullptr);
    });
}

void VulkanEngine::init_render_debug_pipeline()
{
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module(
            "../../shaders/render_debug.vert.spv", _device, &vertexShader) ||
        !vkutil::load_shader_module(
            "../../shaders/render_debug.frag.spv", _device, &fragmentShader)) {
        fmt::print("Error loading render debug shaders\n");
        if (vertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, vertexShader, nullptr);
        }
        if (fragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, fragmentShader, nullptr);
        }
        return;
    }

    VkDescriptorSetLayout layouts[] = {
        _gpuSceneDataDescriptorLayout,
        _prepassImageDescriptorLayout,
        _ssaoDebugDescriptorLayout,
        _ssgiDebugDescriptorLayout};
    VkPushConstantRange settingsRange{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(RenderDebugPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
    layoutInfo.setLayoutCount = 4;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &settingsRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &layoutInfo, nullptr, &_renderDebugPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _renderDebugPipeline.layout;
    builder.set_shaders(vertexShader, fragmentShader);
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    builder.disable_blending();
    // It replaces the finished image outright, so it neither tests nor writes
    // the depth buffer the portal passes still depend on.
    builder.disable_depthtest();
    builder.set_color_attachment_format(_drawImage.imageFormat);
    builder.set_depth_format(VK_FORMAT_UNDEFINED);
    _renderDebugPipeline.pipeline = builder.build_pipeline(_device);

    vkDestroyShaderModule(_device, fragmentShader, nullptr);
    vkDestroyShaderModule(_device, vertexShader, nullptr);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _renderDebugPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _renderDebugPipeline.layout, nullptr);
    });
}

void VulkanEngine::init_ssgi_pipelines()
{
    VkShaderModule computeShader = VK_NULL_HANDLE;
    VkShaderModule temporalShader = VK_NULL_HANDLE;
    VkShaderModule filterShader = VK_NULL_HANDLE;
    VkShaderModule compositeVertexShader = VK_NULL_HANDLE;
    VkShaderModule compositeFragmentShader = VK_NULL_HANDLE;
    VkShaderModule meshVertexShader = VK_NULL_HANDLE;
    VkShaderModule maskFragmentShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module(
            "../../shaders/ssgi.comp.spv", _device, &computeShader) ||
        !vkutil::load_shader_module(
            "../../shaders/ssgi_temporal.comp.spv", _device,
            &temporalShader) ||
        !vkutil::load_shader_module(
            "../../shaders/ssgi_bilateral.comp.spv", _device, &filterShader) ||
        !vkutil::load_shader_module(
            "../../shaders/render_debug.vert.spv", _device,
            &compositeVertexShader) ||
        !vkutil::load_shader_module(
            "../../shaders/ssgi_composite.frag.spv", _device,
            &compositeFragmentShader) ||
        !vkutil::load_shader_module(
            "../../shaders/mesh.vert.spv", _device, &meshVertexShader) ||
        !vkutil::load_shader_module(
            "../../shaders/portal_mask_output.frag.spv", _device,
            &maskFragmentShader)) {
        fmt::print("Error loading SSGI shaders\n");
        if (computeShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, computeShader, nullptr);
        }
        if (temporalShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, temporalShader, nullptr);
        }
        if (filterShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, filterShader, nullptr);
        }
        if (compositeVertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, compositeVertexShader, nullptr);
        }
        if (compositeFragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, compositeFragmentShader, nullptr);
        }
        if (meshVertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, meshVertexShader, nullptr);
        }
        if (maskFragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, maskFragmentShader, nullptr);
        }
        return;
    }

    const std::array<VkDescriptorSetLayout, 2> computeLayouts{
        _gpuSceneDataDescriptorLayout, _ssgiDescriptorLayout};
    VkPushConstantRange computeRange{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(SSGIPushConstants)};
    VkPipelineLayoutCreateInfo computeLayoutInfo =
        vkinit::pipeline_layout_create_info();
    computeLayoutInfo.setLayoutCount =
        static_cast<uint32_t>(computeLayouts.size());
    computeLayoutInfo.pSetLayouts = computeLayouts.data();
    computeLayoutInfo.pushConstantRangeCount = 1;
    computeLayoutInfo.pPushConstantRanges = &computeRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &computeLayoutInfo, nullptr, &_ssgiPipelineLayout));

    VkComputePipelineCreateInfo computeInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    computeInfo.layout = _ssgiPipelineLayout;
    computeInfo.stage = vkinit::pipeline_shader_stage_create_info(
        VK_SHADER_STAGE_COMPUTE_BIT, computeShader);
    VK_CHECK(vkCreateComputePipelines(
        _device, VK_NULL_HANDLE, 1, &computeInfo, nullptr, &_ssgiPipeline));
    computeInfo.stage = vkinit::pipeline_shader_stage_create_info(
        VK_SHADER_STAGE_COMPUTE_BIT, temporalShader);
    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
        &computeInfo, nullptr, &_ssgiTemporalPipeline));

    VkPushConstantRange filterRange{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(SSGIFilterPushConstants)};
    VkPipelineLayoutCreateInfo filterLayoutInfo =
        vkinit::pipeline_layout_create_info();
    filterLayoutInfo.setLayoutCount = 1;
    filterLayoutInfo.pSetLayouts = &_ssgiFilterDescriptorLayout;
    filterLayoutInfo.pushConstantRangeCount = 1;
    filterLayoutInfo.pPushConstantRanges = &filterRange;
    VK_CHECK(vkCreatePipelineLayout(_device, &filterLayoutInfo, nullptr,
        &_ssgiFilterPipelineLayout));
    computeInfo.layout = _ssgiFilterPipelineLayout;
    computeInfo.stage = vkinit::pipeline_shader_stage_create_info(
        VK_SHADER_STAGE_COMPUTE_BIT, filterShader);
    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
        &computeInfo, nullptr, &_ssgiFilterPipeline));

    VkPushConstantRange compositeRange{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(SSGICompositePushConstants)};
    VkPipelineLayoutCreateInfo compositeLayoutInfo =
        vkinit::pipeline_layout_create_info();
    compositeLayoutInfo.setLayoutCount = 1;
    compositeLayoutInfo.pSetLayouts = &_ssgiCompositeDescriptorLayout;
    compositeLayoutInfo.pushConstantRangeCount = 1;
    compositeLayoutInfo.pPushConstantRanges = &compositeRange;
    VK_CHECK(vkCreatePipelineLayout(_device, &compositeLayoutInfo, nullptr,
        &_ssgiCompositePipeline.layout));
    PipelineBuilder compositeBuilder;
    compositeBuilder._pipelineLayout = _ssgiCompositePipeline.layout;
    compositeBuilder.set_shaders(
        compositeVertexShader, compositeFragmentShader);
    compositeBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    compositeBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    compositeBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    compositeBuilder.set_multisampling_none();
    compositeBuilder.enable_blending_additive();
    compositeBuilder.disable_depthtest();
    compositeBuilder.set_color_attachment_format(_drawImage.imageFormat);
    compositeBuilder.set_depth_format(VK_FORMAT_UNDEFINED);
    _ssgiCompositePipeline.pipeline =
        compositeBuilder.build_pipeline(_device);

    const std::array<VkDescriptorSetLayout, 2> maskLayouts{
        _gpuSceneDataDescriptorLayout, metalRoughMaterial.materialLayout};
    VkPushConstantRange maskRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(GPUDrawPushConstants)};
    VkPipelineLayoutCreateInfo maskLayoutInfo =
        vkinit::pipeline_layout_create_info();
    maskLayoutInfo.setLayoutCount = static_cast<uint32_t>(maskLayouts.size());
    maskLayoutInfo.pSetLayouts = maskLayouts.data();
    maskLayoutInfo.pushConstantRangeCount = 1;
    maskLayoutInfo.pPushConstantRanges = &maskRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &maskLayoutInfo, nullptr, &_ssgiPortalMaskPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _ssgiPortalMaskPipeline.layout;
    builder.set_shaders(meshVertexShader, maskFragmentShader);
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    builder.disable_blending();
    builder.enable_depthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);
    builder.set_color_attachment_format(_portalMaskImage.imageFormat);
    builder.set_depth_format(_depthImage.imageFormat);
    builder.set_stencil_format(_depthImage.imageFormat);
    _ssgiPortalMaskPipeline.pipeline = builder.build_pipeline(_device);

    vkDestroyShaderModule(_device, maskFragmentShader, nullptr);
    vkDestroyShaderModule(_device, meshVertexShader, nullptr);
    vkDestroyShaderModule(_device, computeShader, nullptr);
    vkDestroyShaderModule(_device, temporalShader, nullptr);
    vkDestroyShaderModule(_device, filterShader, nullptr);
    vkDestroyShaderModule(_device, compositeFragmentShader, nullptr);
    vkDestroyShaderModule(_device, compositeVertexShader, nullptr);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _ssgiPipeline, nullptr);
        vkDestroyPipeline(_device, _ssgiTemporalPipeline, nullptr);
        vkDestroyPipelineLayout(_device, _ssgiPipelineLayout, nullptr);
        vkDestroyPipeline(_device, _ssgiFilterPipeline, nullptr);
        vkDestroyPipelineLayout(_device, _ssgiFilterPipelineLayout, nullptr);
        vkDestroyPipeline(_device, _ssgiCompositePipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(
            _device, _ssgiCompositePipeline.layout, nullptr);
        vkDestroyPipeline(_device, _ssgiPortalMaskPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(
            _device, _ssgiPortalMaskPipeline.layout, nullptr);
    });
}

void VulkanEngine::init_post_process_resources()
{
    // A filter cannot read and write one image in the same pass, so the
    // composed frame is read from _drawImage and resolved into this one.  It
    // matches the draw image exactly, including being allocated at window size
    // and only partly used when the resolution scale is below 1.
    // Both post-process targets carry the swapchain's format rather than the
    // draw image's.  By the time the frame reaches them the tonemap has
    // already reduced it to display range, so the extra twelve bits per pixel
    // of the HDR format would store nothing, and matching the swapchain lets
    // the final blit copy rather than convert.
    _postProcessImage = create_image(
        _drawImage.imageExtent,
        _swapchainImageFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    // The tonemap's own target.  It is separate from _postProcessImage for
    // the same reason that one is separate from _drawImage: anti-aliasing
    // reads this and writes that, and no pass may do both to one image.
    _tonemapImage = create_image(
        _drawImage.imageExtent,
        _swapchainImageFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    // Linear, unlike the prepass sampler: the whole point of the final tap is
    // to land between two texels and let the hardware mix them.  Clamped
    // because a filter kernel always reaches past the image at its border.
    VkSamplerCreateInfo samplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(_device, &samplerInfo, nullptr, &_postProcessSampler));

    _mainDeletionQueue.push_function([this]() {
        vkDestroySampler(_device, _postProcessSampler, nullptr);
        destroy_image(_tonemapImage);
        destroy_image(_postProcessImage);
    });
}

void VulkanEngine::init_tonemap_pipeline()
{
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    // The same fullscreen triangle the anti-aliasing pass uses.  It binds
    // nothing and interpolates nothing, so there is no reason for a second
    // copy of it.
    if (!vkutil::load_shader_module(
            "../../shaders/fxaa.vert.spv", _device, &vertexShader) ||
        !vkutil::load_shader_module(
            "../../shaders/tonemap.frag.spv", _device, &fragmentShader)) {
        fmt::print("Error loading tonemap shaders\n");
        if (vertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, vertexShader, nullptr);
        }
        if (fragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, fragmentShader, nullptr);
        }
        return;
    }

    VkPushConstantRange settingsRange{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(TonemapPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &_singleImageDescriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &settingsRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &layoutInfo, nullptr, &_tonemapPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _tonemapPipeline.layout;
    builder.set_shaders(vertexShader, fragmentShader);
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    builder.disable_blending();
    builder.disable_depthtest();
    builder.set_color_attachment_format(_tonemapImage.imageFormat);
    builder.set_depth_format(VK_FORMAT_UNDEFINED);
    _tonemapPipeline.pipeline = builder.build_pipeline(_device);

    vkDestroyShaderModule(_device, fragmentShader, nullptr);
    vkDestroyShaderModule(_device, vertexShader, nullptr);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _tonemapPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _tonemapPipeline.layout, nullptr);
    });
}

void VulkanEngine::init_fxaa_pipeline()
{
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module(
            "../../shaders/fxaa.vert.spv", _device, &vertexShader) ||
        !vkutil::load_shader_module(
            "../../shaders/fxaa.frag.spv", _device, &fragmentShader)) {
        fmt::print("Error loading FXAA shaders\n");
        if (vertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, vertexShader, nullptr);
        }
        if (fragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, fragmentShader, nullptr);
        }
        return;
    }

    // One sampled image and one push constant block.  The filter works purely
    // in screen space, so it needs neither scene data nor a camera.
    VkPushConstantRange settingsRange{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(FXAAPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &_singleImageDescriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &settingsRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &layoutInfo, nullptr, &_fxaaPipeline.layout));

    PipelineBuilder builder;
    builder._pipelineLayout = _fxaaPipeline.layout;
    builder.set_shaders(vertexShader, fragmentShader);
    builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    builder.set_multisampling_none();
    // It writes every pixel of its target from scratch, so there is nothing
    // to blend against and no depth to test.
    builder.disable_blending();
    builder.disable_depthtest();
    builder.set_color_attachment_format(_postProcessImage.imageFormat);
    builder.set_depth_format(VK_FORMAT_UNDEFINED);
    _fxaaPipeline.pipeline = builder.build_pipeline(_device);

    vkDestroyShaderModule(_device, fragmentShader, nullptr);
    vkDestroyShaderModule(_device, vertexShader, nullptr);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _fxaaPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(_device, _fxaaPipeline.layout, nullptr);
    });
}

void VulkanEngine::draw_depth_normal_prepass(VkCommandBuffer cmd)
{
    const auto startTime = std::chrono::steady_clock::now();

    vkutil::transition_image(
        cmd,
        _prepassNormalImage.image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkutil::transition_image(
        cmd,
        _prepassDepthImage.image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    // A zero normal marks a pixel no surface covered, which is how the debug
    // views and later the occlusion pass recognise the background.
    VkClearValue normalClear{};
    normalClear.color.float32[0] = 0.0f;
    normalClear.color.float32[1] = 0.0f;
    normalClear.color.float32[2] = 0.0f;
    normalClear.color.float32[3] = 0.0f;
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _prepassNormalImage.imageView,
        &normalClear,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    // depth_attachment_info already clears to 0, the far value under the main
    // camera's reversed depth. This pass shares that convention deliberately,
    // unlike the shadow map, so its depth could later replace the main one.
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
        _prepassDepthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, &depthAttachment);
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
    // No stencil attachment here, but the shared builder makes stencil state
    // dynamic on every pipeline it produces.
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0x00);

    if (_depthNormalPipeline.pipeline != VK_NULL_HANDLE) {
        // Both prepass layouts declare the scene set at index 0 with the same
        // push constant range, which makes them compatible for that set: it
        // survives a switch between the two pipelines and is bound once.
        vkCmdBindPipeline(
            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _depthNormalPipeline.pipeline);
        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            _depthNormalPipeline.layout,
            0,
            1,
            &get_current_frame().sceneDescriptor,
            0,
            nullptr);

        VkPipeline lastPipeline = _depthNormalPipeline.pipeline;
        MaterialInstance* lastMaterial = nullptr;
        VkBuffer lastIndexBuffer = VK_NULL_HANDLE;
        // mainDrawContext and the main camera's frustum test, because these
        // buffers must describe the image the player is actually looking at.
        // Transparent surfaces are left out for now: one depth and one normal
        // per pixel cannot describe a surface seen through another.  Masked
        // surfaces are not in that category - they are opaque wherever they
        // draw at all - so they belong here, drawn through a pipeline that
        // runs their cutoff test.
        for (const RenderObject& renderObject : mainDrawContext.OpaqueSurfaces) {
            if (!is_visible(renderObject, sceneData.viewproj)) {
                continue;
            }

            const bool masked = renderObject.material != nullptr &&
                renderObject.material->passType == MaterialPass::Mask &&
                _depthNormalMaskPipeline.pipeline != VK_NULL_HANDLE;
            const MaterialPipeline& active = masked
                ? _depthNormalMaskPipeline
                : _depthNormalPipeline;
            if (active.pipeline != lastPipeline) {
                lastPipeline = active.pipeline;
                vkCmdBindPipeline(
                    cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, active.pipeline);
            }
            if (masked && renderObject.material != lastMaterial) {
                lastMaterial = renderObject.material;
                vkCmdBindDescriptorSets(
                    cmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    active.layout,
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
                active.layout,
                VK_SHADER_STAGE_VERTEX_BIT,
                0,
                sizeof(GPUDrawPushConstants),
                &pushConstants);
            vkCmdDrawIndexed(
                cmd, renderObject.indexCount, 1, renderObject.firstIndex, 0, 0);

            ++stats.prepass_drawcall_count;
            stats.prepass_triangle_count +=
                static_cast<int>(renderObject.indexCount / 3);
        }
    }

    vkCmdEndRendering(cmd);

    vkutil::transition_image(
        cmd,
        _prepassNormalImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkutil::transition_image(
        cmd,
        _prepassDepthImage.image,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    stats.prepass_record_time = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - startTime).count();
}

void VulkanEngine::draw_render_debug(VkCommandBuffer cmd)
{
    if (_renderDebugPipeline.pipeline == VK_NULL_HANDLE) {
        return;
    }

    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, nullptr);
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
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0x00);

    vkCmdBindPipeline(
        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _renderDebugPipeline.pipeline);
    const std::array<VkDescriptorSet, 4> sets{
        get_current_frame().sceneDescriptor,
        _prepassImageDescriptor,
        _ssaoDebugDescriptor,
        _ssgiDebugDescriptors[1u - _ssgiHistoryWriteIndex]};
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        _renderDebugPipeline.layout,
        0,
        static_cast<uint32_t>(sets.size()),
        sets.data(),
        0,
        nullptr);

    RenderDebugPushConstants pushConstants{};
    pushConstants.settings = glm::vec4(
        static_cast<float>(static_cast<int>(_renderDebugView)),
        static_cast<float>(_drawExtent.width),
        static_cast<float>(_drawExtent.height),
        _renderDebugDepthRange);
    vkCmdPushConstants(
        cmd,
        _renderDebugPipeline.layout,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(RenderDebugPushConstants),
        &pushConstants);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);
}

void VulkanEngine::draw_ssgi(VkCommandBuffer cmd)
{
    // The forward MRT outputs become read-only inputs for both SSGI and the
    // debug views. Direct lighting is copied into a ping-pong history so the
    // march never samples an image being rendered this frame.
    vkutil::transition_image(
        cmd, _gbufferAlbedoImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkutil::transition_image(
        cmd, _gbufferVelocityImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkutil::transition_image(
        cmd, _directLightingImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    const VkExtent2D ssgiExtent = active_ssgi_extent();
    if (_ssgiHistoryExtent.width != ssgiExtent.width ||
        _ssgiHistoryExtent.height != ssgiExtent.height) {
        _ssgiHistoryValid = false;
        _ssgiHistoryExtent = ssgiExtent;
    }

    const uint32_t writeIndex = _ssgiHistoryWriteIndex;
    const uint32_t readIndex = 1u - writeIndex;
    vkutil::transition_image(
        cmd, _directLightingHistory[writeIndex].image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkutil::copy_image_to_image(
        cmd,
        _directLightingImage.image,
        _directLightingHistory[writeIndex].image,
        _drawExtent,
        _drawExtent);
    vkutil::transition_image(
        cmd, _directLightingHistory[writeIndex].image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkutil::transition_image(
        cmd, _directLightingImage.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    const bool debugRequestsSSGI =
        _renderDebugView == RenderDebugView::SSGIRaw ||
        _renderDebugView == RenderDebugView::SSGIHitMiss ||
        _renderDebugView == RenderDebugView::SSGISteps ||
        _renderDebugView == RenderDebugView::SSGITemporal ||
        _renderDebugView == RenderDebugView::SSGIHistoryRejection ||
        _renderDebugView == RenderDebugView::SSGIReprojection ||
        _renderDebugView == RenderDebugView::SSGIFiltered ||
        _renderDebugView == RenderDebugView::SSGIFallback ||
        _renderDebugView == RenderDebugView::SSGIReferenceDifference;
    const bool runSSGI = (_ssgiEnabled || debugRequestsSSGI) &&
        _ssgiPipeline != VK_NULL_HANDLE &&
        _ssgiTemporalPipeline != VK_NULL_HANDLE;
    if (runSSGI) {
        const uint32_t timingBase =
            (_frameNumber % FRAME_OVERLAP) * SSGITimestampsPerFrame;
        if (_gpuTimingSupported) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                _ssgiTimestampPool, timingBase + 0);
        }
        // Same-layout barriers make the preceding frame's fragment reads and
        // this dispatch's writes explicit without changing descriptor state.
        vkutil::transition_image(
            cmd, _ssgiRawImage.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiDebugImage.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiFallbackImage.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL);

        vkutil::transition_image(
            cmd, _ssgiTemporalHistory[writeIndex].image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiMetadataHistory[writeIndex].image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiTemporalDiagnosticImage.image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);

        vkCmdBindPipeline(
            cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgiPipeline);
        const std::array<VkDescriptorSet, 2> sets{
            get_current_frame().sceneDescriptor,
            _ssgiDescriptors[writeIndex]};
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgiPipelineLayout,
            0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        SSGIPushConstants pushConstants{};
        pushConstants.control = glm::uvec4(
            ssgiExtent.width,
            ssgiExtent.height,
            _ssgiHistoryValid ? 1u : 0u,
            static_cast<uint32_t>(_frameNumber));
        pushConstants.settings = glm::vec4(
            _ssgiRayLength,
            _ssgiThickness,
            _ssgiStartOffset,
            static_cast<float>(_ssgiStepCount));
        pushConstants.temporal = glm::vec4(
            _ssgiHistoryWeight,
            _ssgiDepthRejection,
            _ssgiNormalRejection,
            _ssgiVelocityRejection);
        pushConstants.quality = glm::uvec4(
            static_cast<uint32_t>(std::clamp(_ssgiRaysPerPixel, 1, 8)),
            0u, 0u, 0u);
        vkCmdPushConstants(
            cmd, _ssgiPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(pushConstants), &pushConstants);
        vkCmdDispatch(
            cmd,
            (ssgiExtent.width + 7u) / 8u,
            (ssgiExtent.height + 7u) / 8u,
            1);
        if (_gpuTimingSupported) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                _ssgiTimestampPool, timingBase + 1);
        }
        vkutil::transition_image(
            cmd, _ssgiRawImage.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiDebugImage.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiFallbackImage.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL);

        // Temporal accumulation is a separate dispatch so every invocation
        // sees the complete current-frame raw image. This makes the 3x3
        // neighborhood clamp deterministic across workgroups.
        vkCmdBindPipeline(
            cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgiTemporalPipeline);
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgiPipelineLayout,
            0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        vkCmdPushConstants(
            cmd, _ssgiPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(pushConstants), &pushConstants);
        vkCmdDispatch(
            cmd,
            (ssgiExtent.width + 7u) / 8u,
            (ssgiExtent.height + 7u) / 8u,
            1);
        if (_gpuTimingSupported) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                _ssgiTimestampPool, timingBase + 2);
        }
        vkutil::transition_image(
            cmd, _ssgiTemporalHistory[writeIndex].image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiMetadataHistory[writeIndex].image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(
            cmd, _ssgiTemporalDiagnosticImage.image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);

        if (_ssgiFilterPipeline != VK_NULL_HANDLE) {
            vkCmdBindPipeline(
                cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgiFilterPipeline);
            SSGIFilterPushConstants filterPush{};
            filterPush.settings = glm::vec4(
                static_cast<float>(_ssgiFilterRadius),
                _ssgiFilterDepthFalloff,
                _ssgiFilterNormalPower,
                _ssgiSpatialFilterEnabled ? 1.0f : 0.0f);
            const uint32_t groupX = (ssgiExtent.width + 7u) / 8u;
            const uint32_t groupY = (ssgiExtent.height + 7u) / 8u;
            filterPush.control = glm::ivec4(
                static_cast<int>(ssgiExtent.width),
                static_cast<int>(ssgiExtent.height), 1, 0);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                _ssgiFilterPipelineLayout, 0, 1,
                &_ssgiFilterDescriptors[writeIndex], 0, nullptr);
            vkCmdPushConstants(cmd, _ssgiFilterPipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(filterPush), &filterPush);
            vkCmdDispatch(cmd, groupX, groupY, 1);
            vkutil::transition_image(cmd, _ssgiFilterScratchImage.image,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);

            filterPush.control.z = 0;
            filterPush.control.w = 1;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                _ssgiFilterPipelineLayout, 0, 1,
                &_ssgiFilterDescriptors[2], 0, nullptr);
            vkCmdPushConstants(cmd, _ssgiFilterPipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(filterPush), &filterPush);
            vkCmdDispatch(cmd, groupX, groupY, 1);
            vkutil::transition_image(cmd, _ssgiFilteredImage.image,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        }
        if (_gpuTimingSupported) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                _ssgiTimestampPool, timingBase + 3);
        }

        _ssgiHistoryValid = true;
        _ssgiHistoryWriteIndex = readIndex;
    } else {
        // Enabling the effect later must not blend history from before it was
        // disabled, when neither geometry nor camera continuity was tracked.
        _ssgiHistoryValid = false;
    }
}

void VulkanEngine::draw_ssgi_composite(VkCommandBuffer cmd)
{
    if (!_ssgiEnabled || _ssgiCompositePipeline.pipeline == VK_NULL_HANDLE) {
        return;
    }

    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _drawImage.imageView, nullptr,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, nullptr);
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
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0x00);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        _ssgiCompositePipeline.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        _ssgiCompositePipeline.layout, 0, 1,
        &_ssgiCompositeDescriptors[1u - _ssgiHistoryWriteIndex], 0, nullptr);
    SSGICompositePushConstants push{};
    const VkExtent2D ssgiExtent = active_ssgi_extent();
    push.extents = glm::vec4(
        static_cast<float>(_drawExtent.width),
        static_cast<float>(_drawExtent.height),
        static_cast<float>(ssgiExtent.width),
        static_cast<float>(ssgiExtent.height));
    push.settings = glm::vec4(
        _ssgiIntensity, _ssgiHalfResolution ? 1.0f : 0.0f, 0.0f, 0.0f);
    vkCmdPushConstants(cmd, _ssgiCompositePipeline.layout,
        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    if (_gpuTimingSupported) {
        const uint32_t timingBase =
            (_frameNumber % FRAME_OVERLAP) * SSGITimestampsPerFrame;
        vkCmdWriteTimestamp2(cmd,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            _ssgiTimestampPool, timingBase + 4);
        _ssgiTimingWritten[_frameNumber % FRAME_OVERLAP] = true;
    }
}

VkExtent2D VulkanEngine::active_ssgi_extent() const
{
    if (!_ssgiHalfResolution) {
        return _drawExtent;
    }
    return VkExtent2D{
        std::max(1u, (_drawExtent.width + 1u) / 2u),
        std::max(1u, (_drawExtent.height + 1u) / 2u)};
}

void VulkanEngine::draw_tonemap(VkCommandBuffer cmd)
{
    // The composed linear frame becomes a texture and the pass resolves it
    // into the display-range image beside it.  Everything downstream of here
    // - anti-aliasing, the blit, ImGui - works in that range.
    vkutil::transition_image(
        cmd,
        _drawImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkutil::transition_image(
        cmd,
        _tonemapImage.image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _tonemapImage.imageView,
        nullptr,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, nullptr);
    vkCmdBeginRendering(cmd, &renderInfo);

    // _drawExtent rather than the allocation, for the same reason the
    // anti-aliasing pass uses it: below a resolution scale of 1 the rest of
    // the image holds nothing this frame produced.
    VkViewport viewport{};
    viewport.width = static_cast<float>(_drawExtent.width);
    viewport.height = static_cast<float>(_drawExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = _drawExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    // No stencil attachment here either, but the shared builder makes stencil
    // state dynamic on every pipeline it produces.
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0x00);

    vkCmdBindPipeline(
        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _tonemapPipeline.pipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        _tonemapPipeline.layout,
        0,
        1,
        &_tonemapInputDescriptor,
        0,
        nullptr);

    TonemapPushConstants pushConstants{};
    pushConstants.settings = glm::vec4(
        _tonemapExposure,
        _tonemapOperator == 1 ? 1.0f : 0.0f,
        _tonemapBypassCurve ? 1.0f : 0.0f,
        0.0f);
    vkCmdPushConstants(
        cmd,
        _tonemapPipeline.layout,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(TonemapPushConstants),
        &pushConstants);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);

    // Left readable rather than transfer-ready: anti-aliasing may still want
    // it as a texture.  draw() transitions it for the blit if it does not.
    vkutil::transition_image(
        cmd,
        _tonemapImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanEngine::draw_fxaa(VkCommandBuffer cmd)
{
    // Its source is the tonemap's output, which that pass already left in a
    // readable layout, so only the target needs transitioning here.  Reading
    // and writing one image within a single draw has no defined result, which
    // is why the pair exists.
    vkutil::transition_image(
        cmd,
        _postProcessImage.image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
        _postProcessImage.imageView,
        nullptr,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo = vkinit::rendering_info(
        _drawExtent, &colorAttachment, nullptr);
    vkCmdBeginRendering(cmd, &renderInfo);

    // _drawExtent, not the allocation: at a resolution scale below 1 the rest
    // of the image holds nothing this frame rendered, and the copy to the
    // swapchain reads only this region anyway.
    VkViewport viewport{};
    viewport.width = static_cast<float>(_drawExtent.width);
    viewport.height = static_cast<float>(_drawExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = _drawExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    // No stencil attachment here, but the shared builder makes stencil state
    // dynamic on every pipeline it produces.
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0x00);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _fxaaPipeline.pipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        _fxaaPipeline.layout,
        0,
        1,
        &_fxaaInputDescriptor,
        0,
        nullptr);

    // The texel step is one pixel of the allocation, because that is what a
    // UV offset of one pixel means in this texture.  The limit is the last
    // pixel centre the render scale actually filled, so a search along a long
    // edge stops at the live region instead of sampling last frame's leftover.
    const glm::vec2 allocationSize(
        static_cast<float>(_drawImage.imageExtent.width),
        static_cast<float>(_drawImage.imageExtent.height));
    const glm::vec2 texelSize = 1.0f / allocationSize;
    const glm::vec2 renderedLimit = glm::vec2(
        (static_cast<float>(_drawExtent.width) - 0.5f) / allocationSize.x,
        (static_cast<float>(_drawExtent.height) - 0.5f) / allocationSize.y);

    FXAAPushConstants pushConstants{};
    pushConstants.settings = glm::vec4(
        texelSize.x, texelSize.y, _fxaaEdgeThreshold, _fxaaSubpixelStrength);
    pushConstants.limits = glm::vec4(
        renderedLimit.x, renderedLimit.y, _fxaaShowEdges ? 1.0f : 0.0f, 0.0f);
    vkCmdPushConstants(
        cmd,
        _fxaaPipeline.layout,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(FXAAPushConstants),
        &pushConstants);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);

    vkutil::transition_image(
        cmd,
        _postProcessImage.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}

VkExtent2D VulkanEngine::active_ssao_extent() const
{
    // Rounded up, so an odd rendered width still has a column of occlusion
    // pixels covering its last column of geometry.
    return VkExtent2D{
        std::max(1u, (_drawExtent.width + 1) / 2),
        std::max(1u, (_drawExtent.height + 1) / 2)};
}

bool VulkanEngine::ssao_active() const
{
    // Every pipeline has to have been built, and the whole effect reads the
    // prepass, so a device that failed to produce either leaves it off rather
    // than shading against an image nothing wrote.
    return _ssaoSettings.enabled &&
        _ssaoBlurPipeline != VK_NULL_HANDLE &&
        _ssaoPipelines[_ssaoQuality] != VK_NULL_HANDLE &&
        _depthNormalPipeline.pipeline != VK_NULL_HANDLE;
}

void VulkanEngine::init_ssao_resources()
{
    // The occlusion images are both written by a compute shader and read by
    // one, so storage and sampled use are both required and neither is
    // guaranteed for a given format.  R8 is the cheapest that can hold a
    // visibility fraction; the wider candidates exist for devices that do not
    // advertise storage support for it.
    static constexpr std::array<VkFormat, 3> candidates{
        VK_FORMAT_R8_UNORM,
        VK_FORMAT_R16_SFLOAT,
        VK_FORMAT_R16G16_SFLOAT};
    constexpr VkFormatFeatureFlags2 requiredFeatures =
        VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    for (VkFormat candidate : candidates) {
        VkFormatProperties3 properties3{
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 properties2{
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
            .pNext = &properties3};
        vkGetPhysicalDeviceFormatProperties2(
            _chosenGPU, candidate, &properties2);
        if ((properties3.optimalTilingFeatures & requiredFeatures) ==
            requiredFeatures) {
            _ssaoFormat = candidate;
            break;
        }
    }
    if (_ssaoFormat == VK_FORMAT_UNDEFINED) {
        // Reported rather than fatal: the rest of the renderer works without
        // ambient occlusion, and the flag in the scene data already makes
        // every material shade as if nothing were occluding it.
        fmt::print("No format supports a storage-plus-sampled occlusion image; "
                   "ambient occlusion disabled\n");
        _ssaoSettings.enabled = false;
        return;
    }
    _ssaoFormatName = string_VkFormat(_ssaoFormat);

    // Half of the allocation rather than of the current render scale, so
    // moving the resolution slider never reallocates any of this.
    _ssaoExtent = VkExtent2D{
        std::max(1u, (_drawImage.imageExtent.width + 1) / 2),
        std::max(1u, (_drawImage.imageExtent.height + 1) / 2)};
    const VkExtent3D ssaoExtent3D{_ssaoExtent.width, _ssaoExtent.height, 1};
    constexpr VkImageUsageFlags ssaoUsage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    _ssaoRawImage = create_image(ssaoExtent3D, _ssaoFormat, ssaoUsage);
    _ssaoBlurImage = create_image(ssaoExtent3D, _ssaoFormat, ssaoUsage);
    _ssaoFinalImage = create_image(ssaoExtent3D, _ssaoFormat, ssaoUsage);

    // Fully visible is the neutral value, so a frame that never ran the
    // dispatches - the first one, or any frame with the effect off - shades
    // exactly as it would with no ambient occlusion at all.
    immediate_submit([this](VkCommandBuffer cmd) {
        VkClearColorValue white{};
        white.float32[0] = 1.0f;
        white.float32[1] = 1.0f;
        white.float32[2] = 1.0f;
        white.float32[3] = 1.0f;
        VkImageSubresourceRange range =
            vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
        for (AllocatedImage* image :
             {&_ssaoRawImage, &_ssaoBlurImage, &_ssaoFinalImage}) {
            vkutil::transition_image(
                cmd,
                image->image,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(
                cmd,
                image->image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                &white,
                1,
                &range);
            vkutil::transition_image(
                cmd,
                image->image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });

    // A fixed seed, so the pattern is identical from run to run.  Captures
    // stay comparable and a change in the image is a change in the code.
    std::mt19937 generator(0x5EED5A0Bu);
    std::uniform_real_distribution<float> zeroToOne(0.0f, 1.0f);
    const auto signedRandom = [&]() { return zeroToOne(generator) * 2.0f - 1.0f; };

    // Every quality level reads a prefix of this one kernel, so the position
    // of an entry within it decides which qualities ever see that entry.
    // Taking the distance from the index straight through would give the
    // first 32 entries only the nearest third of the range, and Medium would
    // sample a radius a third of the one it was asked for.  Reversing the
    // bits spreads any power-of-two prefix across the whole range instead.
    const auto reversedFraction = [](uint32_t index) {
        uint32_t reversed = 0;
        for (int bit = 0; bit < 6; ++bit) {
            reversed = (reversed << 1) | ((index >> bit) & 1u);
        }
        return static_cast<float>(reversed) /
            static_cast<float>(MaxSSAOKernelSize);
    };

    SSAOKernelBlock kernel{};
    for (uint32_t i = 0; i < MaxSSAOKernelSize; ++i) {
        glm::vec3 point(signedRandom(), signedRandom(), zeroToOne(generator));
        // A point on the unit hemisphere, then pulled inside it.
        point = glm::normalize(point) * zeroToOne(generator);
        // Squared interpolation still crowds the kernel towards the surface.
        // Contact occlusion is a close-range effect, so most of the samples
        // are worth spending near it - just not all of them.
        const float t = reversedFraction(i);
        point *= 0.1f + 0.9f * t * t;
        kernel.samples[i] = glm::vec4(point, 0.0f);
    }
    // Raising the sample count therefore refines the estimate rather than
    // replacing it, and every quality covers the same radius.
    _ssaoKernelBuffer = create_buffer(
        sizeof(SSAOKernelBlock),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    std::memcpy(
        _ssaoKernelBuffer.info.pMappedData, &kernel, sizeof(SSAOKernelBlock));

    // Rotation vectors in the tangent plane, stored unsigned and decoded in
    // the shader because the upload helper works in four bytes per pixel.  A
    // larger tile keeps its repetition from surviving the bilateral blur as
    // regularly spaced bands on broad, flat surfaces.
    constexpr uint32_t NoiseSize = 16;
    constexpr float TwoPi = 6.28318530718f;
    std::uniform_real_distribution<float> randomAngle(0.0f, TwoPi);
    std::array<uint32_t, NoiseSize * NoiseSize> noisePixels{};
    for (uint32_t& pixel : noisePixels) {
        // Sampling an angle gives every rotation equal probability.  Picking
        // x and y independently from a square would favour its diagonals.
        const float angle = randomAngle(generator);
        const glm::vec2 direction(std::cos(angle), std::sin(angle));
        const glm::vec2 encoded = direction * 0.5f + 0.5f;
        pixel = glm::packUnorm4x8(glm::vec4(encoded.x, encoded.y, 0.5f, 1.0f));
    }
    _ssaoNoiseImage = create_image(
        noisePixels.data(),
        VkExtent3D{NoiseSize, NoiseSize, 1},
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT);

    // Nearest and repeating: the point is a hard 16x16 tile of distinct
    // rotations, and filtering between them would average the noise away.
    VkSamplerCreateInfo noiseSamplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    noiseSamplerInfo.magFilter = VK_FILTER_NEAREST;
    noiseSamplerInfo.minFilter = VK_FILTER_NEAREST;
    noiseSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    noiseSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    noiseSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    noiseSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    noiseSamplerInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(
        _device, &noiseSamplerInfo, nullptr, &_ssaoNoiseSampler));

    // Linear and clamped, for the half-to-full resolution step in material
    // shading.  The bilateral blur has already preserved the edges, so a plain
    // bilinear lift is enough to start with.
    VkSamplerCreateInfo ssaoSamplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ssaoSamplerInfo.magFilter = VK_FILTER_LINEAR;
    ssaoSamplerInfo.minFilter = VK_FILTER_LINEAR;
    ssaoSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ssaoSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ssaoSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ssaoSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ssaoSamplerInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(_device, &ssaoSamplerInfo, nullptr, &_ssaoSampler));

    _mainDeletionQueue.push_function([this]() {
        vkDestroySampler(_device, _ssaoSampler, nullptr);
        vkDestroySampler(_device, _ssaoNoiseSampler, nullptr);
        destroy_image(_ssaoNoiseImage);
        destroy_buffer(_ssaoKernelBuffer);
        destroy_image(_ssaoFinalImage);
        destroy_image(_ssaoBlurImage);
        destroy_image(_ssaoRawImage);
    });
}

void VulkanEngine::init_ssao_pipelines()
{
    if (_ssaoFormat == VK_FORMAT_UNDEFINED) {
        return;
    }

    VkShaderModule ssaoShader = VK_NULL_HANDLE;
    VkShaderModule blurShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module(
            "../../shaders/ssao.comp.spv", _device, &ssaoShader) ||
        !vkutil::load_shader_module(
            "../../shaders/ssao_blur.comp.spv", _device, &blurShader)) {
        fmt::print("Error loading ambient occlusion shaders\n");
        if (ssaoShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, ssaoShader, nullptr);
        }
        if (blurShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, blurShader, nullptr);
        }
        return;
    }

    // Set 0 is the camera block, which both passes need for the projection
    // and its inverse.  Set 1 holds the images belonging to the pass.
    VkPushConstantRange pushRange{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(SSAOPushConstants)};

    const std::array<VkDescriptorSetLayout, 2> ssaoLayouts{
        _gpuSceneDataDescriptorLayout, _ssaoDescriptorLayout};
    VkPipelineLayoutCreateInfo ssaoLayoutInfo =
        vkinit::pipeline_layout_create_info();
    ssaoLayoutInfo.setLayoutCount = static_cast<uint32_t>(ssaoLayouts.size());
    ssaoLayoutInfo.pSetLayouts = ssaoLayouts.data();
    ssaoLayoutInfo.pushConstantRangeCount = 1;
    ssaoLayoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &ssaoLayoutInfo, nullptr, &_ssaoPipelineLayout));

    const std::array<VkDescriptorSetLayout, 2> blurLayouts{
        _gpuSceneDataDescriptorLayout, _ssaoBlurDescriptorLayout};
    VkPipelineLayoutCreateInfo blurLayoutInfo =
        vkinit::pipeline_layout_create_info();
    blurLayoutInfo.setLayoutCount = static_cast<uint32_t>(blurLayouts.size());
    blurLayoutInfo.pSetLayouts = blurLayouts.data();
    blurLayoutInfo.pushConstantRangeCount = 1;
    blurLayoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(
        _device, &blurLayoutInfo, nullptr, &_ssaoBlurPipelineLayout));

    // One pipeline per sample count.  The count is a specialization constant,
    // so the compiler sees a fixed loop bound and a lower quality setting
    // genuinely issues fewer texture fetches rather than skipping iterations.
    VkSpecializationMapEntry kernelEntry{
        .constantID = 0, .offset = 0, .size = sizeof(int32_t)};
    for (size_t quality = 0; quality < SSAOKernelSizes.size(); ++quality) {
        const int32_t kernelSize = SSAOKernelSizes[quality];
        VkSpecializationInfo specialization{
            .mapEntryCount = 1,
            .pMapEntries = &kernelEntry,
            .dataSize = sizeof(kernelSize),
            .pData = &kernelSize};

        VkPipelineShaderStageCreateInfo stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = ssaoShader;
        stage.pName = "main";
        stage.pSpecializationInfo = &specialization;

        VkComputePipelineCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        createInfo.layout = _ssaoPipelineLayout;
        createInfo.stage = stage;
        VK_CHECK(vkCreateComputePipelines(
            _device,
            VK_NULL_HANDLE,
            1,
            &createInfo,
            nullptr,
            &_ssaoPipelines[quality]));
    }

    // Both blur directions are the same pipeline; only the tap direction in
    // the push constants and the bound images differ.
    VkPipelineShaderStageCreateInfo blurStage{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    blurStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    blurStage.module = blurShader;
    blurStage.pName = "main";
    VkComputePipelineCreateInfo blurCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    blurCreateInfo.layout = _ssaoBlurPipelineLayout;
    blurCreateInfo.stage = blurStage;
    VK_CHECK(vkCreateComputePipelines(
        _device,
        VK_NULL_HANDLE,
        1,
        &blurCreateInfo,
        nullptr,
        &_ssaoBlurPipeline));

    vkDestroyShaderModule(_device, blurShader, nullptr);
    vkDestroyShaderModule(_device, ssaoShader, nullptr);

    _mainDeletionQueue.push_function([this]() {
        vkDestroyPipeline(_device, _ssaoBlurPipeline, nullptr);
        for (VkPipeline pipeline : _ssaoPipelines) {
            vkDestroyPipeline(_device, pipeline, nullptr);
        }
        vkDestroyPipelineLayout(_device, _ssaoBlurPipelineLayout, nullptr);
        vkDestroyPipelineLayout(_device, _ssaoPipelineLayout, nullptr);
    });
}

SSAOPushConstants VulkanEngine::build_ssao_push_constants() const
{
    const VkExtent2D activeExtent = active_ssao_extent();
    const glm::vec2 ssaoAllocation(
        static_cast<float>(_ssaoExtent.width),
        static_cast<float>(_ssaoExtent.height));
    const glm::vec2 prepassAllocation(
        static_cast<float>(_drawImage.imageExtent.width),
        static_cast<float>(_drawImage.imageExtent.height));

    SSAOPushConstants push{};
    push.extents = glm::ivec4(
        static_cast<int>(activeExtent.width),
        static_cast<int>(activeExtent.height),
        0,
        0);
    push.texelSize = glm::vec4(
        1.0f / ssaoAllocation.x,
        1.0f / ssaoAllocation.y,
        1.0f / prepassAllocation.x,
        1.0f / prepassAllocation.y);
    // What fraction of each allocation this frame's render scale filled.  A
    // screen-space coordinate is scaled by these to reach a live texel.
    push.activeFraction = glm::vec4(
        static_cast<float>(activeExtent.width) / ssaoAllocation.x,
        static_cast<float>(activeExtent.height) / ssaoAllocation.y,
        static_cast<float>(_drawExtent.width) / prepassAllocation.x,
        static_cast<float>(_drawExtent.height) / prepassAllocation.y);
    push.screenTexel = glm::vec4(
        1.0f / static_cast<float>(_drawExtent.width),
        1.0f / static_cast<float>(_drawExtent.height),
        0.0f,
        0.0f);
    return push;
}

void VulkanEngine::draw_ssao(VkCommandBuffer cmd)
{
    const VkExtent2D activeExtent = active_ssao_extent();
    const uint32_t groupsX = (activeExtent.width + 7) / 8;
    const uint32_t groupsY = (activeExtent.height + 7) / 8;
    const uint32_t timestampBase =
        (_frameNumber % FRAME_OVERLAP) * TimestampsPerFrame;

    stats.ssao_width = static_cast<int>(activeExtent.width);
    stats.ssao_height = static_cast<int>(activeExtent.height);
    stats.ssao_kernel_samples = SSAOKernelSizes[_ssaoQuality];

    // Each image moves through the same two steps: written by one dispatch,
    // then read by the next.  The barrier between them is what makes the
    // second dispatch see the first one.
    const auto toStorageWrite = [&](const AllocatedImage& image) {
        vkutil::transition_image(
            cmd,
            image.image,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
    };
    const auto toSampledRead = [&](const AllocatedImage& image) {
        vkutil::transition_image(
            cmd,
            image.image,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    };

    SSAOPushConstants push = build_ssao_push_constants();

    if (_gpuTimingSupported) {
        vkCmdWriteTimestamp2(
            cmd,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            _timestampPool,
            timestampBase + 0);
    }

    // Sampling pass: raw, noisy occlusion at half resolution.
    toStorageWrite(_ssaoRawImage);
    vkCmdBindPipeline(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssaoPipelines[_ssaoQuality]);
    const std::array<VkDescriptorSet, 2> ssaoSets{
        get_current_frame().sceneDescriptor, _ssaoDescriptor};
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        _ssaoPipelineLayout,
        0,
        static_cast<uint32_t>(ssaoSets.size()),
        ssaoSets.data(),
        0,
        nullptr);
    push.settings = glm::vec4(
        _ssaoSettings.radius,
        _ssaoSettings.bias,
        _ssaoSettings.power,
        _ssaoSettings.intensity);
    vkCmdPushConstants(
        cmd,
        _ssaoPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(SSAOPushConstants),
        &push);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);
    toSampledRead(_ssaoRawImage);

    if (_gpuTimingSupported) {
        vkCmdWriteTimestamp2(
            cmd,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            _timestampPool,
            timestampBase + 1);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssaoBlurPipeline);
    push.settings =
        glm::vec4(_ssaoDepthFalloff, _ssaoNormalFalloff, 0.0f, 0.0f);

    const auto runBlur = [&](VkDescriptorSet set,
                             const AllocatedImage& target,
                             int directionX,
                             int directionY) {
        toStorageWrite(target);
        const std::array<VkDescriptorSet, 2> blurSets{
            get_current_frame().sceneDescriptor, set};
        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            _ssaoBlurPipelineLayout,
            0,
            static_cast<uint32_t>(blurSets.size()),
            blurSets.data(),
            0,
            nullptr);
        push.extents.z = directionX;
        push.extents.w = directionY;
        vkCmdPushConstants(
            cmd,
            _ssaoBlurPipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            sizeof(SSAOPushConstants),
            &push);
        vkCmdDispatch(cmd, groupsX, groupsY, 1);
        toSampledRead(target);
    };

    runBlur(_ssaoBlurHorizontalDescriptor, _ssaoBlurImage, 1, 0);
    if (_gpuTimingSupported) {
        vkCmdWriteTimestamp2(
            cmd,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            _timestampPool,
            timestampBase + 2);
    }

    runBlur(_ssaoBlurVerticalDescriptor, _ssaoFinalImage, 0, 1);
    if (_gpuTimingSupported) {
        vkCmdWriteTimestamp2(
            cmd,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            _timestampPool,
            timestampBase + 3);
        _timestampsPending[_frameNumber % FRAME_OVERLAP] = true;
    }
}

void VulkanEngine::init_gpu_timestamps()
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(_chosenGPU, &properties);
    // A period of zero means the device records no timestamps at all, and the
    // queue this engine submits to has to be able to take them, which is what
    // timestampComputeAndGraphics reports.
    if (properties.limits.timestampPeriod <= 0.0f ||
        properties.limits.timestampComputeAndGraphics == VK_FALSE) {
        fmt::print("GPU timestamps unavailable; occlusion timings will report "
                   "CPU recording time instead\n");
        return;
    }
    _timestampPeriod = properties.limits.timestampPeriod;

    VkQueryPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    poolInfo.queryCount = FRAME_OVERLAP * TimestampsPerFrame;
    VK_CHECK(vkCreateQueryPool(_device, &poolInfo, nullptr, &_timestampPool));
    poolInfo.queryCount = FRAME_OVERLAP * SSGITimestampsPerFrame;
    VK_CHECK(vkCreateQueryPool(
        _device, &poolInfo, nullptr, &_ssgiTimestampPool));
    _gpuTimingSupported = true;

    _mainDeletionQueue.push_function([this]() {
        vkDestroyQueryPool(_device, _timestampPool, nullptr);
        vkDestroyQueryPool(_device, _ssgiTimestampPool, nullptr);
    });
}

void VulkanEngine::read_gpu_timestamps(uint32_t frameIndex)
{
    // Called only after this frame slot's fence has been waited on, so the
    // results being read belong to a submission that has certainly finished.
    if (!_gpuTimingSupported) {
        return;
    }
    if (_timestampsPending[frameIndex]) {
        _timestampsPending[frameIndex] = false;

    std::array<uint64_t, TimestampsPerFrame> ticks{};
    const VkResult result = vkGetQueryPoolResults(
        _device,
        _timestampPool,
        frameIndex * TimestampsPerFrame,
        TimestampsPerFrame,
        sizeof(ticks),
        ticks.data(),
        sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS) {
        return;
    }

    // timestampPeriod is nanoseconds per tick; the panel reports milliseconds
    // like every other timing beside it.
    const auto toMilliseconds = [this](uint64_t from, uint64_t to) {
        return static_cast<float>(to - from) * _timestampPeriod * 1e-6f;
    };
    stats.ssao_raw_time = toMilliseconds(ticks[0], ticks[1]);
    stats.ssao_blur_horizontal_time = toMilliseconds(ticks[1], ticks[2]);
    stats.ssao_blur_vertical_time = toMilliseconds(ticks[2], ticks[3]);
    stats.ssao_total_time = toMilliseconds(ticks[0], ticks[3]);
    stats.ssao_time_is_gpu = true;
    }

    if (_ssgiTimingWritten[frameIndex]) {
        _ssgiTimingWritten[frameIndex] = false;
        std::array<uint64_t, SSGITimestampsPerFrame> ssgiTicks{};
        const VkResult ssgiResult = vkGetQueryPoolResults(
            _device, _ssgiTimestampPool,
            frameIndex * SSGITimestampsPerFrame,
            SSGITimestampsPerFrame, sizeof(ssgiTicks), ssgiTicks.data(),
            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (ssgiResult == VK_SUCCESS) {
            const auto milliseconds = [&](uint64_t from, uint64_t to) {
                return static_cast<float>(to - from) *
                    _timestampPeriod * 1e-6f;
            };
            stats.ssgi_raw_time = milliseconds(ssgiTicks[0], ssgiTicks[1]);
            stats.ssgi_temporal_time = milliseconds(ssgiTicks[1], ssgiTicks[2]);
            stats.ssgi_filter_time = milliseconds(ssgiTicks[2], ssgiTicks[3]);
            stats.ssgi_composite_time = milliseconds(ssgiTicks[3], ssgiTicks[4]);
            stats.ssgi_total_time = milliseconds(ssgiTicks[0], ssgiTicks[4]);
            stats.ssgi_time_is_gpu = true;
        }
    }
}
