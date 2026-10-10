#pragma once
#define VK_NO_PROTOTYPES
#include "vulkan_core.h"
#ifdef __ANDROID__
#include "vulkan_android.h"
#endif

#define OVK_GLOBAL(X) \
    X(vkCreateInstance) \
    X(vkEnumerateInstanceVersion)

#define OVK_INSTANCE_BASE(X) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkCreateDevice) \
    X(vkGetDeviceProcAddr)

#define OVK_DEVICE_BASE(X) \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkDeviceWaitIdle) \
    X(vkQueueSubmit) \
    X(vkQueueWaitIdle) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkWaitForFences) \
    X(vkResetFences) \
    X(vkCreateSemaphore) \
    X(vkDestroySemaphore) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkMapMemory) \
    X(vkCreateImage) \
    X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkCreateRenderPass) \
    X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) \
    X(vkDestroyFramebuffer) \
    X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) \
    X(vkCmdClearAttachments) \
    X(vkCmdClearColorImage) \
    X(vkCmdClearDepthStencilImage) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdBlitImage)

#ifdef __ANDROID__
#define OVK_INSTANCE_WSI(X) \
    X(vkCreateAndroidSurfaceKHR) \
    X(vkDestroySurfaceKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define OVK_DEVICE_WSI(X) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR)
#else
#define OVK_INSTANCE_WSI(X)
#define OVK_DEVICE_WSI(X)
#endif

#define OVK_INSTANCE(X) OVK_INSTANCE_BASE(X) OVK_INSTANCE_WSI(X)
#define OVK_DEVICE(X) OVK_DEVICE_BASE(X) OVK_DEVICE_WSI(X)

namespace ovk {
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define OVK_DECL(n) extern PFN_##n n;
OVK_GLOBAL(OVK_DECL)
OVK_INSTANCE(OVK_DECL)
OVK_DEVICE(OVK_DECL)
#undef OVK_DECL

bool load_global();
void load_instance(VkInstance inst);
void load_device(VkDevice dev);
}
