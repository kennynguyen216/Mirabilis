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
