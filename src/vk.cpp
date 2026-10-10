#include "vk.hpp"
#include "oryon.hpp"
#include <dlfcn.h>
#include <initializer_list>

namespace oryon {

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define ORYON_VK_DEF(n) PFN_##n n = nullptr;
ORYON_VK_GLOBAL(ORYON_VK_DEF)
ORYON_VK_GLOBAL_OPT(ORYON_VK_DEF)
ORYON_VK_INSTANCE(ORYON_VK_DEF)
ORYON_VK_INSTANCE_OPT(ORYON_VK_DEF)
ORYON_VK_DEVICE(ORYON_VK_DEF)
ORYON_VK_DEVICE_OPT(ORYON_VK_DEF)
#undef ORYON_VK_DEF

#define ORYON_VK_REQ(n) ok &= (n = reinterpret_cast<PFN_##n>(LOAD(n))) != nullptr;
#define ORYON_VK_OPT(n) n = reinterpret_cast<PFN_##n>(LOAD(n));

bool vkg_load() {
  if (vkGetInstanceProcAddr) return true;
  void* lib = nullptr;
  for (const char* name : {"libvulkan.so", "libvulkan.so.1"})
    if ((lib = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
  if (!lib) {
    ORYON_LOG("cannot dlopen the Vulkan loader");
    return false;
  }
  vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
  if (!vkGetInstanceProcAddr) return false;
  bool ok = true;
#define LOAD(n) vkGetInstanceProcAddr(VK_NULL_HANDLE, #n)
  ORYON_VK_GLOBAL(ORYON_VK_REQ)
  ORYON_VK_GLOBAL_OPT(ORYON_VK_OPT)
#undef LOAD
  return ok;
}

bool vki_load(VkInstance inst) {
  bool ok = true;
#define LOAD(n) vkGetInstanceProcAddr(inst, #n)
  ORYON_VK_INSTANCE(ORYON_VK_REQ)
  ORYON_VK_INSTANCE_OPT(ORYON_VK_OPT)
#undef LOAD
  return ok;
}

bool vkd_load(VkDevice dev) {
  bool ok = true;
#define LOAD(n) vkGetDeviceProcAddr(dev, #n)
  ORYON_VK_DEVICE(ORYON_VK_REQ)
  ORYON_VK_DEVICE_OPT(ORYON_VK_OPT)
#undef LOAD
  return ok;
}

}  // namespace oryon
