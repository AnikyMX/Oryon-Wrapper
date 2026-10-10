#include "vk/dispatch.hpp"
#include "oryon.hpp"
#include <dlfcn.h>

namespace ovk {
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define OVK_DEF(n) PFN_##n n = nullptr;
OVK_GLOBAL(OVK_DEF)
OVK_INSTANCE(OVK_DEF)
OVK_DEVICE(OVK_DEF)
#undef OVK_DEF

bool load_global() {
    if (vkGetInstanceProcAddr) return true;
    void* lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        OLOG("dlopen libvulkan failed: %s", dlerror());
        return false;
    }
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr) return false;
#define OVK_LOAD(n) n = reinterpret_cast<PFN_##n>(vkGetInstanceProcAddr(nullptr, #n));
    OVK_GLOBAL(OVK_LOAD)
#undef OVK_LOAD
    return vkCreateInstance != nullptr;
}

void load_instance(VkInstance inst) {
#define OVK_LOAD(n) n = reinterpret_cast<PFN_##n>(vkGetInstanceProcAddr(inst, #n));
    OVK_INSTANCE(OVK_LOAD)
#undef OVK_LOAD
}

void load_device(VkDevice dev) {
#define OVK_LOAD(n) n = reinterpret_cast<PFN_##n>(vkGetDeviceProcAddr(dev, #n));
    OVK_DEVICE(OVK_LOAD)
#undef OVK_LOAD
}
}
