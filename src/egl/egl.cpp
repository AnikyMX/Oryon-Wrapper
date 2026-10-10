#include <EGL/egl.h>
#include "gl/context.hpp"
#include "oryon.hpp"
#include "vk/frame.hpp"
#include <algorithm>
#include <climits>
#include <cstring>
#include <mutex>
#include <vector>
#ifdef __ANDROID__
#include <android/native_window.h>
#endif

// Single display, single config (RGBA8888, depth 24/32, stencil 8, window + pbuffer).
namespace {
struct Surface {
    ovk::Target target;
    EGLint type = 0;  // EGL_WINDOW_BIT or EGL_PBUFFER_BIT
    EGLint width = 0, height = 0;
    int bound = 0;      // contexts using it as draw or read surface
    bool dead = false;  // destroyed while bound; freed on release
};

struct Context {
    gl::Context gl;
    EGLenum api = EGL_OPENGL_ES_API;
    EGLint version = 1;
    Surface* draw = nullptr;
    Surface* read = nullptr;
    bool current = false;
    bool dead = false;
};

struct Display {
    std::mutex lock;
    bool initialized = false;
    std::vector<Surface*> surfaces;
    std::vector<Context*> contexts;
};

Display g_dpy;
int g_config;
const EGLConfig kConfig = &g_config;
constexpr EGLint kSurfaceTypes = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
constexpr EGLint kRenderable = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT | EGL_OPENGL_BIT;

thread_local EGLint t_error = EGL_SUCCESS;
thread_local EGLenum t_api = EGL_OPENGL_ES_API;
thread_local Context* t_ctx = nullptr;

template <typename T>
T fail(EGLint e, T r) {
    t_error = e;
    return r;
}

EGLBoolean ok() {
    t_error = EGL_SUCCESS;
    return EGL_TRUE;
}

EGLint check(EGLDisplay dpy) {
    if (dpy != &g_dpy) return EGL_BAD_DISPLAY;
    return g_dpy.initialized ? EGL_SUCCESS : EGL_NOT_INITIALIZED;
}

Surface* surface_of(EGLSurface s) {
    for (Surface* p : g_dpy.surfaces)
        if (p == s && !p->dead) return p;
    return nullptr;
}

Context* context_of(EGLContext c) {
    for (Context* p : g_dpy.contexts)
        if (p == c && !p->dead) return p;
    return nullptr;
}

EGLint depth_bits() { return ovk::gpu.depth_fmt == VK_FORMAT_D24_UNORM_S8_UINT ? 24 : 32; }

void surface_size(const Surface* s, EGLint& w, EGLint& h) {
    w = s->type == EGL_PBUFFER_BIT ? s->width : EGLint(s->target.w);
    h = s->type == EGL_PBUFFER_BIT ? s->height : EGLint(s->target.h);
}

void free_surface(Surface* s) {
    {  // queued work of any context may still use the images
        std::lock_guard<std::mutex> g(ovk::gpu.qlock);
        ovk::vkQueueWaitIdle(ovk::gpu.queue);
    }
#ifdef __ANDROID__
    ovk::swapchain_release(s->target);
#endif
    ovk::target_release(s->target);
    g_dpy.surfaces.erase(std::find(g_dpy.surfaces.begin(), g_dpy.surfaces.end(), s));
    delete s;
}

void free_context(Context* c) {
    c->gl.rec.destroy();
    g_dpy.contexts.erase(std::find(g_dpy.contexts.begin(), g_dpy.contexts.end(), c));
    delete c;
}

void reap() {
    for (size_t i = g_dpy.surfaces.size(); i-- > 0;)
        if (g_dpy.surfaces[i]->dead && !g_dpy.surfaces[i]->bound) free_surface(g_dpy.surfaces[i]);
    for (size_t i = g_dpy.contexts.size(); i-- > 0;)
        if (g_dpy.contexts[i]->dead && !g_dpy.contexts[i]->current) free_context(g_dpy.contexts[i]);
    if (!g_dpy.initialized && g_dpy.surfaces.empty() && g_dpy.contexts.empty()) ovk::device_shutdown();
}

void unbind(Context* c) {
    c->gl.rec.submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, false);
    if (c->draw) c->draw->bound--;
    if (c->read) c->read->bound--;
    c->draw = c->read = nullptr;
    c->gl.draw = c->gl.read = nullptr;
    c->current = false;
}

void release_current() {
    if (!t_ctx) return;
    unbind(t_ctx);
    t_ctx = nullptr;
    gl::make_current(nullptr);
    reap();
}

bool config_matches(const EGLint* attr) {
    for (; attr && attr[0] != EGL_NONE; attr += 2) {
        const EGLint v = attr[1];
        if (v == EGL_DONT_CARE) continue;
        switch (attr[0]) {
        case EGL_RED_SIZE:
        case EGL_GREEN_SIZE:
        case EGL_BLUE_SIZE:
        case EGL_ALPHA_SIZE:
            if (v > 8) return false;
            break;
        case EGL_BUFFER_SIZE:
            if (v > 32) return false;
            break;
        case EGL_DEPTH_SIZE:
            if (v > depth_bits()) return false;
            break;
        case EGL_STENCIL_SIZE:
            if (v > 8) return false;
            break;
        case EGL_SAMPLES:
        case EGL_SAMPLE_BUFFERS:
        case EGL_LEVEL:
        case EGL_LUMINANCE_SIZE:
        case EGL_ALPHA_MASK_SIZE:
            if (v > 0) return false;
            break;
        case EGL_SURFACE_TYPE:
            if (v & ~kSurfaceTypes) return false;
            break;
        case EGL_RENDERABLE_TYPE:
        case EGL_CONFORMANT:
            if (v & ~kRenderable) return false;
            break;
        case EGL_CONFIG_ID:
            if (v != 1) return false;
            break;
        case EGL_COLOR_BUFFER_TYPE:
            if (v != EGL_RGB_BUFFER) return false;
            break;
        case EGL_TRANSPARENT_TYPE:
            if (v != EGL_NONE) return false;
            break;
        default:
            break;
        }
    }
    return true;
}
}

#define OEGL_FUNCS(X) \
    X(eglBindAPI) \
    X(eglChooseConfig) \
    X(eglCreateContext) \
    X(eglCreatePbufferSurface) \
    X(eglCreateWindowSurface) \
    X(eglDestroyContext) \
    X(eglDestroySurface) \
    X(eglGetConfigAttrib) \
    X(eglGetCurrentContext) \
    X(eglGetCurrentSurface) \
    X(eglGetDisplay) \
    X(eglGetError) \
    X(eglGetProcAddress) \
    X(eglInitialize) \
    X(eglMakeCurrent) \
    X(eglQuerySurface) \
    X(eglReleaseThread) \
    X(eglSwapBuffers) \
    X(eglSwapInterval) \
    X(eglTerminate)

OEGL_API EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id) {
    return display_id == EGL_DEFAULT_DISPLAY ? EGLDisplay(&g_dpy) : EGL_NO_DISPLAY;
}

OEGL_API EGLBoolean eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
    if (dpy != &g_dpy) return fail(EGL_BAD_DISPLAY, EGL_FALSE);
    std::lock_guard<std::mutex> g(g_dpy.lock);
    if (!ovk::device_init()) return fail(EGL_NOT_INITIALIZED, EGL_FALSE);
    g_dpy.initialized = true;
    if (major) *major = 1;
    if (minor) *minor = 4;
    return ok();
}

OEGL_API EGLBoolean eglTerminate(EGLDisplay dpy) {
    if (dpy != &g_dpy) return fail(EGL_BAD_DISPLAY, EGL_FALSE);
    std::lock_guard<std::mutex> g(g_dpy.lock);
    if (!g_dpy.initialized) return ok();
    g_dpy.initialized = false;
    for (Surface* s : g_dpy.surfaces) s->dead = true;
    for (Context* c : g_dpy.contexts) c->dead = true;
    reap();
    return ok();
}

OEGL_API EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs, EGLint config_size,
                                    EGLint* num_config) {
    if (EGLint e = check(dpy); e != EGL_SUCCESS) return fail(e, EGL_FALSE);
    if (!num_config) return fail(EGL_BAD_PARAMETER, EGL_FALSE);
    const bool match = config_matches(attrib_list);
    *num_config = match && (!configs || config_size > 0) ? 1 : 0;
    if (match && configs && config_size > 0) configs[0] = kConfig;
    return ok();
}

OEGL_API EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint* value) {
    if (EGLint e = check(dpy); e != EGL_SUCCESS) return fail(e, EGL_FALSE);
    if (config != kConfig) return fail(EGL_BAD_CONFIG, EGL_FALSE);
    if (!value) return fail(EGL_BAD_PARAMETER, EGL_FALSE);
    const EGLint dim = EGLint(ovk::gpu.props.limits.maxImageDimension2D);
    EGLint v;
    switch (attribute) {
    case EGL_BUFFER_SIZE: v = 32; break;
    case EGL_RED_SIZE:
    case EGL_GREEN_SIZE:
    case EGL_BLUE_SIZE:
    case EGL_ALPHA_SIZE:
    case EGL_STENCIL_SIZE: v = 8; break;
    case EGL_DEPTH_SIZE: v = depth_bits(); break;
    case EGL_LUMINANCE_SIZE:
    case EGL_ALPHA_MASK_SIZE:
    case EGL_SAMPLES:
    case EGL_SAMPLE_BUFFERS:
    case EGL_LEVEL:
    case EGL_MIN_SWAP_INTERVAL:
    case EGL_TRANSPARENT_RED_VALUE:
    case EGL_TRANSPARENT_GREEN_VALUE:
    case EGL_TRANSPARENT_BLUE_VALUE: v = 0; break;
    case EGL_MAX_SWAP_INTERVAL:
    case EGL_CONFIG_ID: v = 1; break;
    case EGL_CONFIG_CAVEAT:
    case EGL_TRANSPARENT_TYPE:
    case EGL_NATIVE_VISUAL_TYPE: v = EGL_NONE; break;
    case EGL_COLOR_BUFFER_TYPE: v = EGL_RGB_BUFFER; break;
    case EGL_SURFACE_TYPE: v = kSurfaceTypes; break;
    case EGL_RENDERABLE_TYPE:
    case EGL_CONFORMANT: v = kRenderable; break;
    case EGL_NATIVE_RENDERABLE:
    case EGL_BIND_TO_TEXTURE_RGB:
    case EGL_BIND_TO_TEXTURE_RGBA: v = EGL_FALSE; break;
    case EGL_MAX_PBUFFER_WIDTH:
    case EGL_MAX_PBUFFER_HEIGHT: v = dim; break;
    case EGL_MAX_PBUFFER_PIXELS: v = dim > 46340 ? INT_MAX : dim * dim; break;
#ifdef __ANDROID__
    case EGL_NATIVE_VISUAL_ID: v = WINDOW_FORMAT_RGBA_8888; break;
#else
    case EGL_NATIVE_VISUAL_ID: v = 0; break;
#endif
    default: return fail(EGL_BAD_ATTRIBUTE, EGL_FALSE);
    }
    *value = v;
    return ok();
}

OEGL_API EGLBoolean eglBindAPI(EGLenum api) {
    if (api != EGL_OPENGL_ES_API && api != EGL_OPENGL_API) return fail(EGL_BAD_PARAMETER, EGL_FALSE);
    t_api = api;
    return ok();
}

OEGL_API EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context,
                                     const EGLint* attrib_list) {
    std::lock_guard<std::mutex> g(g_dpy.lock);
    if (EGLint e = check(dpy); e != EGL_SUCCESS) return fail(e, EGL_NO_CONTEXT);
    if (config != kConfig) return fail(EGL_BAD_CONFIG, EGL_NO_CONTEXT);
    if (share_context != EGL_NO_CONTEXT && !context_of(share_context)) return fail(EGL_BAD_CONTEXT, EGL_NO_CONTEXT);
    Context* c = new Context;
    c->api = t_api;
    for (const EGLint* a = attrib_list; a && a[0] != EGL_NONE; a += 2)
        if (a[0] == EGL_CONTEXT_CLIENT_VERSION) c->version = a[1];
    if (!c->gl.rec.init()) {
        c->gl.rec.destroy();
        delete c;
        return fail(EGL_BAD_ALLOC, EGL_NO_CONTEXT);
    }
    g_dpy.contexts.push_back(c);
    t_error = EGL_SUCCESS;
    return c;
}

OEGL_API EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    std::lock_guard<std::mutex> g(g_dpy.lock);
    if (EGLint e = check(dpy); e != EGL_SUCCESS) return fail(e, EGL_FALSE);
    Context* c = context_of(ctx);
    if (!c) return fail(EGL_BAD_CONTEXT, EGL_FALSE);
    c->dead = true;
    reap();
    return ok();
}

OEGL_API EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win,
                                           const EGLint* attrib_list) {
    (void)attrib_list;
    std::lock_guard<std::mutex> g(g_dpy.lock);
    if (EGLint e = check(dpy); e != EGL_SUCCESS) return fail(e, EGL_NO_SURFACE);
    if (config != kConfig) return fail(EGL_BAD_CONFIG, EGL_NO_SURFACE);
#ifdef __ANDROID__
    if (!win) return fail(EGL_BAD_NATIVE_WINDOW, EGL_NO_SURFACE);
    for (Surface* s : g_dpy.surfaces)
        if (s->target.window == win && !s->dead) return fail(EGL_BAD_ALLOC, EGL_NO_SURFACE);
    Surface* s = new Surface;
    s->type = EGL_WINDOW_BIT;
    if (!ovk::swapchain_create(s->target, win)) {
        ovk::target_release(s->target);
        delete s;
        return fail(EGL_BAD_NATIVE_WINDOW, EGL_NO_SURFACE);
    }
    g_dpy.surfaces.push_back(s);
    t_error = EGL_SUCCESS;
    return s;
#else
    (void)win;
    return fail(EGL_BAD_NATIVE_WINDOW, EGL_NO_SURFACE);
#endif
}

OEGL_API EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint* attrib_list) {
    std::lock_guard<std::mutex> g(g_dpy.lock);
    if (EGLint e = check(dpy); e != EGL_SUCCESS) return fail(e, EGL_NO_SURFACE);
    if (config != kConfig) return fail(EGL_BAD_CONFIG, EGL_NO_SURFACE);
    EGLint w = 0, h = 0;
    for (const EGLint* a = attrib_list; a && a[0] != EGL_NONE; a += 2) {
        if (a[0] == EGL_WIDTH) w = a[1];
        else if (a[0] == EGL_HEIGHT) h = a[1];
    }
    const EGLint dim = EGLint(ovk::gpu.props.limits.maxImageDimension2D);
    if (w < 0 || h < 0) return fail(EGL_BAD_PARAMETER, EGL_NO_SURFACE);
    if (w > dim || h > dim) return fail(EGL_BAD_MATCH, EGL_NO_SURFACE);
    Surface* s = new Surface;
    s->type = EGL_PBUFFER_BIT;
    s->width = w;
    s->height = h;
    if (!ovk::target_create(s->target, uint32_t(w), uint32_t(h))) {
        delete s;
        return fail(EGL_BAD_ALLOC, EGL_NO_SURFACE);
    }
    g_dpy.surfaces.push_back(s);
    t_error = EGL_SUCCESS;
    return s;
}

OEGL_API EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
    std::lock_guard<std::mutex> g(g_dpy.lock);
    if (EGLint e = check(dpy); e != EGL_SUCCESS) return fail(e, EGL_FALSE);
    Surface* s = surface_of(surface);
    if (!s) return fail(EGL_BAD_SURFACE, EGL_FALSE);
    s->dead = true;
    reap();
    return ok();
}

OEGL_API EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    if (dpy != &g_dpy) return fail(EGL_BAD_DISPLAY, EGL_FALSE);
    std::lock_guard<std::mutex> g(g_dpy.lock);
    if (ctx == EGL_NO_CONTEXT) {
        if (draw != EGL_NO_SURFACE || read != EGL_NO_SURFACE) return fail(EGL_BAD_MATCH, EGL_FALSE);
        release_current();
        return ok();
    }
    if (!g_dpy.initialized) return fail(EGL_NOT_INITIALIZED, EGL_FALSE);
    Context* c = context_of(ctx);
    if (!c) return fail(EGL_BAD_CONTEXT, EGL_FALSE);
    Surface* d = surface_of(draw);
    Surface* r = surface_of(read);
    if (!d || !r) return fail(EGL_BAD_SURFACE, EGL_FALSE);
    if (c->current && c != t_ctx) return fail(EGL_BAD_ACCESS, EGL_FALSE);
    if (c == t_ctx && c->draw == d && c->read == r) return ok();
    if (t_ctx) unbind(t_ctx);
    c->draw = d;
    c->read = r;
    d->bound++;
    r->bound++;
    c->gl.draw = &d->target;
    c->gl.read = &r->target;
    c->current = true;
    if (!c->gl.initialized) {  // GL: viewport starts as the first drawable's size
        EGLint w, h;
        surface_size(d, w, h);
        c->gl.viewport[2] = w;
        c->gl.viewport[3] = h;
        c->gl.initialized = true;
    }
    t_ctx = c;
    gl::make_current(&c->gl);
    reap();
    return ok();
}

OEGL_API EGLBoolean eglReleaseThread(void) {
    if (t_ctx) {
        std::lock_guard<std::mutex> g(g_dpy.lock);
        release_current();
    }
    t_api = EGL_OPENGL_ES_API;
    return ok();
}

OEGL_API EGLContext eglGetCurrentContext(void) { return t_ctx ? EGLContext(t_ctx) : EGL_NO_CONTEXT; }

OEGL_API EGLSurface eglGetCurrentSurface(EGLint readdraw) {
    if (readdraw != EGL_DRAW && readdraw != EGL_READ) return fail(EGL_BAD_PARAMETER, EGL_NO_SURFACE);
    if (!t_ctx) return EGL_NO_SURFACE;
    return readdraw == EGL_DRAW ? EGLSurface(t_ctx->draw) : EGLSurface(t_ctx->read);
}

OEGL_API EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint* value) {
    std::lock_guard<std::mutex> g(g_dpy.lock);
    if (EGLint e = check(dpy); e != EGL_SUCCESS) return fail(e, EGL_FALSE);
    Surface* s = surface_of(surface);
    if (!s) return fail(EGL_BAD_SURFACE, EGL_FALSE);
    if (!value) return fail(EGL_BAD_PARAMETER, EGL_FALSE);
    EGLint w, h;
    surface_size(s, w, h);
    switch (attribute) {
    case EGL_WIDTH: *value = w; break;
    case EGL_HEIGHT: *value = h; break;
    case EGL_CONFIG_ID: *value = 1; break;
    case EGL_RENDER_BUFFER: *value = EGL_BACK_BUFFER; break;
    case EGL_SWAP_BEHAVIOR: *value = EGL_BUFFER_DESTROYED; break;
    case EGL_MULTISAMPLE_RESOLVE: *value = EGL_MULTISAMPLE_RESOLVE_DEFAULT; break;
    case EGL_LARGEST_PBUFFER:
    case EGL_MIPMAP_TEXTURE:
    case EGL_MIPMAP_LEVEL: *value = 0; break;
    case EGL_TEXTURE_FORMAT:
    case EGL_TEXTURE_TARGET: *value = EGL_NO_TEXTURE; break;
    case EGL_HORIZONTAL_RESOLUTION:
    case EGL_VERTICAL_RESOLUTION:
    case EGL_PIXEL_ASPECT_RATIO: *value = EGL_UNKNOWN; break;
    default: return fail(EGL_BAD_ATTRIBUTE, EGL_FALSE);
    }
    return ok();
}

OEGL_API EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (dpy != &g_dpy) return fail(EGL_BAD_DISPLAY, EGL_FALSE);
    Context* c = t_ctx;
    if (!c || surface == EGL_NO_SURFACE || EGLSurface(c->draw) != surface) return fail(EGL_BAD_SURFACE, EGL_FALSE);
    Surface* s = c->draw;
    if (s->type == EGL_PBUFFER_BIT) {
        c->gl.rec.submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, false);
        return ok();
    }
#ifdef __ANDROID__
    const VkResult r = ovk::swapchain_present(c->gl.rec, s->target);
    if (r == VK_SUCCESS) return ok();
    OLOG("present failed: %d", r);
    return fail(r == VK_ERROR_DEVICE_LOST ? EGL_CONTEXT_LOST : EGL_BAD_SURFACE, EGL_FALSE);
#else
    return fail(EGL_BAD_SURFACE, EGL_FALSE);
#endif
}

OEGL_API EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval) {
    if (EGLint e = check(dpy); e != EGL_SUCCESS) return fail(e, EGL_FALSE);
    if (!t_ctx || !t_ctx->draw) return fail(EGL_BAD_SURFACE, EGL_FALSE);
    ovk::Target& t = t_ctx->draw->target;
    const int v = interval <= 0 ? 0 : 1;
    if (t.interval != v) {
        t.interval = v;
        t.dirty = t.swapchain != VK_NULL_HANDLE;
    }
    return ok();
}

OEGL_API EGLint eglGetError(void) {
    const EGLint e = t_error;
    t_error = EGL_SUCCESS;
    return e;
}

OEGL_API __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* procname) {
    if (!procname) return nullptr;
    void* p = nullptr;
    if (procname[0] == 'g' && procname[1] == 'l') {
        p = gl::lookup(procname);
    } else {
        struct Entry {
            const char* name;
            void* fn;
        };
#define OEGL_ENTRY(n) {#n, reinterpret_cast<void*>(&n)},
        static const Entry kEgl[] = {OEGL_FUNCS(OEGL_ENTRY)};
#undef OEGL_ENTRY
        for (const Entry& e : kEgl)
            if (!std::strcmp(e.name, procname)) p = e.fn;
    }
    return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(p);
}
