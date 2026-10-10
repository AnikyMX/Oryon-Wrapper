#include "device.hpp"
#include "oryon.hpp"
#include "shaders.inc"
#include <cstring>

namespace oryon {

namespace {

bool has_ext(const std::vector<VkExtensionProperties>& list, const char* name) {
  for (const auto& e : list)
    if (!std::strcmp(e.extensionName, name)) return true;
  return false;
}

int rank(VkPhysicalDeviceType t) {
  switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
    default: return 0;
  }
}

bool create_fixed_function(Device& d);

bool create(Device& d) {
  if (!vkg_load()) return false;
  uint32_t api = VK_API_VERSION_1_0;
  if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&api);
  api = api >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;

  uint32_t n = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> iext(n);
  vkEnumerateInstanceExtensionProperties(nullptr, &n, iext.data());
#if defined(__ANDROID__)
  const char* exts[2] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
#else
  const char* exts[2] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
#endif
  d.surface_ext = has_ext(iext, exts[0]) && has_ext(iext, exts[1]);

  VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                        .pNext = nullptr,
                        .pApplicationName = "Oryon",
                        .applicationVersion = 1,
                        .pEngineName = "Oryon",
                        .engineVersion = 1,
                        .apiVersion = api};
  VkInstanceCreateInfo ici{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                           .pNext = nullptr,
                           .flags = 0,
                           .pApplicationInfo = &app,
                           .enabledLayerCount = 0,
                           .ppEnabledLayerNames = nullptr,
                           .enabledExtensionCount = d.surface_ext ? 2u : 0u,
                           .ppEnabledExtensionNames = exts};
  if (vkCreateInstance(&ici, nullptr, &d.instance) != VK_SUCCESS || !vki_load(d.instance)) return false;

  vkEnumeratePhysicalDevices(d.instance, &n, nullptr);
  std::vector<VkPhysicalDevice> gpus(n);
  vkEnumeratePhysicalDevices(d.instance, &n, gpus.data());
  int best = -1;
  for (VkPhysicalDevice g : gpus) {
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qf.data());
    for (uint32_t i = 0; i < qn; ++i) {
      if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(g, &p);
      if (rank(p.deviceType) > best) {
        best = rank(p.deviceType);
        d.gpu = g;
        d.family = i;
        d.props = p;
      }
      break;
    }
  }
  if (!d.gpu) return false;
  vkGetPhysicalDeviceMemoryProperties(d.gpu, &d.memory);

  vkEnumerateDeviceExtensionProperties(d.gpu, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> dext(n);
  vkEnumerateDeviceExtensionProperties(d.gpu, nullptr, &n, dext.data());
  const char* swapchain = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  d.swapchain_ext = d.surface_ext && has_ext(dext, swapchain);

  VkPhysicalDeviceFeatures have;
  vkGetPhysicalDeviceFeatures(d.gpu, &have);
  d.features.wideLines = have.wideLines;
  d.features.logicOp = have.logicOp;
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo qci{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                              .pNext = nullptr,
                              .flags = 0,
                              .queueFamilyIndex = d.family,
                              .queueCount = 1,
                              .pQueuePriorities = &priority};
  VkDeviceCreateInfo dci{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                         .pNext = nullptr,
                         .flags = 0,
                         .queueCreateInfoCount = 1,
                         .pQueueCreateInfos = &qci,
                         .enabledLayerCount = 0,
                         .ppEnabledLayerNames = nullptr,
                         .enabledExtensionCount = d.swapchain_ext ? 1u : 0u,
                         .ppEnabledExtensionNames = &swapchain,
                         .pEnabledFeatures = &d.features};
  if (vkCreateDevice(d.gpu, &dci, nullptr, &d.dev) != VK_SUCCESS || !vkd_load(d.dev)) return false;
  vkGetDeviceQueue(d.dev, d.family, 0, &d.queue);

  for (VkFormat f : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT}) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(d.gpu, f, &fp);
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
      d.depth_format = f;
      break;
    }
  }

  VkCommandPoolCreateInfo pci{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                              .pNext = nullptr,
                              .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                              .queueFamilyIndex = d.family};
  VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
  if (vkCreateCommandPool(d.dev, &pci, nullptr, &d.once_pool) != VK_SUCCESS ||
      vkCreateFence(d.dev, &fci, nullptr, &d.once_fence) != VK_SUCCESS)
    return false;
  VkCommandBufferAllocateInfo cai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                  .pNext = nullptr,
                                  .commandPool = d.once_pool,
                                  .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                  .commandBufferCount = 1};
  if (vkAllocateCommandBuffers(d.dev, &cai, &d.once_cmd) != VK_SUCCESS) return false;
  if (!create_fixed_function(d)) return false;

  ORYON_LOG("Vulkan device '%s' api %u.%u.%u (instance %u.%u), swapchain=%d",
            d.props.deviceName, VK_VERSION_MAJOR(d.props.apiVersion), VK_VERSION_MINOR(d.props.apiVersion),
            VK_VERSION_PATCH(d.props.apiVersion), VK_VERSION_MAJOR(api), VK_VERSION_MINOR(api), d.swapchain_ext);
  return true;
}

// Layouts, shader modules and constant resources shared by every fixed-function draw.
bool create_fixed_function(Device& d) {
  const VkDescriptorSetLayoutBinding bindings[3] = {
      {.binding = 0,
       .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
       .pImmutableSamplers = nullptr},
      {.binding = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
       .pImmutableSamplers = nullptr},
      {.binding = 2,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
       .pImmutableSamplers = nullptr}};
  VkDescriptorSetLayoutCreateInfo sci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                      .pNext = nullptr,
                                      .flags = 0,
                                      .bindingCount = 3,
                                      .pBindings = bindings};
  if (vkCreateDescriptorSetLayout(d.dev, &sci, nullptr, &d.set_layout) != VK_SUCCESS) return false;
  VkPipelineLayoutCreateInfo lci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                 .pNext = nullptr,
                                 .flags = 0,
                                 .setLayoutCount = 1,
                                 .pSetLayouts = &d.set_layout,
                                 .pushConstantRangeCount = 0,
                                 .pPushConstantRanges = nullptr};
  if (vkCreatePipelineLayout(d.dev, &lci, nullptr, &d.layout) != VK_SUCCESS) return false;
  VkShaderModuleCreateInfo vci{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                               .pNext = nullptr,
                               .flags = 0,
                               .codeSize = sizeof kFFVert,
                               .pCode = kFFVert};
  VkShaderModuleCreateInfo fci{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                               .pNext = nullptr,
                               .flags = 0,
                               .codeSize = sizeof kFFFrag,
                               .pCode = kFFFrag};
  VkPipelineCacheCreateInfo pci{.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                .pNext = nullptr,
                                .flags = 0,
                                .initialDataSize = 0,
                                .pInitialData = nullptr};
  if (vkCreateShaderModule(d.dev, &vci, nullptr, &d.vert) != VK_SUCCESS ||
      vkCreateShaderModule(d.dev, &fci, nullptr, &d.frag) != VK_SUCCESS ||
      vkCreatePipelineCache(d.dev, &pci, nullptr, &d.cache) != VK_SUCCESS)
    return false;

  VkFormatProperties fp;  // GL_SHORT texcoords (MC lightmap) need a scaled vertex format; not mandatory in Vulkan
  vkGetPhysicalDeviceFormatProperties(d.gpu, VK_FORMAT_R16G16_SSCALED, &fp);
  if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) ORYON_LOG("warning: R16G16_SSCALED vertex fetch unsupported");

  constexpr uint32_t quads = kQuadBatch / 4;
  if (!dev_host_buffer(d.quads, quads * 6 * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
    return false;
  auto* idx = static_cast<uint16_t*>(d.quads.map);
  for (uint32_t q = 0; q < quads; ++q) {  // GL quad (0,1,2,3) -> triangles (0,1,2) (2,3,0), winding kept
    const uint16_t v = static_cast<uint16_t>(q * 4);
    const uint16_t tri[6] = {v, uint16_t(v + 1), uint16_t(v + 2), uint16_t(v + 2), uint16_t(v + 3), v};
    std::memcpy(idx + q * 6, tri, sizeof tri);
  }

  if (!dev_image(d.white, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT))
    return false;
  VkCommandBuffer cmd = dev_once_begin();
  const VkClearColorValue one{.float32 = {1.0f, 1.0f, 1.0f, 1.0f}};
  const VkImageSubresourceRange range{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                      .baseMipLevel = 0,
                                      .levelCount = 1,
                                      .baseArrayLayer = 0,
                                      .layerCount = 1};
  image_barrier(cmd, d.white.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  vkCmdClearColorImage(cmd, d.white.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &range);
  image_barrier(cmd, d.white.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
  dev_once_end(cmd);
  return true;
}

}  // namespace

Device g_device;

Device* device() {
  static const bool ok = create(g_device);
  return ok ? &g_device : nullptr;
}

bool format_has_stencil(VkFormat f) {
  return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT ||
         f == VK_FORMAT_S8_UINT;
}

uint32_t dev_memory_type(uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags want) {
  const VkPhysicalDeviceMemoryProperties& m = dev().memory;
  uint32_t fallback = UINT32_MAX;
  for (uint32_t i = 0; i < m.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags f = m.memoryTypes[i].propertyFlags;
    if (!(bits & (1u << i)) || (f & need) != need) continue;
    if ((f & want) == want) return i;
    if (fallback == UINT32_MAX) fallback = i;
  }
  return fallback;
}

bool dev_image(Image& im, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags aspect,
               uint32_t levels) {
  Device& d = dev();
  VkImageCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                       .pNext = nullptr,
                       .flags = 0,
                       .imageType = VK_IMAGE_TYPE_2D,
                       .format = format,
                       .extent = {w, h, 1},
                       .mipLevels = levels,
                       .arrayLayers = 1,
                       .samples = VK_SAMPLE_COUNT_1_BIT,
                       .tiling = VK_IMAGE_TILING_OPTIMAL,
                       .usage = usage,
                       .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                       .queueFamilyIndexCount = 0,
                       .pQueueFamilyIndices = nullptr,
                       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
  if (vkCreateImage(d.dev, &ci, nullptr, &im.image) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(d.dev, im.image, &req);
  VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                          .pNext = nullptr,
                          .allocationSize = req.size,
                          .memoryTypeIndex = dev_memory_type(req.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(d.dev, &ai, nullptr, &im.memory) != VK_SUCCESS ||
      vkBindImageMemory(d.dev, im.image, im.memory, 0) != VK_SUCCESS) {
    dev_destroy(im);
    return false;
  }
  VkImageViewCreateInfo vi{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                           .pNext = nullptr,
                           .flags = 0,
                           .image = im.image,
                           .viewType = VK_IMAGE_VIEW_TYPE_2D,
                           .format = format,
                           .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                           .subresourceRange = {aspect, 0, levels, 0, 1}};
  if (vkCreateImageView(d.dev, &vi, nullptr, &im.view) != VK_SUCCESS) {
    dev_destroy(im);
    return false;
  }
  im.format = format;
  im.width = w;
  im.height = h;
  return true;
}

void dev_destroy(Image& im) {
  Device& d = dev();
  if (im.view) vkDestroyImageView(d.dev, im.view, nullptr);
  if (im.image) vkDestroyImage(d.dev, im.image, nullptr);
  if (im.memory) vkFreeMemory(d.dev, im.memory, nullptr);
  im = Image{};
}

bool dev_host_buffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags need,
                     VkMemoryPropertyFlags want) {
  Device& d = dev();
  VkBufferCreateInfo ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                        .pNext = nullptr,
                        .flags = 0,
                        .size = size,
                        .usage = usage,
                        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                        .queueFamilyIndexCount = 0,
                        .pQueueFamilyIndices = nullptr};
  if (vkCreateBuffer(d.dev, &ci, nullptr, &b.buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(d.dev, b.buffer, &req);
  const uint32_t type = dev_memory_type(req.memoryTypeBits, need, want);
  VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                          .pNext = nullptr,
                          .allocationSize = req.size,
                          .memoryTypeIndex = type};
  if (type == UINT32_MAX || vkAllocateMemory(d.dev, &ai, nullptr, &b.memory) != VK_SUCCESS ||
      vkBindBufferMemory(d.dev, b.buffer, b.memory, 0) != VK_SUCCESS ||
      vkMapMemory(d.dev, b.memory, 0, VK_WHOLE_SIZE, 0, &b.map) != VK_SUCCESS) {
    dev_destroy(b);
    return false;
  }
  b.size = size;
  b.coherent = d.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  return true;
}

void dev_destroy(Buffer& b) {
  Device& d = dev();
  if (b.buffer) vkDestroyBuffer(d.dev, b.buffer, nullptr);
  if (b.memory) vkFreeMemory(d.dev, b.memory, nullptr);
  b = Buffer{};
}

VkRenderPass dev_pass(VkFormat color, VkFormat depth, uint32_t clear) {
  Device& d = dev();
  std::lock_guard<std::mutex> lock(d.pass_lock);
  for (const auto& p : d.passes)
    if (p.color == color && p.depth == depth && p.clear == clear) return p.pass;

  const auto load = [clear](uint32_t bit) {
    return (clear & bit) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
  };
  const VkAttachmentDescription att[2] = {
      {.flags = 0,
       .format = color,
       .samples = VK_SAMPLE_COUNT_1_BIT,
       .loadOp = load(CLEAR_COLOR),
       .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
       .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
       .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {.flags = 0,
       .format = depth,
       .samples = VK_SAMPLE_COUNT_1_BIT,
       .loadOp = load(CLEAR_DEPTH),
       .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
       .stencilLoadOp = load(CLEAR_STENCIL),
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
       .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
       .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
  const VkAttachmentReference cref{.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  const VkAttachmentReference dref{.attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  const bool has_depth = depth != VK_FORMAT_UNDEFINED;
  VkSubpassDescription sub{.flags = 0,
                           .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                           .inputAttachmentCount = 0,
                           .pInputAttachments = nullptr,
                           .colorAttachmentCount = 1,
                           .pColorAttachments = &cref,
                           .pResolveAttachments = nullptr,
                           .pDepthStencilAttachment = has_depth ? &dref : nullptr,
                           .preserveAttachmentCount = 0,
                           .pPreserveAttachments = nullptr};
  // Successive passes / transfers on the same attachments are ordered by these external dependencies.
  const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  const VkAccessFlags writes = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  const VkAccessFlags access = writes | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
  const VkSubpassDependency deps[2] = {
      {.srcSubpass = VK_SUBPASS_EXTERNAL,
       .dstSubpass = 0,
       .srcStageMask = stages | VK_PIPELINE_STAGE_TRANSFER_BIT,
       .dstStageMask = stages,
       .srcAccessMask = writes | VK_ACCESS_TRANSFER_WRITE_BIT,
       .dstAccessMask = access,
       .dependencyFlags = 0},
      {.srcSubpass = 0,
       .dstSubpass = VK_SUBPASS_EXTERNAL,
       .srcStageMask = stages,
       .dstStageMask = stages | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
       .srcAccessMask = writes,
       .dstAccessMask = access | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
       .dependencyFlags = 0}};
  VkRenderPassCreateInfo ci{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                            .pNext = nullptr,
                            .flags = 0,
                            .attachmentCount = has_depth ? 2u : 1u,
                            .pAttachments = att,
                            .subpassCount = 1,
                            .pSubpasses = &sub,
                            .dependencyCount = 2,
                            .pDependencies = deps};
  VkRenderPass rp = VK_NULL_HANDLE;
  if (vkCreateRenderPass(d.dev, &ci, nullptr, &rp) != VK_SUCCESS) return VK_NULL_HANDLE;
  d.passes.push_back({color, depth, clear, rp});
  return rp;
}

VkCommandBuffer dev_once_begin() {
  Device& d = dev();
  d.once_lock.lock();
  vkResetCommandPool(d.dev, d.once_pool, 0);
  VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                              .pNext = nullptr,
                              .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                              .pInheritanceInfo = nullptr};
  vkBeginCommandBuffer(d.once_cmd, &bi);
  return d.once_cmd;
}

void dev_once_end(VkCommandBuffer cmd) {
  Device& d = dev();
  vkEndCommandBuffer(cmd);
  VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                  .pNext = nullptr,
                  .waitSemaphoreCount = 0,
                  .pWaitSemaphores = nullptr,
                  .pWaitDstStageMask = nullptr,
                  .commandBufferCount = 1,
                  .pCommandBuffers = &cmd,
                  .signalSemaphoreCount = 0,
                  .pSignalSemaphores = nullptr};
  if (dev_submit(si, d.once_fence) == VK_SUCCESS) {
    vkWaitForFences(d.dev, 1, &d.once_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(d.dev, 1, &d.once_fence);
  }
  d.once_lock.unlock();
}

VkResult dev_submit(const VkSubmitInfo& info, VkFence fence) {
  Device& d = dev();
  std::lock_guard<std::mutex> lock(d.queue_lock);
  const VkResult r = vkQueueSubmit(d.queue, 1, &info, fence);
  if (r != VK_SUCCESS) ORYON_LOG("vkQueueSubmit failed: %d", r);
  return r;
}

VkResult dev_present(const VkPresentInfoKHR& info) {
  Device& d = dev();
  std::lock_guard<std::mutex> lock(d.queue_lock);
  return vkQueuePresentKHR(d.queue, &info);
}

void dev_idle() {
  Device& d = dev();
  std::lock_guard<std::mutex> lock(d.queue_lock);
  vkDeviceWaitIdle(d.dev);
}

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                   VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                   VkAccessFlags dst_access, uint32_t level, uint32_t levels) {
  VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                         .pNext = nullptr,
                         .srcAccessMask = src_access,
                         .dstAccessMask = dst_access,
                         .oldLayout = from,
                         .newLayout = to,
                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                         .image = image,
                         .subresourceRange = {aspect, level, levels, 0, 1}};
  vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace oryon
