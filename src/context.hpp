// GL context (state + Vulkan command recording) and EGL surface (default framebuffer + swapchain).
#pragma once
#include "device.hpp"
#include "oryon.hpp"
#include <EGL/egl.h>

namespace oryon {

// Default framebuffer images keep GL row order (row 0 = bottom); the present blit flips Y.
struct Surface {
  bool window = false;
  void* native = nullptr;
  Image color, depth;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkExtent2D extent{};
  std::vector<VkImage> images;
  std::vector<VkSemaphore> rendered;           // per swapchain image, waited by present
  VkSemaphore acquired[2]{};                    // per presenting context frame slot
  const void* presenter = nullptr;              // context owning `acquired`
  int interval = 1;
  bool stale = false;  // swapchain must be recreated before the next acquire
  int bound = 0;       // contexts using it as draw/read
  bool doomed = false; // eglDestroySurface while bound
};

struct Context {
  struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool busy = false;
  } frames[2];
  uint32_t frame = 0;
  VkCommandBuffer cmd = VK_NULL_HANDLE;  // recording, or null
  Surface* draw = nullptr;
  Surface* read = nullptr;
  bool in_pass = false;
  uint32_t pending = 0;           // CLEAR_* bits folded into the next render pass load ops
  VkClearValue pending_values[2]{};
  Buffer readback;
  bool bound = false, doomed = false, fresh = true;
  EGLint client_version = 1;
  EGLenum api = EGL_OPENGL_ES_API;

  GLenum error = GL_NO_ERROR;
  GLfloat clear_color[4]{};
  GLfloat clear_depth = 1.0f;
  GLint clear_stencil = 0;
  GLint viewport[4]{};
  GLint scissor[4]{};
  uint64_t caps = 0;
  GLenum extra_caps[16]{};
  uint32_t extra_count = 0;
  char renderer[320]{};
  char version[48]{};
};

extern thread_local constinit Context* current;  // constinit: direct TLS access, no wrapper call

inline void gl_error(Context* c, GLenum e) {
  if (c->error == GL_NO_ERROR) c->error = e;
}

void gl_defaults(Context& c);  // GL initial state (gl_core.cpp)
Context* ctx_create(EGLint client_version, EGLenum api);
void ctx_destroy(Context* c);
VkCommandBuffer ctx_cmd(Context& c);
void ctx_begin_pass(Context& c);
void ctx_end_pass(Context& c);
void ctx_submit(Context& c, VkSemaphore wait, VkPipelineStageFlags stage, VkSemaphore signal, bool block);
void ctx_bind(Context& c, Surface* draw, Surface* read);
EGLint ctx_swap(Context& c, Surface& s);

Surface* surf_create(bool window, void* native, uint32_t w, uint32_t h);
void surf_destroy(Surface* s);
bool surf_swapchain(Surface& s);

}  // namespace oryon
