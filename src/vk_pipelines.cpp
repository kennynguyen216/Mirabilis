#include <vk_pipelines.h>
#include <fstream>
#include <vk_initializers.h>

bool vkutil::load_shader_module(const char* filePath,
VkDevice device, 
VkShaderModule* outShaderModule)
{

    //open the file wit cursor at the end
    std::ifstream file(filePath, std::ios::ate | std::ios::binary);

    if(!file.is_open()){
        return false;
    }
    //fid what the size of the file is by looking up locaiton of cursor
    // cursro at end so it gives size in bytes

    size_t fileSize = (size_t)file.tellg();

    //spirv expects buffer = uint32 so reserve it
    // vector big enough for entire file

    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));

    // put file cursor at the beginning 
    file.seekg(0);

    //load entire file inot the buffer
    file.read((char*)buffer.data(), fileSize);

    // close affter loadeing in 

    file.close();

    //create new shader module using loaded bugger
    VkShaderModuleCreateInfo createInfo ={};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.pNext = nullptr;

    // codeSize has to be in bytes, so multiply the ints in the buffer by size
    // int to know real size of the buffer
    createInfo.codeSize = buffer.size() * sizeof(uint32_t);
    createInfo.pCode = buffer.data();
    // check that the creation goes well.
    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        return false;
    }
    *outShaderModule = shaderModule;
    return true;
}

void PipelineBuilder::clear()
{
    _inputAssembly = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    _rasterizer = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    _colorBlendAttachments.assign(1, {});
    _multisampling = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    _pipelineLayout = VK_NULL_HANDLE;
    _depthStencil = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    _renderInfo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    _colorAttachmentFormats.clear();
    _shaderStages.clear();
}

VkPipeline PipelineBuilder::build_pipeline(VkDevice device)
{
    VkPipelineViewportStateCreateInfo viewportState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1
    };

    VkPipelineColorBlendStateCreateInfo colorBlending{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = _renderInfo.colorAttachmentCount,
        .pAttachments = _colorBlendAttachments.data()
    };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };

    VkGraphicsPipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &_renderInfo,
        .stageCount = static_cast<uint32_t>(_shaderStages.size()),
        .pStages = _shaderStages.data(),
        .pVertexInputState = &vertexInputInfo,
        .pInputAssemblyState = &_inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &_rasterizer,
        .pMultisampleState = &_multisampling,
        .pDepthStencilState = &_depthStencil,
        .pColorBlendState = &colorBlending,
        .layout = _pipelineLayout
    };

    VkDynamicState states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK
    };
    VkPipelineDynamicStateCreateInfo dynamicInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 5,
        .pDynamicStates = states
    };
    pipelineInfo.pDynamicState = &dynamicInfo;

    VkPipeline newPipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &newPipeline) != VK_SUCCESS) {
        fmt::print("Failed to create graphics pipeline\n");
        return VK_NULL_HANDLE;
    }
    return newPipeline;
}

void PipelineBuilder::set_shaders(VkShaderModule vertexShader, VkShaderModule fragmentShader)
{
    _shaderStages.clear();
    _shaderStages.push_back(vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_VERTEX_BIT, vertexShader));
    _shaderStages.push_back(vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentShader));
}

void PipelineBuilder::set_vertex_only_shader(VkShaderModule vertexShader)
{
    _shaderStages.clear();
    _shaderStages.push_back(vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_VERTEX_BIT, vertexShader));
}

void PipelineBuilder::set_input_topology(VkPrimitiveTopology topology)
{
    _inputAssembly.topology = topology;
    _inputAssembly.primitiveRestartEnable = VK_FALSE;
}

void PipelineBuilder::set_polygon_mode(VkPolygonMode mode)
{
    _rasterizer.polygonMode = mode;
    _rasterizer.lineWidth = 1.0f;
}

void PipelineBuilder::set_cull_mode(VkCullModeFlags cullMode, VkFrontFace frontFace)
{
    _rasterizer.cullMode = cullMode;
    _rasterizer.frontFace = frontFace;
}

void PipelineBuilder::set_multisampling_none()
{
    _multisampling.sampleShadingEnable = VK_FALSE;
    _multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    _multisampling.minSampleShading = 1.0f;
    _multisampling.alphaToCoverageEnable = VK_FALSE;
    _multisampling.alphaToOneEnable = VK_FALSE;
}

void PipelineBuilder::disable_blending()
{
    for (VkPipelineColorBlendAttachmentState& attachment : _colorBlendAttachments) {
        attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        attachment.blendEnable = VK_FALSE;
    }
}

void PipelineBuilder::enable_blending_additive()
{
    for (VkPipelineColorBlendAttachmentState& attachment : _colorBlendAttachments) {
        attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
}

void PipelineBuilder::enable_blending_alphablend()
{
    for (VkPipelineColorBlendAttachmentState& attachment : _colorBlendAttachments) {
        attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
}

void PipelineBuilder::set_color_attachment_format(VkFormat format)
{
    set_color_attachment_formats(std::span<const VkFormat>(&format, 1));
}

void PipelineBuilder::set_color_attachment_formats(std::span<const VkFormat> formats)
{
    _colorAttachmentFormats.assign(formats.begin(), formats.end());
    _colorBlendAttachments.resize(_colorAttachmentFormats.size());
    _renderInfo.colorAttachmentCount =
        static_cast<uint32_t>(_colorAttachmentFormats.size());
    _renderInfo.pColorAttachmentFormats = _colorAttachmentFormats.data();
}

void PipelineBuilder::set_depth_format(VkFormat format)
{
    _renderInfo.depthAttachmentFormat = format;
}

void PipelineBuilder::set_stencil_format(VkFormat format)
{
    _renderInfo.stencilAttachmentFormat = format;
}

void PipelineBuilder::set_color_write_mask(
    VkColorComponentFlags mask, size_t attachment)
{
    if (attachment < _colorBlendAttachments.size()) {
        _colorBlendAttachments[attachment].colorWriteMask = mask;
    }
}

void PipelineBuilder::disable_color_attachment()
{
    _colorAttachmentFormats.clear();
    _colorBlendAttachments.clear();
    _renderInfo.colorAttachmentCount = 0;
    _renderInfo.pColorAttachmentFormats = nullptr;
}

// Slope-scaled bias is applied by the rasterizer, before the depth test, so
// it corrects the depth a shadow caster actually writes rather than nudging
// the comparison afterwards.
void PipelineBuilder::enable_depth_bias(float constantFactor, float slopeFactor)
{
    _rasterizer.depthBiasEnable = VK_TRUE;
    _rasterizer.depthBiasConstantFactor = constantFactor;
    _rasterizer.depthBiasClamp = 0.0f;
    _rasterizer.depthBiasSlopeFactor = slopeFactor;
}

void PipelineBuilder::disable_depthtest()
{
    _depthStencil.depthTestEnable = VK_FALSE;
    _depthStencil.depthWriteEnable = VK_FALSE;
    _depthStencil.depthCompareOp = VK_COMPARE_OP_NEVER;
    _depthStencil.depthBoundsTestEnable = VK_FALSE;
    _depthStencil.stencilTestEnable = VK_FALSE;
    _depthStencil.front = {};
    _depthStencil.back = {};
    _depthStencil.minDepthBounds = 0.0f;
    _depthStencil.maxDepthBounds = 1.0f;
}

void PipelineBuilder::enable_depthtest(bool depthWriteEnable, VkCompareOp op)
{
    _depthStencil.depthTestEnable = VK_TRUE;
    _depthStencil.depthWriteEnable = depthWriteEnable ? VK_TRUE : VK_FALSE;
    _depthStencil.depthCompareOp = op;
    _depthStencil.depthBoundsTestEnable = VK_FALSE;
    _depthStencil.stencilTestEnable = VK_FALSE;
    _depthStencil.front = {};
    _depthStencil.back = {};
    _depthStencil.minDepthBounds = 0.0f;
    _depthStencil.maxDepthBounds = 1.0f;
}

void PipelineBuilder::enable_stenciltest(VkCompareOp compareOp, VkStencilOp passOp)
{
    const VkStencilOpState state{
        .failOp = VK_STENCIL_OP_KEEP,
        .passOp = passOp,
        .depthFailOp = VK_STENCIL_OP_KEEP,
        .compareOp = compareOp,
        .compareMask = 0xff,
        .writeMask = 0xff,
        .reference = 0,
    };
    _depthStencil.stencilTestEnable = VK_TRUE;
    _depthStencil.front = state;
    _depthStencil.back = state;
}
