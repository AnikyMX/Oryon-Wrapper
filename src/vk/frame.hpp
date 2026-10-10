#pragma once
#include "vk/device.hpp"
#include <vector>

struct ANativeWindow;

namespace ovk {
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};
bool image_create(Image& im, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags aspect);
void image_destroy(Image& im);

// GL default framebuffer. Rows are stored bottom-up (GL window y == image row), so
// glReadPixels and texture copies need no flip; only presentation flips.
struct Target {
    uint32_t w = 0, h = 0;
    Image color, depth;
    VkFramebuffer fb = VK_NULL_HANDLE;
    bool fresh = true;  // images still in UNDEFINED layout
    // window surfaces only
    ANativeWindow* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat sc_format = VK_FORMAT_UNDEFINED;
    VkExtent2D sc_extent{};
    std::vector<VkImage> sc_images;
    std::vector<VkSemaphore> sc_ready;
    int interval = 1;
    bool dirty = false;  // swapchain must be rebuilt before next present
};
bool target_create(Target& t, uint32_t w, uint32_t h);
void target_release(Target& t);

// Per-context command recording over a ring of submit slots.
struct Recorder {
    static constexpr uint32_t kSlots = 3;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd[kSlots] = {};
    VkFence fence[kSlots] = {};
    VkSemaphore acquire[kSlots] = {};
    bool pending[kSlots] = {};
    uint32_t slot = 0;
    bool recording = false;
    Target* pass_target = nullptr;
    VkBuffer rb = VK_NULL_HANDLE;
    VkDeviceMemory rb_mem = VK_NULL_HANDLE;
    uint8_t* rb_ptr = nullptr;
    VkDeviceSize rb_size = 0;

    bool init();
    void destroy();
    VkCommandBuffer begin();
    VkCommandBuffer outside();
    VkCommandBuffer outside(Target& t);
    VkCommandBuffer inside(Target& t);
    void submit(VkSemaphore wait, VkPipelineStageFlags stage, VkSemaphore signal, bool block);
    void wait(uint32_t s);
    void idle();
    const uint8_t* readback(Target& t, int32_t x, int32_t y, uint32_t w, uint32_t h);
};

#ifdef __ANDROID__
bool swapchain_create(Target& t, ANativeWindow* win);
VkResult swapchain_present(Recorder& rec, Target& t);
void swapchain_release(Target& t);
#endif
}
