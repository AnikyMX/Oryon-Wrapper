#include "context.hpp"
#include <algorithm>

namespace oryon {

namespace {

#if !defined(__ANDROID__)
constexpr uint32_t kHeadlessWidth = 640, kHeadlessHeight = 480;  // headless surfaces report no extent
#endif

VkSemaphore make_semaphore() {
  VkSemaphoreCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = nullptr, .flags = 0};
  VkSemaphore s = VK_NULL_HANDLE;
  vkCreateSemaphore(dev().dev, &ci, nullptr, &s);
  return s;
}

void drop_framebuffer(Surface& s) {
  if (s.framebuffer) vkDestroyFramebuffer(dev().dev, s.framebuffer, nullptr);
  s.framebuffer = VK_NULL_HANDLE;
  dev_destroy(s.color);
  dev_destroy(s.depth);
}

void drop_swapchain(Surface& s, bool keep_handle) {
  Device& d = dev();
  for (VkSemaphore& x : s.acquired) {
    if (x) vkDestroySemaphore(d.dev, x, nullptr);
    x = VK_NULL_HANDLE;
  }
  for (VkSemaphore x : s.rendered) vkDestroySemaphore(d.dev, x, nullptr);
  s.rendered.clear();
  s.images.clear();
  if (!keep_handle && s.swapchain) {
    vkDestroySwapchainKHR(d.dev, s.swapchain, nullptr);
    s.swapchain = VK_NULL_HANDLE;
  }
}

bool make_framebuffer(Surface& s, uint32_t w, uint32_t h) {
  Device& d = dev();
  const bool stencil = format_has_stencil(d.depth_format);
  const VkImageAspectFlags depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
  if (!dev_image(s.color, VK_FORMAT_R8G8B8A8_UNORM, w, h,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT))
    return false;
  if (d.depth_format != VK_FORMAT_UNDEFINED &&
      !dev_image(s.depth, d.depth_format, w, h,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, depth_aspect))
    return false;
  const VkImageView views[2] = {s.color.view, s.depth.view};
  VkFramebufferCreateInfo fi{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                             .pNext = nullptr,
                             .flags = 0,
                             .renderPass = dev_pass(s.color.format, s.depth.format, 0),
                             .attachmentCount = s.depth.image ? 2u : 1u,
                             .pAttachments = views,
                             .width = w,
                             .height = h,
                             .layers = 1};
  if (vkCreateFramebuffer(d.dev, &fi, nullptr, &s.framebuffer) != VK_SUCCESS) return false;

  // Defined initial contents (black, depth 1) in the layouts every render pass expects.
  VkCommandBuffer cmd = dev_once_begin();
  const VkClearColorValue black{};
  const VkImageSubresourceRange cr{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                   .baseMipLevel = 0,
                                   .levelCount = 1,
                                   .baseArrayLayer = 0,
                                   .layerCount = 1};
  image_barrier(cmd, s.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  vkCmdClearColorImage(cmd, s.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &cr);
  image_barrier(cmd, s.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  if (s.depth.image) {
    const VkClearDepthStencilValue one{.depth = 1.0f, .stencil = 0};
    const VkImageSubresourceRange dr{.aspectMask = depth_aspect,
                                     .baseMipLevel = 0,
                                     .levelCount = 1,
                                     .baseArrayLayer = 0,
                                     .layerCount = 1};
    image_barrier(cmd, s.depth.image, depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdClearDepthStencilImage(cmd, s.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &dr);
    image_barrier(cmd, s.depth.image, depth_aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
  }
  dev_once_end(cmd);
  return true;
}

}  // namespace

bool surf_swapchain(Surface& s) {
  Device& d = dev();
  VkSurfaceCapabilitiesKHR caps;
  if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.gpu, s.surface, &caps) != VK_SUCCESS) {
    ORYON_LOG("swapchain: surface capabilities unavailable");
    return false;
  }
  VkExtent2D ext = caps.currentExtent;
#if !defined(__ANDROID__)
  if (ext.width == UINT32_MAX) ext = {kHeadlessWidth, kHeadlessHeight};
#endif
  ext.width = std::clamp(ext.width, caps.minImageExtent.width, caps.maxImageExtent.width);
  ext.height = std::clamp(ext.height, caps.minImageExtent.height, caps.maxImageExtent.height);
  if (!ext.width || !ext.height || !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
    ORYON_LOG("swapchain: extent %ux%u usage 0x%x unusable", ext.width, ext.height, caps.supportedUsageFlags);
    return false;
  }

  uint32_t n = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(d.gpu, s.surface, &n, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(n);
  vkGetPhysicalDeviceSurfaceFormatsKHR(d.gpu, s.surface, &n, formats.data());
  if (formats.empty()) return false;
  VkSurfaceFormatKHR format = formats[0];
  for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
    for (const auto& f : formats)
      if (f.format == want) format = f;
  VkFormatProperties fp;
  vkGetPhysicalDeviceFormatProperties(d.gpu, format.format, &fp);
  if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
    ORYON_LOG("swapchain: format %d is not a blit destination", format.format);
    return false;
  }

  vkGetPhysicalDeviceSurfacePresentModesKHR(d.gpu, s.surface, &n, nullptr);
  std::vector<VkPresentModeKHR> modes(n);
  vkGetPhysicalDeviceSurfacePresentModesKHR(d.gpu, s.surface, &n, modes.data());
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
  if (s.interval == 0)
    for (VkPresentModeKHR want : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR})
      if (std::find(modes.begin(), modes.end(), want) != modes.end()) mode = want;

  uint32_t count = std::max(caps.minImageCount + 1, 3u);
  if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
  VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  for (VkCompositeAlphaFlagBitsKHR a :
       {VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR})
    if (caps.supportedCompositeAlpha & a) alpha = a;

  dev_idle();  // old images/semaphores may still be in flight
  drop_swapchain(s, true);
  const VkSwapchainKHR old = s.swapchain;
  VkSwapchainCreateInfoKHR ci{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                              .pNext = nullptr,
                              .flags = 0,
                              .surface = s.surface,
                              .minImageCount = count,
                              .imageFormat = format.format,
                              .imageColorSpace = format.colorSpace,
                              .imageExtent = ext,
                              .imageArrayLayers = 1,
                              .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                              .queueFamilyIndexCount = 0,
                              .pQueueFamilyIndices = nullptr,
                              .preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                                                  ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                  : caps.currentTransform,
                              .compositeAlpha = alpha,
                              .presentMode = mode,
                              .clipped = VK_TRUE,
                              .oldSwapchain = old};
  const VkResult r = vkCreateSwapchainKHR(d.dev, &ci, nullptr, &s.swapchain);
  if (old) vkDestroySwapchainKHR(d.dev, old, nullptr);
  if (r != VK_SUCCESS) {
    ORYON_LOG("vkCreateSwapchainKHR failed: %d", r);
    s.swapchain = VK_NULL_HANDLE;
    return false;
  }
  vkGetSwapchainImagesKHR(d.dev, s.swapchain, &n, nullptr);
  s.images.resize(n);
  vkGetSwapchainImagesKHR(d.dev, s.swapchain, &n, s.images.data());
  s.rendered.resize(n);
  for (VkSemaphore& x : s.rendered) x = make_semaphore();
  for (VkSemaphore& x : s.acquired) x = make_semaphore();
  s.extent = ext;
  s.stale = false;
  if (s.color.width != ext.width || s.color.height != ext.height) {
    drop_framebuffer(s);
    if (!make_framebuffer(s, ext.width, ext.height)) return false;
  }
  return true;
}

Surface* surf_create(bool window, void* native, uint32_t w, uint32_t h) {
  Device& d = dev();
  Surface* s = new Surface();
  s->window = window;
  s->native = native;
  bool ok = false;
  if (!window) {
    ok = make_framebuffer(*s, w, h);
  } else if (d.swapchain_ext) {
#if defined(__ANDROID__)
    VkAndroidSurfaceCreateInfoKHR ci{.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                     .pNext = nullptr,
                                     .flags = 0,
                                     .window = static_cast<ANativeWindow*>(native)};
    VkResult r = vkCreateAndroidSurfaceKHR(d.instance, &ci, nullptr, &s->surface);
#else
    VkHeadlessSurfaceCreateInfoEXT ci{
        .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT, .pNext = nullptr, .flags = 0};
    VkResult r = vkCreateHeadlessSurfaceEXT(d.instance, &ci, nullptr, &s->surface);
#endif
    VkBool32 present = VK_FALSE;
    if (r == VK_SUCCESS) vkGetPhysicalDeviceSurfaceSupportKHR(d.gpu, d.family, s->surface, &present);
    if (!present) ORYON_LOG("window surface: create=%d, queue family %u cannot present", r, d.family);
    ok = present && surf_swapchain(*s);
  }
  if (!ok) {
    surf_destroy(s);
    return nullptr;
  }
  return s;
}

void surf_destroy(Surface* s) {
  dev_idle();
  drop_swapchain(*s, false);
  drop_framebuffer(*s);
  if (s->surface) vkDestroySurfaceKHR(dev().instance, s->surface, nullptr);
  delete s;
}

}  // namespace oryon
