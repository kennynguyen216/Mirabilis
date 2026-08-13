#include <vk_types.h>
#include "vk_descriptors.h"

    void DescriptorLayoutBuilder::add_binding(uint32_t binding, VkDescriptorType type)
    {
        VkDescriptorSetLayoutBinding newbind {};
        newbind.binding = binding;
        newbind.descriptorCount = 1;
        newbind.descriptorType = type;

        bindings.push_back(newbind);
    }

    void DescriptorLayoutBuilder::clear()
    {
        bindings.clear();
    }

    VkDescriptorSetLayout DescriptorLayoutBuilder::build(VkDevice device, VkShaderStageFlags shaderStages, void* pNext, VkDescriptorSetLayoutCreateFlags flags)
    {
        for (auto& b : bindings) {
        b.stageFlags |= shaderStages;
        }

        VkDescriptorSetLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.pNext = pNext;

        info.pBindings = bindings.data();
        info.bindingCount = (uint32_t)bindings.size();
        info.flags = flags;

        VkDescriptorSetLayout set;
        VK_CHECK(vkCreateDescriptorSetLayout(device, &info, nullptr, &set));
        return set;
    }

    void DescriptorAllocator::init_pool(VkDevice device, uint32_t maxSets, std::span<PoolSizeRatio> poolRatios)
    {
        std::vector<VkDescriptorPoolSize> poolSizes;
        for(PoolSizeRatio ratio: poolRatios){
            poolSizes.push_back(VkDescriptorPoolSize{
                .type = ratio.type,
                .descriptorCount = uint32_t(ratio.ratio * maxSets)
            });
        }
        VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.flags = 0;
	    pool_info.maxSets = maxSets;
	    pool_info.poolSizeCount = (uint32_t)poolSizes.size();
	    pool_info.pPoolSizes = poolSizes.data();

	    vkCreateDescriptorPool(device, &pool_info, nullptr, &pool);
    }

    void DescriptorAllocator::clear_descriptors(VkDevice device)
    {
        vkResetDescriptorPool(device, pool, 0);
    }

    void DescriptorAllocator::destroy_pool(VkDevice device)
    {
        vkDestroyDescriptorPool(device, pool, nullptr);
    }

    VkDescriptorSet DescriptorAllocator::allocate(VkDevice device, VkDescriptorSetLayout layout)
    {
        VkDescriptorSetAllocateInfo allocInfo = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.pNext = nullptr;
        allocInfo.descriptorPool = pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &layout;

        VkDescriptorSet ds;
        VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, &ds));

        return ds;
    }

    VkDescriptorPool DescriptorAllocatorGrowable::get_pool(VkDevice device)
    {
        VkDescriptorPool newPool = VK_NULL_HANDLE;
        if (!readyPools.empty()) {
            newPool = readyPools.back();
            readyPools.pop_back();
        } else {
            newPool = create_pool(device, setsPerPool, ratios);
            setsPerPool = static_cast<uint32_t>(setsPerPool * 1.5f);
            setsPerPool = std::min(setsPerPool, 4092u);
        }
        return newPool;
    }

    VkDescriptorPool DescriptorAllocatorGrowable::create_pool(
        VkDevice device,
        uint32_t setCount,
        std::span<PoolSizeRatio> poolRatios)
    {
        std::vector<VkDescriptorPoolSize> poolSizes;
        poolSizes.reserve(poolRatios.size());
        for (PoolSizeRatio ratio : poolRatios) {
            poolSizes.push_back(VkDescriptorPoolSize{
                .type = ratio.type,
                .descriptorCount = static_cast<uint32_t>(ratio.ratio * setCount)});
        }

        VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = setCount,
            .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
            .pPoolSizes = poolSizes.data()};
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool));
        return pool;
    }

    void DescriptorAllocatorGrowable::init(
        VkDevice device,
        uint32_t initialSets,
        std::span<PoolSizeRatio> poolRatios)
    {
        ratios.assign(poolRatios.begin(), poolRatios.end());
        setsPerPool = initialSets;
        readyPools.push_back(create_pool(device, setsPerPool, ratios));
        setsPerPool = std::min(static_cast<uint32_t>(setsPerPool * 1.5f), 4092u);
    }

    void DescriptorAllocatorGrowable::clear_pools(VkDevice device)
    {
        for (VkDescriptorPool pool : readyPools) {
            VK_CHECK(vkResetDescriptorPool(device, pool, 0));
        }
        for (VkDescriptorPool pool : fullPools) {
            VK_CHECK(vkResetDescriptorPool(device, pool, 0));
            readyPools.push_back(pool);
        }
        fullPools.clear();
    }

    void DescriptorAllocatorGrowable::destroy_pools(VkDevice device)
    {
        for (VkDescriptorPool pool : readyPools) {
            vkDestroyDescriptorPool(device, pool, nullptr);
        }
        for (VkDescriptorPool pool : fullPools) {
            vkDestroyDescriptorPool(device, pool, nullptr);
        }
        readyPools.clear();
        fullPools.clear();
    }

    VkDescriptorSet DescriptorAllocatorGrowable::allocate(
        VkDevice device,
        VkDescriptorSetLayout layout,
        void* pNext)
    {
        VkDescriptorPool pool = get_pool(device);
        VkDescriptorSetAllocateInfo allocInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .pNext = pNext,
            .descriptorPool = pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &layout};

        VkDescriptorSet set = VK_NULL_HANDLE;
        VkResult result = vkAllocateDescriptorSets(device, &allocInfo, &set);
        if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) {
            fullPools.push_back(pool);
            pool = get_pool(device);
            allocInfo.descriptorPool = pool;
            VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, &set));
        } else {
            VK_CHECK(result);
        }
        readyPools.push_back(pool);
        return set;
    }

    void DescriptorWriter::write_image(
        int binding,
        VkImageView image,
        VkSampler sampler,
        VkImageLayout layout,
        VkDescriptorType type)
    {
        VkDescriptorImageInfo& info = imageInfos.emplace_back(VkDescriptorImageInfo{
            .sampler = sampler,
            .imageView = image,
            .imageLayout = layout});
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstBinding = static_cast<uint32_t>(binding);
        write.descriptorCount = 1;
        write.descriptorType = type;
        write.pImageInfo = &info;
        writes.push_back(write);
    }

    void DescriptorWriter::write_buffer(
        int binding,
        VkBuffer buffer,
        size_t size,
        size_t offset,
        VkDescriptorType type)
    {
        VkDescriptorBufferInfo& info = bufferInfos.emplace_back(VkDescriptorBufferInfo{
            .buffer = buffer,
            .offset = offset,
            .range = size});
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstBinding = static_cast<uint32_t>(binding);
        write.descriptorCount = 1;
        write.descriptorType = type;
        write.pBufferInfo = &info;
        writes.push_back(write);
    }

    void DescriptorWriter::clear()
    {
        imageInfos.clear();
        bufferInfos.clear();
        writes.clear();
    }

    void DescriptorWriter::update_set(VkDevice device, VkDescriptorSet set)
    {
        for (VkWriteDescriptorSet& write : writes) {
            write.dstSet = set;
        }
        vkUpdateDescriptorSets(
            device,
            static_cast<uint32_t>(writes.size()),
            writes.data(),
            0,
            nullptr);
    }
    
