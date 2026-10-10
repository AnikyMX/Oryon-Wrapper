#include "vk/device.hpp"
#include "oryon.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

namespace ovk {
Device gpu;

namespace {
bool format_ok(VkFormat f, VkFormatFeatureFlags need) {
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(gpu.phys, f, &fp);
    return (fp.optimalTilingFeatures & need) == need;
}

bool pick_physical() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(gpu.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(gpu.instance, &n, pds.data());
    int best = -1;
    for (VkPhysicalDevice pd : pds) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pd, &p);
        if (p.apiVersion < VK_API_VERSION_1_1) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
        for (uint32_t i = 0; i < qn; i++) {
            if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                        : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                        : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? 0
                                                                                 : 1;
            if (score > best) {
                best = score;
                gpu.phys = pd;
                gpu.qfam = i;
                gpu.props = p;
            }
            break;
        }
    }
    return best >= 0;
}

bool create_pass() {
    VkAttachmentDescription att[2] = {};
    att[0].format = gpu.color_fmt;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[1] = att[0];
    att[1].format = gpu.depth_fmt;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{
        .flags = 0,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount = 0,
        .pInputAttachments = nullptr,
        .colorAttachmentCount = 1,
        .pColorAttachments = &cref,
        .pResolveAttachments = nullptr,
        .pDepthStencilAttachment = &dref,
        .preserveAttachmentCount = 0,
        .pPreserveAttachments = nullptr,
    };
    // Consecutive passes on one target load what the previous pass stored.
    const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency dep{
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = stages,
        .dstStageMask = stages,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = access,
        .dependencyFlags = 0,
    };
    VkRenderPassCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .attachmentCount = 2,
        .pAttachments = att,
        .subpassCount = 1,
        .pSubpasses = &sub,
        .dependencyCount = 1,
        .pDependencies = &dep,
    };
    return vkCreateRenderPass(gpu.dev, &ci, nullptr, &gpu.pass) == VK_SUCCESS;
}
}

bool device_init() {
    if (gpu.dev) return true;
    if (!load_global()) return false;
    uint32_t api = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&api);
    if (api < VK_API_VERSION_1_1) {
        OLOG("Vulkan 1.1 loader required, found %u.%u", VK_VERSION_MAJOR(api), VK_VERSION_MINOR(api));
        return false;
    }
    const char* iext[2];
    uint32_t niext = 0;
#ifdef __ANDROID__
    iext[niext++] = VK_KHR_SURFACE_EXTENSION_NAME;
    iext[niext++] = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
#endif
    VkApplicationInfo app{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = "Minecraft",
        .applicationVersion = VK_MAKE_VERSION(1, 12, 2),
        .pEngineName = "Oryon",
        .engineVersion = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pApplicationInfo = &app,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = niext,
        .ppEnabledExtensionNames = niext ? iext : nullptr,
    };
    VkResult r = vkCreateInstance(&ici, nullptr, &gpu.instance);
    if (r != VK_SUCCESS) {
        OLOG("vkCreateInstance failed: %d", r);
        return false;
    }
    load_instance(gpu.instance);
    if (!pick_physical()) {
        OLOG("no Vulkan 1.1 device with a graphics queue");
        device_shutdown();
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(gpu.phys, &gpu.mem);
    const VkFormatFeatureFlags dsneed =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (VkFormat f : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
        if (format_ok(f, dsneed)) {
            gpu.depth_fmt = f;
            break;
        }
    }
    const VkFormatFeatureFlags cneed = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (gpu.depth_fmt == VK_FORMAT_UNDEFINED || !format_ok(gpu.color_fmt, cneed)) {
        OLOG("required attachment formats unsupported");
        device_shutdown();
        return false;
    }

    const char* dext[1];
    uint32_t ndext = 0;
#ifdef __ANDROID__
    dext[ndext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
#endif
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueFamilyIndex = gpu.qfam,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = ndext,
        .ppEnabledExtensionNames = ndext ? dext : nullptr,
        .pEnabledFeatures = nullptr,
    };
    r = vkCreateDevice(gpu.phys, &dci, nullptr, &gpu.dev);
    if (r != VK_SUCCESS) {
        OLOG("vkCreateDevice failed: %d", r);
        device_shutdown();
        return false;
    }
    load_device(gpu.dev);
    vkGetDeviceQueue(gpu.dev, gpu.qfam, 0, &gpu.queue);
    if (!create_pass()) {
        device_shutdown();
        return false;
    }
    const uint32_t a = gpu.props.apiVersion, d = gpu.props.driverVersion;
    std::snprintf(gpu.renderer, sizeof(gpu.renderer), "Oryon (%s; Vulkan %u.%u.%u)", gpu.props.deviceName,
                  VK_VERSION_MAJOR(a), VK_VERSION_MINOR(a), VK_VERSION_PATCH(a));
    OLOG("%s driver %u.%u.%u depth=%d", gpu.renderer, VK_VERSION_MAJOR(d), VK_VERSION_MINOR(d), VK_VERSION_PATCH(d),
         gpu.depth_fmt);
    return true;
}

void device_shutdown() {
    if (gpu.dev) {
        vkDeviceWaitIdle(gpu.dev);
        if (gpu.pass) vkDestroyRenderPass(gpu.dev, gpu.pass, nullptr);
        vkDestroyDevice(gpu.dev, nullptr);
    }
    if (gpu.instance) vkDestroyInstance(gpu.instance, nullptr);
    gpu.pass = VK_NULL_HANDLE;
    gpu.dev = VK_NULL_HANDLE;
    gpu.queue = VK_NULL_HANDLE;
    gpu.phys = VK_NULL_HANDLE;
    gpu.instance = VK_NULL_HANDLE;
    gpu.depth_fmt = VK_FORMAT_UNDEFINED;
}

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags want) {
    for (VkMemoryPropertyFlags f : {need | want, need}) {
        for (uint32_t i = 0; i < gpu.mem.memoryTypeCount; i++) {
            if ((bits & (1u << i)) && (gpu.mem.memoryTypes[i].propertyFlags & f) == f) return i;
        }
    }
    return UINT32_MAX;
}

VkResult submit(const VkSubmitInfo& si, VkFence fence) {
    std::lock_guard<std::mutex> g(gpu.qlock);
    return vkQueueSubmit(gpu.queue, 1, &si, fence);
}

void barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
             VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
             VkAccessFlags dst_access) {
    VkImageMemoryBarrier b{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}
}
