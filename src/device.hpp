// Process-wide Vulkan device plus small allocation / render pass helpers.
#pragma once
#include "vk.hpp"
#include <mutex>
#include <vector>

namespace oryon {

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0;
};

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* map = nullptr;
  VkDeviceSize size = 0;
  bool coherent = true;
};

constexpr uint32_t kQuadBatch = 65536;  // vertices per indexed quad batch (uint16 indices)

// Render pass load-op key: which aspects start with a clear.
enum : uint32_t { CLEAR_COLOR = 1, CLEAR_DEPTH = 2, CLEAR_STENCIL = 4 };

struct Device {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice gpu = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkPhysicalDeviceProperties props{};
  VkPhysicalDeviceMemoryProperties memory{};
  VkFormat depth_format = VK_FORMAT_UNDEFINED;
  bool surface_ext = false, swapchain_ext = false;
  VkPhysicalDeviceFeatures features{};  // enabled subset (wideLines, logicOp)
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;  // 0: FFUniforms (dynamic), 1-2: texture units
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
  VkPipelineCache cache = VK_NULL_HANDLE;
  Image white;             // 1x1 texture for units without one
  Buffer quads;            // uint16 index pattern 0,1,2, 2,3,0 for kQuadBatch vertices
  std::mutex queue_lock, once_lock, pass_lock;
  VkCommandPool once_pool = VK_NULL_HANDLE;
  VkCommandBuffer once_cmd = VK_NULL_HANDLE;
  VkFence once_fence = VK_NULL_HANDLE;
  struct Pass {
    VkFormat color, depth;
    uint32_t clear;
    VkRenderPass pass;
  };
  std::vector<Pass> passes;
};

extern Device g_device;
Device* device();  // created on first use; nullptr when Vulkan is unusable
inline Device& dev() { return g_device; }  // after device() succeeded; also used while it runs

bool format_has_stencil(VkFormat f);
uint32_t dev_memory_type(uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags want);
bool dev_image(Image& im, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags aspect,
               uint32_t levels = 1);
void dev_destroy(Image& im);
bool dev_host_buffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags need,
                     VkMemoryPropertyFlags want);
void dev_destroy(Buffer& b);
VkRenderPass dev_pass(VkFormat color, VkFormat depth, uint32_t clear);
VkCommandBuffer dev_once_begin();         // locks; record one-shot setup work
void dev_once_end(VkCommandBuffer cmd);   // submit, wait, unlock
VkResult dev_submit(const VkSubmitInfo& info, VkFence fence);
VkResult dev_present(const VkPresentInfoKHR& info);
void dev_idle();
void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                   VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                   VkAccessFlags dst_access, uint32_t level = 0, uint32_t levels = 1);

}  // namespace oryon
