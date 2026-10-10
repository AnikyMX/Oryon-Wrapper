// EGL front end used by the launcher (libpojavexec.so); prototypes come from Khronos <EGL/egl.h>.
#include "context.hpp"
#include "proc.hpp"
#include <algorithm>
#include <mutex>
#if defined(__ANDROID__)
#include <android/native_window.h>
#endif

using namespace oryon;

namespace {

struct Display {
  bool initialized = false;
} display;
int config_tag;  // the single config handle
const EGLConfig kConfig = &config_tag;

std::mutex egl_lock;
thread_local EGLint egl_error = EGL_SUCCESS;
thread_local EGLenum egl_api = EGL_OPENGL_ES_API;

constexpr EGLint kSurfaceType = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
constexpr EGLint kRenderable = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT | EGL_OPENGL_BIT;

EGLBoolean fail(EGLint e) {
  egl_error = e;
  return EGL_FALSE;
}
EGLBoolean ok() {
  egl_error = EGL_SUCCESS;
  return EGL_TRUE;
}
// 0 when usable, else the EGL error to report.
EGLint check(EGLDisplay dpy) {
  if (dpy != &display) return EGL_BAD_DISPLAY;
  return display.initialized ? 0 : EGL_NOT_INITIALIZED;
}

EGLint depth_bits() {
  switch (dev().depth_format) {
    case VK_FORMAT_D24_UNORM_S8_UINT: return 24;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_D32_SFLOAT: return 32;
    default: return 0;
  }
}

bool config_attrib(EGLint attr, EGLint* v) {
  const VkPhysicalDeviceLimits& lim = dev().props.limits;
  switch (attr) {
    case EGL_BUFFER_SIZE: *v = 32; return true;
    case EGL_RED_SIZE:
    case EGL_GREEN_SIZE:
    case EGL_BLUE_SIZE:
    case EGL_ALPHA_SIZE: *v = 8; return true;
    case EGL_DEPTH_SIZE: *v = depth_bits(); return true;
    case EGL_STENCIL_SIZE: *v = format_has_stencil(dev().depth_format) ? 8 : 0; return true;
    case EGL_CONFIG_ID: *v = 1; return true;
    case EGL_SURFACE_TYPE: *v = kSurfaceType; return true;
    case EGL_RENDERABLE_TYPE:
    case EGL_CONFORMANT: *v = kRenderable; return true;
    case EGL_COLOR_BUFFER_TYPE: *v = EGL_RGB_BUFFER; return true;
    case EGL_CONFIG_CAVEAT:
    case EGL_TRANSPARENT_TYPE: *v = EGL_NONE; return true;
    case EGL_NATIVE_VISUAL_ID:
#if defined(__ANDROID__)
      *v = WINDOW_FORMAT_RGBA_8888;
#else
      *v = 0;
#endif
      return true;
    case EGL_NATIVE_RENDERABLE:
    case EGL_BIND_TO_TEXTURE_RGB:
    case EGL_BIND_TO_TEXTURE_RGBA: *v = EGL_FALSE; return true;
    case EGL_LEVEL:
    case EGL_SAMPLES:
    case EGL_SAMPLE_BUFFERS:
    case EGL_LUMINANCE_SIZE:
    case EGL_ALPHA_MASK_SIZE:
    case EGL_NATIVE_VISUAL_TYPE:
    case EGL_TRANSPARENT_RED_VALUE:
    case EGL_TRANSPARENT_GREEN_VALUE:
    case EGL_TRANSPARENT_BLUE_VALUE:
    case EGL_MIN_SWAP_INTERVAL: *v = 0; return true;
    case EGL_MAX_SWAP_INTERVAL: *v = 1; return true;
    case EGL_MAX_PBUFFER_WIDTH:
    case EGL_MAX_PBUFFER_HEIGHT: *v = static_cast<EGLint>(lim.maxImageDimension2D); return true;
    case EGL_MAX_PBUFFER_PIXELS:
      *v = static_cast<EGLint>(std::min<uint64_t>(uint64_t(lim.maxImageDimension2D) * lim.maxImageDimension2D, 0x7fffffff));
      return true;
    default: return false;
  }
}

bool config_matches(const EGLint* attrib) {
  for (const EGLint* a = attrib; a && a[0] != EGL_NONE; a += 2) {
    EGLint have = 0;
    if (a[1] == EGL_DONT_CARE || !config_attrib(a[0], &have)) continue;  // unknown criteria are ignored
    switch (a[0]) {
      case EGL_SURFACE_TYPE:
      case EGL_RENDERABLE_TYPE:
      case EGL_CONFORMANT:
        if ((have & a[1]) != a[1]) return false;
        break;
      case EGL_NATIVE_VISUAL_ID:
      case EGL_MAX_PBUFFER_WIDTH:
      case EGL_MAX_PBUFFER_HEIGHT:
      case EGL_MAX_PBUFFER_PIXELS:
        break;
      case EGL_BUFFER_SIZE:
      case EGL_RED_SIZE:
      case EGL_GREEN_SIZE:
      case EGL_BLUE_SIZE:
      case EGL_ALPHA_SIZE:
      case EGL_DEPTH_SIZE:
      case EGL_STENCIL_SIZE:
      case EGL_SAMPLES:
      case EGL_SAMPLE_BUFFERS:
      case EGL_LUMINANCE_SIZE:
      case EGL_ALPHA_MASK_SIZE:
        if (have < a[1]) return false;
        break;
      default:
        if (have != a[1]) return false;
    }
  }
  return true;
}

void release(Surface* s) {
  if (s && --s->bound == 0 && s->doomed) surf_destroy(s);
}

}  // namespace

OEGL_API EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id) {
  (void)display_id;
  return &display;
}

OEGL_API EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  if (dpy != &display) return fail(EGL_BAD_DISPLAY);
  if (!device()) return fail(EGL_NOT_INITIALIZED);
  display.initialized = true;
  if (major) *major = 1;
  if (minor) *minor = 4;
  return ok();
}

OEGL_API EGLBoolean eglTerminate(EGLDisplay dpy) {
  if (dpy != &display) return fail(EGL_BAD_DISPLAY);
  display.initialized = false;
  return ok();
}

OEGL_API EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size,
                                    EGLint *num_config) {
  if (EGLint e = check(dpy)) return fail(e);
  if (!num_config) return fail(EGL_BAD_PARAMETER);
  *num_config = 0;
  if (config_matches(attrib_list)) {
    if (!configs) *num_config = 1;
    else if (config_size > 0) {
      configs[0] = kConfig;
      *num_config = 1;
    }
  }
  return ok();
}

OEGL_API EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value) {
  if (EGLint e = check(dpy)) return fail(e);
  if (config != kConfig) return fail(EGL_BAD_CONFIG);
  if (!value) return fail(EGL_BAD_PARAMETER);
  return config_attrib(attribute, value) ? ok() : fail(EGL_BAD_ATTRIBUTE);
}

OEGL_API EGLBoolean eglBindAPI(EGLenum api) {
  if (api != EGL_OPENGL_ES_API && api != EGL_OPENGL_API) return fail(EGL_BAD_PARAMETER);
  egl_api = api;
  return ok();
}

OEGL_API EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context,
                                     const EGLint *attrib_list) {
  (void)share_context;  // one Vulkan device: object sharing arrives with GL objects
  if (EGLint e = check(dpy)) return fail(e), EGL_NO_CONTEXT;
  if (config != kConfig) return fail(EGL_BAD_CONFIG), EGL_NO_CONTEXT;
  EGLint version = 1;
  for (const EGLint* a = attrib_list; a && a[0] != EGL_NONE; a += 2)
    if (a[0] == EGL_CONTEXT_CLIENT_VERSION) version = a[1];
  std::lock_guard<std::mutex> lock(egl_lock);
  Context* c = ctx_create(version, egl_api);
  if (!c) return fail(EGL_BAD_ALLOC), EGL_NO_CONTEXT;
  ok();
  return c;
}

OEGL_API EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
  if (EGLint e = check(dpy)) return fail(e);
  if (!ctx) return fail(EGL_BAD_CONTEXT);
  std::lock_guard<std::mutex> lock(egl_lock);
  Context* c = static_cast<Context*>(ctx);
  if (c->bound) c->doomed = true;
  else ctx_destroy(c);
  return ok();
}

OEGL_API EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win,
                                           const EGLint *attrib_list) {
  (void)attrib_list;
  if (EGLint e = check(dpy)) return fail(e), EGL_NO_SURFACE;
  if (config != kConfig) return fail(EGL_BAD_CONFIG), EGL_NO_SURFACE;
  if (!win) return fail(EGL_BAD_NATIVE_WINDOW), EGL_NO_SURFACE;
  std::lock_guard<std::mutex> lock(egl_lock);
  Surface* s = surf_create(true, reinterpret_cast<void*>(win), 0, 0);
  if (!s) return fail(EGL_BAD_NATIVE_WINDOW), EGL_NO_SURFACE;
  ok();
  return s;
}

OEGL_API EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list) {
  if (EGLint e = check(dpy)) return fail(e), EGL_NO_SURFACE;
  if (config != kConfig) return fail(EGL_BAD_CONFIG), EGL_NO_SURFACE;
  EGLint w = 0, h = 0;
  for (const EGLint* a = attrib_list; a && a[0] != EGL_NONE; a += 2) {
    if (a[0] == EGL_WIDTH) w = a[1];
    else if (a[0] == EGL_HEIGHT) h = a[1];
  }
  const EGLint max = static_cast<EGLint>(dev().props.limits.maxImageDimension2D);
  if (w < 0 || h < 0) return fail(EGL_BAD_PARAMETER), EGL_NO_SURFACE;
  if (w > max || h > max) return fail(EGL_BAD_MATCH), EGL_NO_SURFACE;
  std::lock_guard<std::mutex> lock(egl_lock);
  Surface* s = surf_create(false, nullptr, static_cast<uint32_t>(std::max(w, 1)), static_cast<uint32_t>(std::max(h, 1)));
  if (!s) return fail(EGL_BAD_ALLOC), EGL_NO_SURFACE;
  ok();
  return s;
}

OEGL_API EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
  if (EGLint e = check(dpy)) return fail(e);
  if (!surface) return fail(EGL_BAD_SURFACE);
  std::lock_guard<std::mutex> lock(egl_lock);
  Surface* s = static_cast<Surface*>(surface);
  if (s->bound) s->doomed = true;
  else surf_destroy(s);
  return ok();
}

OEGL_API EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
  Context* c = static_cast<Context*>(ctx);
  if (c) {
    if (EGLint e = check(dpy)) return fail(e);
  } else if (draw || read) {
    return fail(EGL_BAD_MATCH);
  }
  if (!draw != !read) return fail(EGL_BAD_MATCH);
  Surface* nd = static_cast<Surface*>(draw);
  Surface* nr = static_cast<Surface*>(read);
  std::lock_guard<std::mutex> lock(egl_lock);
  Context* old = current;
  if (c && c->bound && c != old) return fail(EGL_BAD_ACCESS);  // current on another thread
  if (old == c && (!c || (c->draw == nd && c->read == nr))) return ok();
  if (old) {
    ctx_submit(*old, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, false);
    Surface *od = old->draw, *orr = old->read;
    old->draw = old->read = nullptr;
    old->bound = false;
    release(od);
    release(orr);
    if (old != c && old->doomed) ctx_destroy(old);
  }
  current = c;
  if (c) {
    c->bound = true;
    if (nd) ++nd->bound;
    if (nr) ++nr->bound;
    ctx_bind(*c, nd, nr);
  }
  return ok();
}

OEGL_API EGLContext eglGetCurrentContext(void) {
  return current ? current : EGL_NO_CONTEXT;
}

OEGL_API EGLSurface eglGetCurrentSurface(EGLint readdraw) {
  if (readdraw != EGL_DRAW && readdraw != EGL_READ) return fail(EGL_BAD_PARAMETER), EGL_NO_SURFACE;
  if (!current) return EGL_NO_SURFACE;
  Surface* s = readdraw == EGL_DRAW ? current->draw : current->read;
  return s ? s : EGL_NO_SURFACE;
}

OEGL_API EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value) {
  if (EGLint e = check(dpy)) return fail(e);
  if (!surface) return fail(EGL_BAD_SURFACE);
  if (!value) return fail(EGL_BAD_PARAMETER);
  const Surface* s = static_cast<const Surface*>(surface);
  switch (attribute) {
    case EGL_WIDTH: *value = static_cast<EGLint>(s->color.width); break;
    case EGL_HEIGHT: *value = static_cast<EGLint>(s->color.height); break;
    case EGL_CONFIG_ID: *value = 1; break;
    case EGL_RENDER_BUFFER: *value = EGL_BACK_BUFFER; break;
    case EGL_SWAP_BEHAVIOR: *value = EGL_BUFFER_DESTROYED; break;
    case EGL_LARGEST_PBUFFER:
    case EGL_MIPMAP_TEXTURE: *value = EGL_FALSE; break;
    case EGL_TEXTURE_FORMAT:
    case EGL_TEXTURE_TARGET: *value = EGL_NO_TEXTURE; break;
    case EGL_MULTISAMPLE_RESOLVE: *value = EGL_MULTISAMPLE_RESOLVE_DEFAULT; break;
    case EGL_HORIZONTAL_RESOLUTION:
    case EGL_VERTICAL_RESOLUTION:
    case EGL_PIXEL_ASPECT_RATIO: *value = EGL_UNKNOWN; break;
    default: return fail(EGL_BAD_ATTRIBUTE);
  }
  return ok();
}

OEGL_API EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  if (EGLint e = check(dpy)) return fail(e);
  Surface* s = static_cast<Surface*>(surface);
  if (!s || !current || current->draw != s) return fail(EGL_BAD_SURFACE);
  const EGLint e = ctx_swap(*current, *s);
  return e == EGL_SUCCESS ? ok() : fail(e);
}

OEGL_API EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval) {
  if (EGLint e = check(dpy)) return fail(e);
  if (!current) return fail(EGL_BAD_CONTEXT);
  Surface* s = current->draw;
  if (!s) return fail(EGL_BAD_SURFACE);
  interval = std::clamp(interval, 0, 1);
  if (s->window && s->interval != interval) s->stale = true;
  s->interval = interval;
  return ok();
}

OEGL_API EGLint eglGetError(void) {
  const EGLint e = egl_error;
  egl_error = EGL_SUCCESS;
  return e;
}

OEGL_API EGLBoolean eglReleaseThread(void) {
  if (current) eglMakeCurrent(&display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  egl_api = EGL_OPENGL_ES_API;
  return ok();
}

OEGL_API __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname) {
  return procname ? proc_lookup(procname) : nullptr;
}
