import ctypes as C, json, os, re, sys, threading, time

# hosttest.py <liboryon.so> <index> [egl.h]: launcher-style test (dlopen + dlsym + eglGetProcAddress, no JNI).
SO, IDX = sys.argv[1], sys.argv[2]
EGLH = sys.argv[3] if len(sys.argv) > 3 else "/usr/include/EGL/egl.h"
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

E = {}
for line in open(EGLH):
    m = re.match(r"#define\s+(EGL_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\s*$", line)
    if m: E[m.group(1)] = int(m.group(2), 0)
G = {}
for c, e in json.load(open(f"{IDX}/lwjgl.json")).items():
    if c.startswith("org/lwjgl/opengl/"):
        for n, d, a, cv in e["f"]:
            if isinstance(cv, int) and n.startswith("GL_"): G.setdefault(n, cv)
EGL_REQ = sorted({s for _, s in json.load(open(f"{IDX}/pojavexec_elf.json"))["strings"] if re.fullmatch(r"egl[A-Z]\w+", s)})
PROCS = re.findall(r"^ORYON_(?:EGL|PROC|STUB)\((\w+)\)", open(os.path.join(ROOT, "src/proc_table.inc")).read(), re.M)

lib = C.CDLL(os.path.abspath(SO), mode=os.RTLD_NOW | os.RTLD_LOCAL)
gpa = lib.eglGetProcAddress; gpa.restype = C.c_void_p; gpa.argtypes = [C.c_char_p]
P, I, U, F = C.c_void_p, C.c_int32, C.c_uint32, C.c_float
def fn(name, res, *args):
    a = gpa(name.encode())
    return C.CFUNCTYPE(res, *args)(a) if a else None

results = []
def check(name, cond, info=""):
    results.append((name, bool(cond), info))
    return bool(cond)

# 1. symbol resolution: launcher asks eglGetProcAddress, then dlsym
for n in EGL_REQ:
    a, d = gpa(n.encode()), C.cast(getattr(lib, n, None), C.c_void_p).value if hasattr(lib, n) else None
    check(f"egl symbol {n}", a and a == d)
check("proc table entries", all(gpa(n.encode()) == C.cast(getattr(lib, n), C.c_void_p).value for n in PROCS), f"{len(PROCS)}")
check("unknown proc -> NULL", gpa(b"glDefinitelyNotAFunction") is None)

eglGetDisplay = fn("eglGetDisplay", P, P); eglInitialize = fn("eglInitialize", U, P, C.POINTER(I), C.POINTER(I))
eglChooseConfig = fn("eglChooseConfig", U, P, C.POINTER(I), C.POINTER(P), I, C.POINTER(I))
eglGetConfigAttrib = fn("eglGetConfigAttrib", U, P, P, I, C.POINTER(I)); eglBindAPI = fn("eglBindAPI", U, U)
eglCreateContext = fn("eglCreateContext", P, P, P, P, C.POINTER(I))
eglCreatePbufferSurface = fn("eglCreatePbufferSurface", P, P, P, C.POINTER(I))
eglCreateWindowSurface = fn("eglCreateWindowSurface", P, P, P, C.c_size_t, C.POINTER(I))
eglMakeCurrent = fn("eglMakeCurrent", U, P, P, P, P); eglQuerySurface = fn("eglQuerySurface", U, P, P, I, C.POINTER(I))
eglSwapBuffers = fn("eglSwapBuffers", U, P, P); eglSwapInterval = fn("eglSwapInterval", U, P, I)
eglGetError = fn("eglGetError", I); eglGetCurrentContext = fn("eglGetCurrentContext", P)
eglGetCurrentSurface = fn("eglGetCurrentSurface", P, I)
eglDestroySurface = fn("eglDestroySurface", U, P, P); eglDestroyContext = fn("eglDestroyContext", U, P, P)
eglReleaseThread = fn("eglReleaseThread", U); eglTerminate = fn("eglTerminate", U, P)
glClearColor = fn("glClearColor", None, F, F, F, F); glClear = fn("glClear", None, U)
glReadPixels = fn("glReadPixels", None, I, I, I, I, U, U, P); glGetString = fn("glGetString", C.c_char_p, U)
glGetError = fn("glGetError", U); glGetIntegerv = fn("glGetIntegerv", None, U, C.POINTER(I))
glEnable = fn("glEnable", None, U); glDisable = fn("glDisable", None, U); glIsEnabled = fn("glIsEnabled", C.c_ubyte, U)
glScissor = fn("glScissor", None, I, I, I, I); glViewport = fn("glViewport", None, I, I, I, I)
glFinish = fn("glFinish", None)

def attribs(*kv): return (I * (len(kv) + 1))(*kv, E["EGL_NONE"])
def read(x, y, w, h, fmt="GL_RGBA"):
    buf = (C.c_ubyte * (w * h * 4))()
    glReadPixels(x, y, w, h, G[fmt], G["GL_UNSIGNED_BYTE"], buf)
    return bytes(buf)
def px(b): return tuple(round(v * 255) for v in b)

# 2. launcher flow (README steps 3-5, launcher attribute values)
dpy = eglGetDisplay(None)
mj, mn = I(), I()
check("eglInitialize", eglInitialize(dpy, C.byref(mj), C.byref(mn)), f"EGL {mj.value}.{mn.value}")
cfg_attr = attribs(E["EGL_BLUE_SIZE"], 8, E["EGL_GREEN_SIZE"], 8, E["EGL_RED_SIZE"], 8, E["EGL_ALPHA_SIZE"], 8,
                   E["EGL_DEPTH_SIZE"], 24, E["EGL_SURFACE_TYPE"], 5, E["EGL_RENDERABLE_TYPE"], 4)
num = I(); cfg = P()
check("eglChooseConfig count", eglChooseConfig(dpy, cfg_attr, None, 0, C.byref(num)) and num.value >= 1)
check("eglChooseConfig pick", eglChooseConfig(dpy, cfg_attr, C.byref(cfg), 1, C.byref(num)) and cfg.value)
vid = I(); check("EGL_NATIVE_VISUAL_ID", eglGetConfigAttrib(dpy, cfg, E["EGL_NATIVE_VISUAL_ID"], C.byref(vid)))
check("eglBindAPI ES", eglBindAPI(E["EGL_OPENGL_ES_API"])); check("eglBindAPI GL", eglBindAPI(E["EGL_OPENGL_API"]))
ctx = eglCreateContext(dpy, cfg, None, attribs(E["EGL_CONTEXT_CLIENT_VERSION"], 2))
check("eglCreateContext", ctx)

pb = eglCreatePbufferSurface(dpy, cfg, attribs(E["EGL_WIDTH"], 1, E["EGL_HEIGHT"], 1))
check("eglCreatePbufferSurface 1x1", pb)
check("eglMakeCurrent pbuffer", eglMakeCurrent(dpy, pb, pb, ctx) and eglGetCurrentContext() == ctx)
renderer = (glGetString(G["GL_RENDERER"]) or b"").decode()
check("glGetString VENDOR/RENDERER/VERSION", glGetString(G["GL_VENDOR"]) and renderer and glGetString(G["GL_VERSION"]))
check("glGetString bad enum", glGetString(0xDEAD) is None and glGetError() == G["GL_INVALID_ENUM"] and glGetError() == 0)
v = (I * 4)(); glGetIntegerv(G["GL_MAJOR_VERSION"], v)
check("GL_MAJOR_VERSION hidden below 3.0", glGetError() == G["GL_INVALID_ENUM"])
glClearColor(1, 0, 0, 1); glClear(G["GL_COLOR_BUFFER_BIT"])
check("pbuffer clear + glReadPixels", read(0, 0, 1, 1) == bytes([255, 0, 0, 255]))
check("glReadPixels BGRA", read(0, 0, 1, 1, "GL_BGRA") == bytes([0, 0, 255, 255]))

# 3. headless window surface (swapchain path)
win = eglCreateWindowSurface(dpy, cfg, 1, None)
check("eglCreateWindowSurface", win)
W, H = I(), I()
eglQuerySurface(dpy, win, E["EGL_WIDTH"], C.byref(W)); eglQuerySurface(dpy, win, E["EGL_HEIGHT"], C.byref(H))
W, H = W.value, H.value
check("eglMakeCurrent window", eglMakeCurrent(dpy, win, win, ctx) and eglGetCurrentSurface(E["EGL_DRAW"]) == win, f"{W}x{H}")
ok = True
for i in range(8):
    col = (i / 7, 1 - i / 7, 0.5, 1)
    glClearColor(*col); glClear(G["GL_COLOR_BUFFER_BIT"] | G["GL_DEPTH_BUFFER_BIT"])
    ok &= bool(eglSwapBuffers(dpy, win))
    b = read(0, 0, W, H)
    ok &= b == bytes(px(col)) * (W * H)
check("8 frames clear+swap+readback", ok)

base = read(0, 0, 1, 1)
glEnable(G["GL_SCISSOR_TEST"]); glScissor(10, 20, 30, 40)
glClearColor(0, 1, 0, 1); glClear(G["GL_COLOR_BUFFER_BIT"])
green = bytes([0, 255, 0, 255])
inside = read(10, 20, 1, 1) == green and read(39, 59, 1, 1) == green
outside = read(9, 20, 1, 1) == base and read(10, 19, 1, 1) == base and read(40, 60, 1, 1) == base
check("scissored clear (GL row order)", inside and outside and glIsEnabled(G["GL_SCISSOR_TEST"]) == 1)
glDisable(G["GL_SCISSOR_TEST"])
glGetIntegerv(G["GL_SCISSOR_BOX"], v); check("GL_SCISSOR_BOX", list(v) == [10, 20, 30, 40])
glGetIntegerv(G["GL_VIEWPORT"], v); check("GL_VIEWPORT initial", list(v) == [0, 0, 1, 1], str(list(v)))

err = []
def other_thread(): err.append((eglMakeCurrent(dpy, pb, pb, ctx), eglGetError()))
t = threading.Thread(target=other_thread); t.start(); t.join()
check("context busy on other thread -> EGL_BAD_ACCESS", err == [(0, E["EGL_BAD_ACCESS"])])

check("eglSwapInterval 0", eglSwapInterval(dpy, 0))
frames, t0 = 0, time.perf_counter()
while time.perf_counter() - t0 < 2.0:
    glClearColor(frames % 2, 0, 0, 1); glClear(G["GL_COLOR_BUFFER_BIT"])
    if not eglSwapBuffers(dpy, win): break
    frames += 1
fps = frames / (time.perf_counter() - t0)
check("swap loop", frames > 10, f"{fps:.0f} fps")
glFinish()

ctx2 = eglCreateContext(dpy, cfg, ctx, attribs(E["EGL_CONTEXT_CLIENT_VERSION"], 2))
check("shared context on pbuffer", ctx2 and eglMakeCurrent(dpy, pb, pb, ctx2))
glClearColor(0, 0, 1, 1); glClear(G["GL_COLOR_BUFFER_BIT"])
check("second context clear", read(0, 0, 1, 1) == bytes([0, 0, 255, 255]))

check("eglDestroySurface while bound (deferred)", eglDestroySurface(dpy, pb))
check("release", eglMakeCurrent(dpy, None, None, None) and eglGetCurrentContext() is None)
check("destroy", eglDestroySurface(dpy, win) and eglDestroyContext(dpy, ctx) and eglDestroyContext(dpy, ctx2))
check("eglReleaseThread/eglTerminate", eglReleaseThread() and eglTerminate(dpy))

bad = [r for r in results if not r[1]]
print(f"[hosttest] renderer='{renderer}' surface={W}x{H}")
print(f"[hosttest] checks={len(results)} pass={len(results) - len(bad)} fail={len(bad)} -> {'PASS' if not bad else 'FAIL'}")
for n, okk, info in results:
    if not okk or info: print(f"   {'ok ' if okk else 'FAIL'} {n} {info}")
sys.exit(1 if bad else 0)
