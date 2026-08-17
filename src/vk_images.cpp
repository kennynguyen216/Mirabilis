#include <vk_initializers.h>
#include <vk_images.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cmath>

void vkutil::transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout currentLayout, VkImageLayout newLayout)
{
    VkImageMemoryBarrier2 imageBarrier { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    imageBarrier.pNext = nullptr;

    imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    imageBarrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    imageBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;

    imageBarrier.oldLayout = currentLayout;
    imageBarrier.newLayout = newLayout;

    VkImageAspectFlags aspectMask =
        (newLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL)
        ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
        : VK_IMAGE_ASPECT_COLOR_BIT;
    imageBarrier.subresourceRange = vkinit::image_subresource_range(aspectMask);
    imageBarrier.image = image;

    VkDependencyInfo depInfo {};
    depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    depInfo.pNext = nullptr;

    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &imageBarrier;

    vkCmdPipelineBarrier2(cmd, &depInfo);

}

void vkutil::generate_mipmaps(VkCommandBuffer cmd, VkImage image, VkExtent2D imageSize)
{
    const int mipLevels = static_cast<int>(
        std::floor(std::log2(std::max(imageSize.width, imageSize.height)))) + 1;

    VkExtent2D currentSize = imageSize;
    for (int mip = 0; mip < mipLevels; ++mip) {
        VkImageMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.image = image;
        barrier.subresourceRange = vkinit::image_subresource_range(
            VK_IMAGE_ASPECT_COLOR_BIT);
        barrier.subresourceRange.baseMipLevel = static_cast<uint32_t>(mip);
        barrier.subresourceRange.levelCount = 1;

        VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);

        if (mip < mipLevels - 1) {
            VkExtent2D nextSize{
                std::max(1u, currentSize.width / 2),
                std::max(1u, currentSize.height / 2)};

            VkImageBlit2 region{
                .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
            region.srcOffsets[1] = VkOffset3D{
                static_cast<int32_t>(currentSize.width),
                static_cast<int32_t>(currentSize.height),
                1};
            region.dstOffsets[1] = VkOffset3D{
                static_cast<int32_t>(nextSize.width),
                static_cast<int32_t>(nextSize.height),
                1};
            region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.srcSubresource.mipLevel = static_cast<uint32_t>(mip);
            region.srcSubresource.layerCount = 1;
            region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.dstSubresource.mipLevel = static_cast<uint32_t>(mip + 1);
            region.dstSubresource.layerCount = 1;

            VkBlitImageInfo2 blit{
                .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
            blit.srcImage = image;
            blit.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            blit.dstImage = image;
            blit.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            blit.filter = VK_FILTER_LINEAR;
            blit.regionCount = 1;
            blit.pRegions = &region;
            vkCmdBlitImage2(cmd, &blit);

            currentSize = nextSize;
        }
    }

    VkImageMemoryBarrier2 finalBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    finalBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    finalBarrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    finalBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    finalBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    finalBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    finalBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    finalBarrier.image = image;
    finalBarrier.subresourceRange = vkinit::image_subresource_range(
        VK_IMAGE_ASPECT_COLOR_BIT);

    VkDependencyInfo finalDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    finalDependency.imageMemoryBarrierCount = 1;
    finalDependency.pImageMemoryBarriers = &finalBarrier;
    vkCmdPipelineBarrier2(cmd, &finalDependency);
}
