#include "vk/frame.hpp"
#include "oryon.hpp"

namespace ovk {
namespace {
constexpr VkPipelineStageFlags kAttStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
constexpr VkAccessFlags kColorRW = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
constexpr VkAccessFlags kDepthRW =
    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
constexpr VkImageAspectFlags kDS = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

// First use of a target: define contents (black, depth 1, stencil 0) and enter attachment layouts.
void prepare(VkCommandBuffer cb, Target& t) {
    if (!t.fresh) return;
    t.fresh = false;
    barrier(cb, t.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    barrier(cb, t.depth.image, kDS, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue black{};
    VkClearDepthStencilValue ds{1.0f, 0};
    VkImageSubresourceRange cr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageSubresourceRange dr{kDS, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, t.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &cr);
    vkCmdClearDepthStencilImage(cb, t.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ds, 1, &dr);
    barrier(cb, t.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            kAttStages, kColorRW);
    barrier(cb, t.depth.image, kDS, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, kAttStages, kDepthRW);
}
}

bool image_create(Image& im, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    VkImageCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(gpu.dev, &ci, nullptr, &im.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(gpu.dev, im.image, &req);
    VkMemoryAllocateInfo ai{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .allocationSize = req.size,
        .memoryTypeIndex = memory_type(req.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(gpu.dev, &ai, nullptr, &im.mem) != VK_SUCCESS ||
        vkBindImageMemory(gpu.dev, im.image, im.mem, 0) != VK_SUCCESS) {
        image_destroy(im);
        return false;
    }
    VkImageViewCreateInfo vi{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = im.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .components = {},
        .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    if (vkCreateImageView(gpu.dev, &vi, nullptr, &im.view) != VK_SUCCESS) {
        image_destroy(im);
        return false;
    }
    return true;
}

void image_destroy(Image& im) {
    if (im.view) vkDestroyImageView(gpu.dev, im.view, nullptr);
    if (im.image) vkDestroyImage(gpu.dev, im.image, nullptr);
    if (im.mem) vkFreeMemory(gpu.dev, im.mem, nullptr);
    im = Image{};
}

bool target_create(Target& t, uint32_t w, uint32_t h) {
    t.w = w ? w : 1;
    t.h = h ? h : 1;
    t.fresh = true;
    const VkImageUsageFlags cu = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags du = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!image_create(t.color, gpu.color_fmt, t.w, t.h, cu, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !image_create(t.depth, gpu.depth_fmt, t.w, t.h, du, kDS)) {
        target_release(t);
        return false;
    }
    VkImageView views[2] = {t.color.view, t.depth.view};
    VkFramebufferCreateInfo fi{
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderPass = gpu.pass,
        .attachmentCount = 2,
        .pAttachments = views,
        .width = t.w,
        .height = t.h,
        .layers = 1,
    };
    if (vkCreateFramebuffer(gpu.dev, &fi, nullptr, &t.fb) != VK_SUCCESS) {
        target_release(t);
        return false;
    }
    return true;
}

void target_release(Target& t) {
    if (t.fb) vkDestroyFramebuffer(gpu.dev, t.fb, nullptr);
    t.fb = VK_NULL_HANDLE;
    image_destroy(t.color);
    image_destroy(t.depth);
}

bool Recorder::init() {
    VkCommandPoolCreateInfo pci{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = gpu.qfam,
    };
    if (vkCreateCommandPool(gpu.dev, &pci, nullptr, &pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = kSlots,
    };
    if (vkAllocateCommandBuffers(gpu.dev, &ai, cmd) != VK_SUCCESS) return false;
    VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
    VkSemaphoreCreateInfo sci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = nullptr, .flags = 0};
    for (uint32_t i = 0; i < kSlots; i++) {
        if (vkCreateFence(gpu.dev, &fci, nullptr, &fence[i]) != VK_SUCCESS ||
            vkCreateSemaphore(gpu.dev, &sci, nullptr, &acquire[i]) != VK_SUCCESS)
            return false;
    }
    return true;
}

void Recorder::destroy() {
    if (recording) submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, false);
    idle();
    for (uint32_t i = 0; i < kSlots; i++) {
        if (fence[i]) vkDestroyFence(gpu.dev, fence[i], nullptr);
        if (acquire[i]) vkDestroySemaphore(gpu.dev, acquire[i], nullptr);
    }
    if (pool) vkDestroyCommandPool(gpu.dev, pool, nullptr);
    if (rb) vkDestroyBuffer(gpu.dev, rb, nullptr);
    if (rb_mem) vkFreeMemory(gpu.dev, rb_mem, nullptr);
    *this = Recorder{};
}

void Recorder::wait(uint32_t s) {
    if (!pending[s]) return;
    vkWaitForFences(gpu.dev, 1, &fence[s], VK_TRUE, UINT64_MAX);
    vkResetFences(gpu.dev, 1, &fence[s]);
    pending[s] = false;
}

void Recorder::idle() {
    for (uint32_t s = 0; s < kSlots; s++) wait(s);
}

VkCommandBuffer Recorder::begin() {
    if (recording) return cmd[slot];
    wait(slot);
    vkResetCommandBuffer(cmd[slot], 0);
    VkCommandBufferBeginInfo bi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    vkBeginCommandBuffer(cmd[slot], &bi);
    recording = true;
    return cmd[slot];
}

VkCommandBuffer Recorder::outside() {
    VkCommandBuffer cb = begin();
    if (pass_target) {
        vkCmdEndRenderPass(cb);
        pass_target = nullptr;
    }
    return cb;
}

VkCommandBuffer Recorder::outside(Target& t) {
    VkCommandBuffer cb = outside();
    prepare(cb, t);
    return cb;
}

VkCommandBuffer Recorder::inside(Target& t) {
    if (pass_target == &t) return cmd[slot];
    VkCommandBuffer cb = outside(t);
    VkRenderPassBeginInfo bi{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext = nullptr,
        .renderPass = gpu.pass,
        .framebuffer = t.fb,
        .renderArea = {{0, 0}, {t.w, t.h}},
        .clearValueCount = 0,
        .pClearValues = nullptr,
    };
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    pass_target = &t;
    return cb;
}

void Recorder::submit(VkSemaphore wait_sem, VkPipelineStageFlags stage, VkSemaphore signal, bool block) {
    if (!recording) {
        if (block) idle();
        return;
    }
    VkCommandBuffer cb = outside();
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = wait_sem ? 1u : 0u,
        .pWaitSemaphores = &wait_sem,
        .pWaitDstStageMask = &stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
        .signalSemaphoreCount = signal ? 1u : 0u,
        .pSignalSemaphores = &signal,
    };
    VkResult r = ovk::submit(si, fence[slot]);
    if (r != VK_SUCCESS) OLOG("vkQueueSubmit failed: %d", r);
    pending[slot] = r == VK_SUCCESS;
    recording = false;
    const uint32_t s = slot;
    slot = (slot + 1) % kSlots;
    if (block) wait(s);
}

const uint8_t* Recorder::readback(Target& t, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    const VkDeviceSize need = VkDeviceSize(w) * h * 4;
    if (need > rb_size) {
        idle();
        if (rb) vkDestroyBuffer(gpu.dev, rb, nullptr);
        if (rb_mem) vkFreeMemory(gpu.dev, rb_mem, nullptr);
        rb = VK_NULL_HANDLE;
        rb_mem = VK_NULL_HANDLE;
        rb_size = 0;
        const VkDeviceSize size = (need + 0xFFFF) & ~VkDeviceSize(0xFFFF);
        VkBufferCreateInfo bci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        if (vkCreateBuffer(gpu.dev, &bci, nullptr, &rb) != VK_SUCCESS) return nullptr;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(gpu.dev, rb, &req);
        VkMemoryAllocateInfo ai{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = nullptr,
            .allocationSize = req.size,
            .memoryTypeIndex =
                memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
        };
        void* p = nullptr;
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(gpu.dev, &ai, nullptr, &rb_mem) != VK_SUCCESS ||
            vkBindBufferMemory(gpu.dev, rb, rb_mem, 0) != VK_SUCCESS ||
            vkMapMemory(gpu.dev, rb_mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS)
            return nullptr;
        rb_ptr = static_cast<uint8_t*>(p);
        rb_size = size;
    }
    VkCommandBuffer cb = outside(t);
    barrier(cb, t.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy reg{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {x, y, 0},
        .imageExtent = {w, h, 1},
    };
    vkCmdCopyImageToBuffer(cb, t.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &reg);
    barrier(cb, t.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, kColorRW);
    VkBufferMemoryBarrier hb{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = rb,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0,
                         nullptr);
    submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, true);
    return rb_ptr;
}
}
