// Host smoke test: drives liboryon.so exactly like the launcher (dlopen + dlsym, then
// eglGetProcAddress for GL), on whatever Vulkan driver the loader picks (Mesa lavapipe).
#include <EGL/egl.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "gl/gl_enums.hpp"

namespace {
int g_checks = 0, g_failed = 0;
void expect(bool c, const char* what) {
    g_checks++;
    if (!c) {
        g_failed++;
        std::printf("[smoke] FAIL %s\n", what);
    }
}

#define EGL_FN(n) decltype(&::n) n = nullptr;
struct Egl {
    EGL_FN(eglBindAPI) EGL_FN(eglChooseConfig) EGL_FN(eglCreateContext) EGL_FN(eglCreatePbufferSurface)
    EGL_FN(eglCreateWindowSurface) EGL_FN(eglDestroyContext) EGL_FN(eglDestroySurface) EGL_FN(eglGetConfigAttrib)
    EGL_FN(eglGetCurrentContext) EGL_FN(eglGetCurrentSurface) EGL_FN(eglGetDisplay) EGL_FN(eglGetError)
    EGL_FN(eglGetProcAddress) EGL_FN(eglInitialize) EGL_FN(eglMakeCurrent) EGL_FN(eglQuerySurface)
    EGL_FN(eglReleaseThread) EGL_FN(eglSwapBuffers) EGL_FN(eglSwapInterval) EGL_FN(eglTerminate)
};
#undef EGL_FN

struct Gl {
    GLenum (*GetError)();
    const GLubyte* (*GetString)(GLenum);
    const GLubyte* (*GetStringi)(GLenum, GLuint);
    void (*GetIntegerv)(GLenum, GLint*);
    void (*GetFloatv)(GLenum, GLfloat*);
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*ClearDepth)(GLdouble);
    void (*Clear)(GLbitfield);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*Finish)();
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
};

bool all_pixels(const std::vector<unsigned char>& px, const unsigned char want[4]) {
    for (size_t i = 0; i < px.size(); i += 4)
        if (std::memcmp(&px[i], want, 4)) return false;
    return !px.empty();
}
}

int main(int argc, char** argv) {
    void* lib = dlopen(argc > 1 ? argv[1] : "liboryon.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::printf("[smoke] FAIL dlopen: %s\n", dlerror());
        return 1;
    }
    Egl e;
    int resolved = 0;
#define ELOAD_GL(n) resolved += (e.n = reinterpret_cast<decltype(e.n)>(dlsym(lib, #n))) != nullptr;
    ELOAD_GL(eglBindAPI) ELOAD_GL(eglChooseConfig) ELOAD_GL(eglCreateContext) ELOAD_GL(eglCreatePbufferSurface)
    ELOAD_GL(eglCreateWindowSurface) ELOAD_GL(eglDestroyContext) ELOAD_GL(eglDestroySurface) ELOAD_GL(eglGetConfigAttrib)
    ELOAD_GL(eglGetCurrentContext) ELOAD_GL(eglGetCurrentSurface) ELOAD_GL(eglGetDisplay) ELOAD_GL(eglGetError)
    ELOAD_GL(eglGetProcAddress) ELOAD_GL(eglInitialize) ELOAD_GL(eglMakeCurrent) ELOAD_GL(eglQuerySurface)
    ELOAD_GL(eglReleaseThread) ELOAD_GL(eglSwapBuffers) ELOAD_GL(eglSwapInterval) ELOAD_GL(eglTerminate)
#undef ELOAD_GL
    expect(resolved == 20, "dlsym 20 launcher EGL functions");
    if (resolved != 20) return 1;
    expect(dlsym(lib, "glClear") == nullptr, "GL symbols are not exported");

    // Launcher order (README): display -> initialize -> config x2 -> visual id -> bindAPI -> context.
    EGLDisplay dpy = e.eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    expect(dpy != EGL_NO_DISPLAY && e.eglInitialize(dpy, &major, &minor) && major == 1, "eglInitialize");
    const EGLint attrs[] = {EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
                            EGL_SURFACE_TYPE, 5, EGL_RENDERABLE_TYPE, 4, EGL_NONE};
    EGLint n = 0;
    EGLConfig cfg = nullptr;
    expect(e.eglChooseConfig(dpy, attrs, nullptr, 0, &n) && n == 1, "eglChooseConfig count");
    expect(e.eglChooseConfig(dpy, attrs, &cfg, 1, &n) && n == 1 && cfg, "eglChooseConfig get");
    const EGLint too_deep[] = {EGL_DEPTH_SIZE, 64, EGL_NONE};
    expect(e.eglChooseConfig(dpy, too_deep, &cfg, 1, &n) && n == 0, "eglChooseConfig rejects depth 64");
    e.eglChooseConfig(dpy, attrs, &cfg, 1, &n);
    EGLint vid = -1, depth = 0;
    expect(e.eglGetConfigAttrib(dpy, cfg, EGL_NATIVE_VISUAL_ID, &vid), "EGL_NATIVE_VISUAL_ID");
    expect(e.eglGetConfigAttrib(dpy, cfg, EGL_DEPTH_SIZE, &depth) && depth >= 24, "EGL_DEPTH_SIZE");
    expect(e.eglBindAPI(EGL_OPENGL_API) && e.eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI GL + ES");
    expect(!e.eglBindAPI(0x1234) && e.eglGetError() == EGL_BAD_PARAMETER, "eglBindAPI rejects bad api");
    const EGLint cattr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = e.eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cattr);
    expect(ctx != EGL_NO_CONTEXT, "eglCreateContext");
    const EGLint p1[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    const EGLint p64[] = {EGL_WIDTH, 64, EGL_HEIGHT, 48, EGL_NONE};
    EGLSurface s1 = e.eglCreatePbufferSurface(dpy, cfg, p1);
    EGLSurface s64 = e.eglCreatePbufferSurface(dpy, cfg, p64);
    expect(s1 != EGL_NO_SURFACE && s64 != EGL_NO_SURFACE, "eglCreatePbufferSurface 1x1 + 64x48");
    expect(e.eglCreateWindowSurface(dpy, cfg, 0, nullptr) == EGL_NO_SURFACE &&
               e.eglGetError() == EGL_BAD_NATIVE_WINDOW,
           "window surface rejected on host");
    EGLint qw = 0, qh = 0;
    expect(e.eglQuerySurface(dpy, s64, EGL_WIDTH, &qw) && e.eglQuerySurface(dpy, s64, EGL_HEIGHT, &qh) && qw == 64 &&
               qh == 48,
           "eglQuerySurface size");
    expect(e.eglMakeCurrent(dpy, s64, s64, ctx), "eglMakeCurrent");
    expect(e.eglGetCurrentContext() == ctx && e.eglGetCurrentSurface(EGL_DRAW) == s64, "current context/surface");

    Gl g{};
#define LOAD_GL(f) g.f = reinterpret_cast<decltype(g.f)>(e.eglGetProcAddress("gl" #f)); expect(g.f, "gl" #f);
    LOAD_GL(GetError) LOAD_GL(GetString) LOAD_GL(GetStringi) LOAD_GL(GetIntegerv) LOAD_GL(GetFloatv) LOAD_GL(ClearColor)
    LOAD_GL(ClearDepth) LOAD_GL(Clear) LOAD_GL(Viewport) LOAD_GL(Finish) LOAD_GL(ReadPixels)
#undef LOAD_GL
    expect(e.eglGetProcAddress("glNotAFunction") == nullptr, "unknown GL name -> NULL");
    expect(e.eglGetProcAddress("eglSwapBuffers") == reinterpret_cast<void (*)()>(e.eglSwapBuffers),
           "eglGetProcAddress(egl*)");
    if (g_failed) return 1;

    // What LWJGL GL.createCapabilities queries first.
    expect(g.GetError() == GL_NO_ERROR, "no error at start");
    GLint maj = 0, mnr = -1, next = -1, vp[4] = {};
    g.GetIntegerv(GL_MAJOR_VERSION, &maj);
    g.GetIntegerv(GL_MINOR_VERSION, &mnr);
    g.GetIntegerv(GL_NUM_EXTENSIONS, &next);
    const char* ver = reinterpret_cast<const char*>(g.GetString(GL_VERSION));
    expect(maj == 3 && mnr == 0 && ver && !std::strncmp(ver, "3.0", 3), "GL version 3.0");
    expect(next >= 0 && g.GetString(GL_EXTENSIONS) != nullptr, "extension count/string");
    for (GLint i = 0; i < next; i++) expect(g.GetStringi(GL_EXTENSIONS, GLuint(i)) != nullptr, "glGetStringi");
    expect(g.GetStringi(GL_EXTENSIONS, GLuint(next)) == nullptr && g.GetError() == GL_INVALID_VALUE,
           "glGetStringi out of range");
    const char* rend = reinterpret_cast<const char*>(g.GetString(GL_RENDERER));
    std::printf("[smoke] GL_RENDERER=%s\n", rend ? rend : "(null)");
    g.GetIntegerv(GL_VIEWPORT, vp);
    expect(vp[0] == 0 && vp[1] == 0 && vp[2] == 64 && vp[3] == 48, "initial viewport = surface size");
    g.Viewport(1, 2, 30, 40);
    g.GetIntegerv(GL_VIEWPORT, vp);
    expect(vp[0] == 1 && vp[1] == 2 && vp[2] == 30 && vp[3] == 40, "glViewport roundtrip");
    g.GetIntegerv(0xFFFF, vp);
    expect(g.GetError() == GL_INVALID_ENUM && g.GetError() == GL_NO_ERROR, "invalid enum then cleared");
    GLint maxtex = 0;
    g.GetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
    expect(maxtex >= 4096, "GL_MAX_TEXTURE_SIZE >= 4096");

    // Clear + readback through the Vulkan default framebuffer.
    std::vector<unsigned char> px(64 * 48 * 4, 0x55);
    g.ClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    g.ClearDepth(1.0);
    g.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    g.ReadPixels(0, 0, 64, 48, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const unsigned char red[4] = {255, 0, 0, 255};
    expect(all_pixels(px, red), "clear red -> RGBA readback");
    GLfloat cc[4] = {};
    g.GetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    expect(cc[0] == 1.0f && cc[1] == 0.0f && cc[3] == 1.0f, "GL_COLOR_CLEAR_VALUE");
    g.ClearColor(0.0f, 1.0f, 0.0f, 0.0f);
    g.Clear(GL_COLOR_BUFFER_BIT);
    std::vector<unsigned char> sub(8 * 4 * 4, 0x55);
    g.ReadPixels(60, 44, 8, 4, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, sub.data());
    const unsigned char green_bgra[4] = {0, 255, 0, 0}, untouched[4] = {0x55, 0x55, 0x55, 0x55};
    bool clip_ok = true;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 8; x++)
            clip_ok &= !std::memcmp(&sub[(y * 8 + x) * 4], x < 4 ? green_bgra : untouched, 4);
    expect(clip_ok, "BGRA readback clipped at framebuffer edge");
    g.ReadPixels(0, 0, 1, 1, GL_RGB, GL_FLOAT, px.data());
    expect(g.GetError() == GL_INVALID_ENUM, "unsupported readback format -> GL_INVALID_ENUM");
    g.Clear(0x80000000u);
    expect(g.GetError() == GL_INVALID_VALUE, "glClear bad mask -> GL_INVALID_VALUE");
    expect(e.eglSwapInterval(dpy, 0) && e.eglSwapBuffers(dpy, s64), "eglSwapBuffers pbuffer");
    expect(!e.eglSwapBuffers(dpy, s1) && e.eglGetError() == EGL_BAD_SURFACE, "swap of non-current surface fails");

    // Rebind to the launcher's 1x1 pbuffer; deferred destroy of the bound 64x48 surface.
    expect(e.eglDestroySurface(dpy, s64), "destroy bound surface (deferred)");
    expect(e.eglMakeCurrent(dpy, s1, s1, ctx), "eglMakeCurrent 1x1");
    unsigned char one[4] = {};
    g.ClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    g.Clear(GL_COLOR_BUFFER_BIT);
    g.Finish();
    g.ReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, one);
    expect(one[0] == 0 && one[1] == 0 && one[2] == 255 && one[3] == 255, "1x1 clear blue");
    expect(e.eglMakeCurrent(dpy, s64, s64, ctx) == EGL_FALSE && e.eglGetError() == EGL_BAD_SURFACE,
           "destroyed surface handle rejected");

    expect(e.eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), "release current");
    expect(e.eglGetCurrentContext() == EGL_NO_CONTEXT, "no current context");
    expect(e.eglDestroySurface(dpy, s1) && e.eglDestroyContext(dpy, ctx), "destroy surface + context");
    expect(e.eglTerminate(dpy) && e.eglReleaseThread(), "eglTerminate + eglReleaseThread");
    expect(e.eglGetError() == EGL_SUCCESS, "EGL error clean");

    std::printf("[smoke] checks=%d failed=%d\n", g_checks, g_failed);
    dlclose(lib);
    return g_failed ? 1 : 0;
}
