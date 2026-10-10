#include "context.hpp"
#include "proc.hpp"
#include <cstdio>
#if defined(__ANDROID__)
#include <android/native_window.h>
#endif

namespace oryon {

thread_local constinit Context* current = nullptr;

Context* ctx_create(EGLint client_version, EGLenum api) {
  Device* d = device();
  if (!d) return nullptr;
  Context* c = new Context();
  c->client_version = client_version;
  c->api = api;
  gl_defaults(*c);
  for (auto& f : c->frames) {
    VkCommandPoolCreateInfo pci{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                .pNext = nullptr,
                                .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                .queueFamilyIndex = d->family};
    VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
    if (vkCreateCommandPool(d->dev, &pci, nullptr, &f.pool) != VK_SUCCESS ||
        vkCreateFence(d->dev, &fci, nullptr, &f.fence) != VK_SUCCESS) {
      ctx_destroy(c);
      return nullptr;
    }
    VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                   .pNext = nullptr,
                                   .commandPool = f.pool,
                                   .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                   .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(d->dev, &ai, &f.cmd) != VK_SUCCESS) {
      ctx_destroy(c);
      return nullptr;
    }
  }
  std::snprintf(c->renderer, sizeof c->renderer, "Oryon (Vulkan %u.%u.%u, %s)", VK_VERSION_MAJOR(d->props.apiVersion),
                VK_VERSION_MINOR(d->props.apiVersion), VK_VERSION_PATCH(d->props.apiVersion), d->props.deviceName);
  std::snprintf(c->version, sizeof c->version, "%d.%d Oryon", kCapMajor, kCapMinor);
  return c;
}

void ctx_destroy(Context* c) {
  Device& d = dev();
  for (auto& f : c->frames) {
    if (f.busy) vkWaitForFences(d.dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
    if (f.fence) vkDestroyFence(d.dev, f.fence, nullptr);
    if (f.pool) vkDestroyCommandPool(d.dev, f.pool, nullptr);
  }
  dev_destroy(c->readback);
  delete c;
}

VkCommandBuffer ctx_cmd(Context& c) {
  if (c.cmd) return c.cmd;
  Device& d = dev();
  Context::Frame& f = c.frames[c.frame];
  if (f.busy) {
    vkWaitForFences(d.dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
    f.busy = false;
  }
  vkResetFences(d.dev, 1, &f.fence);
  vkResetCommandPool(d.dev, f.pool, 0);
  VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                              .pNext = nullptr,
                              .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                              .pInheritanceInfo = nullptr};
  vkBeginCommandBuffer(f.cmd, &bi);
  return c.cmd = f.cmd;
}

void ctx_begin_pass(Context& c) {
  if (c.in_pass || !c.draw) return;
  Surface& s = *c.draw;
  VkCommandBuffer cmd = ctx_cmd(c);
  VkRenderPassBeginInfo bi{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                           .pNext = nullptr,
                           .renderPass = dev_pass(s.color.format, s.depth.format, c.pending),
                           .framebuffer = s.framebuffer,
                           .renderArea = {{0, 0}, {s.color.width, s.color.height}},
                           .clearValueCount = 2,
                           .pClearValues = c.pending_values};
  vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
  c.pending = 0;
  c.in_pass = true;
}

void ctx_end_pass(Context& c) {
  if (c.pending) ctx_begin_pass(c);  // a deferred full clear still has to land
  if (!c.in_pass) return;
  vkCmdEndRenderPass(c.cmd);
  c.in_pass = false;
}

void ctx_submit(Context& c, VkSemaphore wait, VkPipelineStageFlags stage, VkSemaphore signal, bool block) {
  Device& d = dev();
  ctx_end_pass(c);
  if (c.cmd || wait || signal) {
    VkCommandBuffer cmd = ctx_cmd(c);
    vkEndCommandBuffer(cmd);
    Context::Frame& f = c.frames[c.frame];
    VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .pNext = nullptr,
                    .waitSemaphoreCount = wait ? 1u : 0u,
                    .pWaitSemaphores = &wait,
                    .pWaitDstStageMask = &stage,
                    .commandBufferCount = 1,
                    .pCommandBuffers = &cmd,
                    .signalSemaphoreCount = signal ? 1u : 0u,
                    .pSignalSemaphores = &signal};
    f.busy = dev_submit(si, f.fence) == VK_SUCCESS;
    c.cmd = VK_NULL_HANDLE;
    c.frame ^= 1;
  }
  if (block)
    for (auto& f : c.frames)
      if (f.busy) {
        vkWaitForFences(d.dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
        f.busy = false;
      }
}

void ctx_bind(Context& c, Surface* draw, Surface* read) {
  if (c.draw != draw) ctx_end_pass(c);
  c.draw = draw;
  c.read = read;
  if (c.fresh && draw) {  // GL: viewport/scissor start as the first draw surface size
    c.fresh = false;
    c.viewport[2] = c.scissor[2] = static_cast<GLint>(draw->color.width);
    c.viewport[3] = c.scissor[3] = static_cast<GLint>(draw->color.height);
  }
}

EGLint ctx_swap(Context& c, Surface& s) {
  if (!s.window) {
    ctx_submit(c, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, false);
    return EGL_SUCCESS;
  }
  Device& d = dev();
#if defined(__ANDROID__)
  if (!s.stale && s.swapchain) {  // window resized: rebuild before acquiring
    auto* win = static_cast<ANativeWindow*>(s.native);
    s.stale = static_cast<uint32_t>(ANativeWindow_getWidth(win)) != s.extent.width ||
              static_cast<uint32_t>(ANativeWindow_getHeight(win)) != s.extent.height;
  }
#endif
  uint32_t index = 0;
  VkSemaphore acquired = VK_NULL_HANDLE;
  VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
  for (int attempt = 0; attempt < 2 && r == VK_ERROR_OUT_OF_DATE_KHR; ++attempt) {
    if (s.stale || !s.swapchain) {
      ctx_submit(c, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, false);
      if (!surf_swapchain(s)) return EGL_BAD_SURFACE;
    }
    if (s.presenter != &c) {  // acquire semaphores are tracked by the presenting context's frame fences
      dev_idle();
      s.presenter = &c;
    }
    ctx_end_pass(c);
    ctx_cmd(c);  // waits this slot's previous submit, so its acquire semaphore has no pending wait
    acquired = s.acquired[c.frame];
    r = vkAcquireNextImageKHR(d.dev, s.swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) s.stale = true;
  }
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
    s.stale = true;
    ctx_submit(c, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, false);
    return EGL_BAD_SURFACE;
  }

  VkCommandBuffer cmd = c.cmd;
  const VkImage dst = s.images[index];
  image_barrier(cmd, s.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  image_barrier(cmd, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  const int32_t sw = static_cast<int32_t>(s.color.width), sh = static_cast<int32_t>(s.color.height);
  const int32_t dw = static_cast<int32_t>(s.extent.width), dh = static_cast<int32_t>(s.extent.height);
  VkImageBlit blit{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                   .srcOffsets = {{0, 0, 0}, {sw, sh, 1}},
                   .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                   .dstOffsets = {{0, dh, 0}, {dw, 0, 1}}};
  vkCmdBlitImage(cmd, s.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                 &blit, sw == dw && sh == dh ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
  image_barrier(cmd, s.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  image_barrier(cmd, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
  ctx_submit(c, acquired, VK_PIPELINE_STAGE_TRANSFER_BIT, s.rendered[index], false);

  VkPresentInfoKHR pi{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                      .pNext = nullptr,
                      .waitSemaphoreCount = 1,
                      .pWaitSemaphores = &s.rendered[index],
                      .swapchainCount = 1,
                      .pSwapchains = &s.swapchain,
                      .pImageIndices = &index,
                      .pResults = nullptr};
  r = dev_present(pi);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) s.stale = true;  // SUBOPTIMAL (e.g. rotation) keeps presenting as is
  else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
    s.stale = true;
    return EGL_BAD_SURFACE;
  }
  return EGL_SUCCESS;
}

}  // namespace oryon
