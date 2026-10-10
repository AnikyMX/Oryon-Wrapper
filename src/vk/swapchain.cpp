#ifdef __ANDROID__
#include "vk/frame.hpp"
#include "oryon.hpp"
#include <android/native_window.h>

namespace ovk {
namespace {
void drop_images(Target& t) {
    for (VkSemaphore s : t.sc_ready) vkDestroySemaphore(gpu.dev, s, nullptr);
    t.sc_ready.clear();
    t.sc_images.clear();
}

bool build(Target& t) {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu.phys, t.surface, &caps) != VK_SUCCESS) return false;
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {
        ext.width = uint32_t(ANativeWindow_getWidth(t.window));
        ext.height = uint32_t(ANativeWindow_getHeight(t.window));
    }
    if (!ext.width || !ext.height) return false;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        OLOG("swapchain images cannot be transfer destinations");
        return false;
    }

    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.phys, t.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.phys, t.surface, &n, fmts.data());
    if (!n) return false;
    VkSurfaceFormatKHR sf = fmts[0];
    for (VkFormat want : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}) {
        bool hit = false;
        for (const VkSurfaceFormatKHR& f : fmts) {
            if (f.format == want) {
                sf = f;
                hit = true;
                break;
            }
        }
        if (hit) break;
    }

    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu.phys, t.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu.phys, t.surface, &n, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (t.interval == 0) {
        for (VkPresentModeKHR want : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
            bool hit = false;
            for (VkPresentModeKHR m : modes) hit |= m == want;
            if (hit) {
                mode = want;
                break;
            }
        }
    }

    uint32_t count = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                          VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
        if (caps.supportedCompositeAlpha & a) {
            alpha = a;
            break;
        }
    }
    const VkSurfaceTransformFlagBitsKHR xf = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                                                 ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                 : caps.currentTransform;
    VkSwapchainKHR old = t.swapchain;
    VkSwapchainCreateInfoKHR ci{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .surface = t.surface,
        .minImageCount = count,
        .imageFormat = sf.format,
        .imageColorSpace = sf.colorSpace,
        .imageExtent = ext,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .preTransform = xf,
        .compositeAlpha = alpha,
        .presentMode = mode,
        .clipped = VK_TRUE,
        .oldSwapchain = old,
    };
    VkResult r = vkCreateSwapchainKHR(gpu.dev, &ci, nullptr, &t.swapchain);
    if (old) vkDestroySwapchainKHR(gpu.dev, old, nullptr);
    drop_images(t);
    if (r != VK_SUCCESS) {
        OLOG("vkCreateSwapchainKHR failed: %d", r);
        t.swapchain = VK_NULL_HANDLE;
        return false;
    }
    vkGetSwapchainImagesKHR(gpu.dev, t.swapchain, &n, nullptr);
    t.sc_images.resize(n);
    vkGetSwapchainImagesKHR(gpu.dev, t.swapchain, &n, t.sc_images.data());
    VkSemaphoreCreateInfo si{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = nullptr, .flags = 0};
    t.sc_ready.resize(n);
    for (VkSemaphore& s : t.sc_ready) vkCreateSemaphore(gpu.dev, &si, nullptr, &s);
    t.sc_format = sf.format;
    t.sc_extent = ext;
    t.dirty = false;
    if (ext.width != t.w || ext.height != t.h || !t.fb) {
        target_release(t);
        if (!target_create(t, ext.width, ext.height)) return false;
    }
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(gpu.phys, sf.format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) OLOG("surface format %d lacks BLIT_DST", sf.format);
    OLOG("swapchain %ux%u images=%u format=%d mode=%d", ext.width, ext.height, n, sf.format, mode);
    return true;
}

bool rebuild(Recorder& rec, Target& t) {
    rec.submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, true);
    {
        std::lock_guard<std::mutex> g(gpu.qlock);
        vkQueueWaitIdle(gpu.queue);
    }
    return build(t);
}
}

bool swapchain_create(Target& t, ANativeWindow* win) {
    ANativeWindow_acquire(win);
    t.window = win;
    VkAndroidSurfaceCreateInfoKHR ci{
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .window = win,
    };
    VkBool32 ok = VK_FALSE;
    if (vkCreateAndroidSurfaceKHR(gpu.instance, &ci, nullptr, &t.surface) != VK_SUCCESS ||
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu.phys, gpu.qfam, t.surface, &ok) != VK_SUCCESS || !ok || !build(t)) {
        swapchain_release(t);
        return false;
    }
    return true;
}

void swapchain_release(Target& t) {
    drop_images(t);
    if (t.swapchain) vkDestroySwapchainKHR(gpu.dev, t.swapchain, nullptr);
    if (t.surface) vkDestroySurfaceKHR(gpu.instance, t.surface, nullptr);
    if (t.window) ANativeWindow_release(t.window);
    t.swapchain = VK_NULL_HANDLE;
    t.surface = VK_NULL_HANDLE;
    t.window = nullptr;
}

VkResult swapchain_present(Recorder& rec, Target& t) {
    if (t.window && (uint32_t(ANativeWindow_getWidth(t.window)) != t.sc_extent.width ||
                     uint32_t(ANativeWindow_getHeight(t.window)) != t.sc_extent.height))
        t.dirty = true;
    if ((t.dirty || !t.swapchain) && !rebuild(rec, t)) {
        rec.submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, false);
        return VK_SUCCESS;  // window not presentable yet (e.g. zero size): drop the frame
    }
    VkCommandBuffer cb = rec.outside(t);
    VkSemaphore acq = rec.acquire[rec.slot];
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(gpu.dev, t.swapchain, UINT64_MAX, acq, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        if (!rebuild(rec, t)) return VK_SUCCESS;
        cb = rec.outside(t);
        acq = rec.acquire[rec.slot];
        r = vkAcquireNextImageKHR(gpu.dev, t.swapchain, UINT64_MAX, acq, VK_NULL_HANDLE, &idx);
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        rec.submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, false);
        return r;
    }
    VkImage dst = t.sc_images[idx];
    barrier(cb, t.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    // Bottom-up GL rows -> top-down window rows.
    VkImageBlit blit{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets = {{0, int32_t(t.h), 0}, {int32_t(t.w), 0, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{0, 0, 0}, {int32_t(t.sc_extent.width), int32_t(t.sc_extent.height), 1}},
    };
    const bool same = t.w == t.sc_extent.width && t.h == t.sc_extent.height;
    vkCmdBlitImage(cb, t.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    barrier(cb, t.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkSemaphore ready = t.sc_ready[idx];
    rec.submit(acq, VK_PIPELINE_STAGE_TRANSFER_BIT, ready, false);
    VkPresentInfoKHR pi{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &ready,
        .swapchainCount = 1,
        .pSwapchains = &t.swapchain,
        .pImageIndices = &idx,
        .pResults = nullptr,
    };
    {
        std::lock_guard<std::mutex> g(gpu.qlock);
        r = vkQueuePresentKHR(gpu.queue, &pi);
    }
    if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) {
        t.dirty = true;
        return VK_SUCCESS;
    }
    return r;
}
}
#endif
