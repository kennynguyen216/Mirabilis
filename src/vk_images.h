#pragma once 

#include <vulkan/vulkan.h>

namespace vkutil {
void transition_image(
	VkCommandBuffer cmd,
	VkImage image,
	VkImageLayout currentLayout,
	VkImageLayout newLayout,
	VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT);
// Orders every earlier write before every later read or write, for passes that
// keep their images in GENERAL and so need memory ordered, not a layout.
void memory_barrier(VkCommandBuffer cmd);
void copy_image_to_image(VkCommandBuffer cmd, VkImage source, VkImage destination, VkExtent2D srcSize, VkExtent2D dstSize);
void generate_mipmaps(VkCommandBuffer cmd, VkImage image, VkExtent2D imageSize);
	
}
