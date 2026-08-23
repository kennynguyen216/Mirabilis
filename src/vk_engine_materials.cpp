#include "vk_engine.h"

#include <vk_initializers.h>
#include <vk_loader.h>
#include <vk_pipelines.h>

void GLTFMetallic_Roughness::build_pipelines(VulkanEngine* engine)
{
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule portalMaskVertexShader = VK_NULL_HANDLE;
    VkShaderModule portalViewVertexShader = VK_NULL_HANDLE;
    VkShaderModule portalViewFragmentShader = VK_NULL_HANDLE;
    VkShaderModule portalCompositeFragmentShader = VK_NULL_HANDLE;
    VkShaderModule portalSkyVertexShader = VK_NULL_HANDLE;
    VkShaderModule portalSkyFragmentShader = VK_NULL_HANDLE;
    VkShaderModule colliderDebugVertexShader = VK_NULL_HANDLE;
    VkShaderModule colliderDebugFragmentShader = VK_NULL_HANDLE;
    if (!vkutil::load_shader_module("../../shaders/mesh.frag.spv", engine->_device, &fragmentShader) ||
        !vkutil::load_shader_module("../../shaders/mesh.vert.spv", engine->_device, &vertexShader) ||
        !vkutil::load_shader_module("../../shaders/portal_mask.vert.spv", engine->_device, &portalMaskVertexShader) ||
        !vkutil::load_shader_module("../../shaders/portal_view.vert.spv", engine->_device, &portalViewVertexShader) ||
        !vkutil::load_shader_module("../../shaders/portal_view.frag.spv", engine->_device, &portalViewFragmentShader) ||
        !vkutil::load_shader_module("../../shaders/portal_composite.frag.spv", engine->_device, &portalCompositeFragmentShader) ||
        !vkutil::load_shader_module("../../shaders/portal_sky.vert.spv", engine->_device, &portalSkyVertexShader) ||
        !vkutil::load_shader_module("../../shaders/portal_sky.frag.spv", engine->_device, &portalSkyFragmentShader) ||
        !vkutil::load_shader_module("../../shaders/collider_debug.vert.spv", engine->_device, &colliderDebugVertexShader) ||
        !vkutil::load_shader_module("../../shaders/collider_debug.frag.spv", engine->_device, &colliderDebugFragmentShader)) {
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
        if (portalCompositeFragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(engine->_device, portalCompositeFragmentShader, nullptr);
        }
        if (portalSkyVertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(engine->_device, portalSkyVertexShader, nullptr);
        }
        if (portalSkyFragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(engine->_device, portalSkyFragmentShader, nullptr);
        }
        if (colliderDebugVertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(engine->_device, colliderDebugVertexShader, nullptr);
        }
        if (colliderDebugFragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(engine->_device, colliderDebugFragmentShader, nullptr);
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
    portalOffscreenPipeline.layout = pipelineLayout;
    portalCompositePipeline.layout = pipelineLayout;

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

    // This pass samples a previously-rendered virtual camera image. Its
    // fragment shader is intentionally unlit, because the source scene was
    // already lit while rendering into that image.
    builder.enable_stenciltest(VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP);
    builder.set_shaders(vertexShader, portalCompositeFragmentShader);
    portalCompositePipeline.pipeline = builder.build_pipeline(engine->_device);

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

    // Same oblique-clipped portal-view shader, but without stencil testing:
    // it renders into a standalone camera target before composition.
    builder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
    portalOffscreenPipeline.pipeline = builder.build_pipeline(engine->_device);

    VkPipelineLayoutCreateInfo portalSkyLayoutInfo = vkinit::pipeline_layout_create_info();
    portalSkyLayoutInfo.setLayoutCount = 1;
    portalSkyLayoutInfo.pSetLayouts = &engine->_singleImageDescriptorLayout;
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

    VkPipelineLayoutCreateInfo colliderDebugLayoutInfo =
        vkinit::pipeline_layout_create_info();
    VkPushConstantRange colliderDebugRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(ColliderDebugPushConstants),
    };
    colliderDebugLayoutInfo.pushConstantRangeCount = 1;
    colliderDebugLayoutInfo.pPushConstantRanges = &colliderDebugRange;
    VK_CHECK(vkCreatePipelineLayout(
        engine->_device,
        &colliderDebugLayoutInfo,
        nullptr,
        &engine->_colliderDebugPipeline.layout));

    PipelineBuilder colliderDebugBuilder;
    colliderDebugBuilder._pipelineLayout = engine->_colliderDebugPipeline.layout;
    colliderDebugBuilder.set_shaders(
        colliderDebugVertexShader, colliderDebugFragmentShader);
    colliderDebugBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
    colliderDebugBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    colliderDebugBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    colliderDebugBuilder.set_multisampling_none();
    colliderDebugBuilder.disable_blending();
    colliderDebugBuilder.enable_depthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);
    colliderDebugBuilder.set_color_attachment_format(engine->_drawImage.imageFormat);
    colliderDebugBuilder.set_depth_format(engine->_depthImage.imageFormat);
    colliderDebugBuilder.set_stencil_format(engine->_depthImage.imageFormat);
    engine->_colliderDebugPipeline.pipeline =
        colliderDebugBuilder.build_pipeline(engine->_device);

    vkDestroyShaderModule(engine->_device, fragmentShader, nullptr);
    vkDestroyShaderModule(engine->_device, vertexShader, nullptr);
    vkDestroyShaderModule(engine->_device, portalMaskVertexShader, nullptr);
    vkDestroyShaderModule(engine->_device, portalViewVertexShader, nullptr);
    vkDestroyShaderModule(engine->_device, portalViewFragmentShader, nullptr);
    vkDestroyShaderModule(engine->_device, portalCompositeFragmentShader, nullptr);
    vkDestroyShaderModule(engine->_device, portalSkyVertexShader, nullptr);
    vkDestroyShaderModule(engine->_device, portalSkyFragmentShader, nullptr);
    vkDestroyShaderModule(engine->_device, colliderDebugVertexShader, nullptr);
    vkDestroyShaderModule(engine->_device, colliderDebugFragmentShader, nullptr);

    engine->_mainDeletionQueue.push_function([this, engine]() {
        vkDestroyPipeline(engine->_device, opaquePipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, transparentPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, portalStencilPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, portalRecursiveStencilPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, portalMaskPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, portalViewPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, portalOffscreenPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, portalCompositePipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, engine->_portalSkyPipeline.pipeline, nullptr);
        vkDestroyPipeline(engine->_device, engine->_colliderDebugPipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(engine->_device, opaquePipeline.layout, nullptr);
        vkDestroyPipelineLayout(engine->_device, engine->_portalSkyPipeline.layout, nullptr);
        vkDestroyPipelineLayout(engine->_device, engine->_colliderDebugPipeline.layout, nullptr);
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
    vkDestroyPipeline(device, portalOffscreenPipeline.pipeline, nullptr);
    vkDestroyPipeline(device, portalCompositePipeline.pipeline, nullptr);
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

