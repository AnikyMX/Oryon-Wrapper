#pragma once
#include "vk/dispatch.hpp"
#include <mutex>

namespace ovk {
struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties mem{};
    VkFormat color_fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depth_fmt = VK_FORMAT_UNDEFINED;
    VkRenderPass pass = VK_NULL_HANDLE;  // default framebuffer: color + depth/stencil, load/store
    char renderer[320] = {};
    std::mutex qlock;  // VkQueue is externally synchronized
};
extern Device gpu;

bool device_init();
void device_shutdown();
uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags want);
VkResult submit(const VkSubmitInfo& si, VkFence fence);
void barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
             VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
             VkAccessFlags dst_access);
}
