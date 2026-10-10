// Vulkan entry points loaded at runtime from the system loader (no link-time dependency).
#pragma once
#define VK_NO_PROTOTYPES
#if defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include "vulkan_core.h"
#if defined(__ANDROID__)
#include "vulkan_android.h"
#endif

#define ORYON_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define ORYON_VK_GLOBAL_OPT(X) X(vkEnumerateInstanceVersion)

#define ORYON_VK_INSTANCE(X) \
  X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
  X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
  X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceFeatures) X(vkEnumerateDeviceExtensionProperties) \
  X(vkCreateDevice) \
  X(vkGetDeviceProcAddr)

#if defined(__ANDROID__)
#define ORYON_VK_PLATFORM(X) X(vkCreateAndroidSurfaceKHR)
#else
#define ORYON_VK_PLATFORM(X) X(vkCreateHeadlessSurfaceEXT)
#endif
#define ORYON_VK_INSTANCE_OPT(X) \
  X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) ORYON_VK_PLATFORM(X)

#define ORYON_VK_DEVICE(X) \
  X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) \
  X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) \
  X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) \
  X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateImage) X(vkDestroyImage) \
  X(vkCreateImageView) X(vkDestroyImageView) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
  X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
  X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkInvalidateMappedMemoryRanges) \
  X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
  X(vkCmdPipelineBarrier) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdClearAttachments) \
  X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdBlitImage) X(vkCmdCopyImageToBuffer) \
  X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateShaderModule) X(vkCreatePipelineCache) \
  X(vkCreateGraphicsPipelines) X(vkCreateSampler) X(vkCreateDescriptorPool) X(vkResetDescriptorPool) \
  X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdBindPipeline) \
  X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) \
  X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetLineWidth) X(vkCmdSetDepthBias) X(vkCmdCopyBufferToImage) \
  X(vkCmdCopyImage)

#define ORYON_VK_DEVICE_OPT(X) \
  X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) \
  X(vkQueuePresentKHR)

namespace oryon {

extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define ORYON_VK_DECL(n) extern PFN_##n n;
ORYON_VK_GLOBAL(ORYON_VK_DECL)
ORYON_VK_GLOBAL_OPT(ORYON_VK_DECL)
ORYON_VK_INSTANCE(ORYON_VK_DECL)
ORYON_VK_INSTANCE_OPT(ORYON_VK_DECL)
ORYON_VK_DEVICE(ORYON_VK_DECL)
ORYON_VK_DEVICE_OPT(ORYON_VK_DECL)
#undef ORYON_VK_DECL

bool vkg_load();                  // dlopen the loader, global-level entry points
bool vki_load(VkInstance inst);   // instance-level entry points
bool vkd_load(VkDevice dev);      // device-level entry points

}  // namespace oryon
