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


# 4. draw path (64x64 pbuffer): MC-style interleaved arrays, VBO, textures, fixed function
import struct
def gl(name, res, *args):
    globals()[name] = fn(name, res, *args)
for n, a in {"glMatrixMode": [U], "glLoadIdentity": [], "glPushMatrix": [], "glPopMatrix": [], "glTranslatef": [F, F, F],
             "glRotatef": [F, F, F, F], "glScalef": [F, F, F], "glEnableClientState": [U], "glDisableClientState": [U],
             "glClientActiveTexture": [U], "glActiveTexture": [U], "glDrawArrays": [U, I, I], "glBindTexture": [U, U],
             "glBindBuffer": [U, U], "glDepthFunc": [U], "glBlendFunc": [U, U], "glAlphaFunc": [U, F], "glBegin": [U],
             "glEnd": [], "glVertex3f": [F, F, F], "glColor4f": [F, F, F, F], "glNormal3f": [F, F, F],
             "glFogi": [U, I], "glFogf": [U, F], "glColorMaterial": [U, U], "glTexEnvi": [U, U, I],
             "glTexGeni": [U, U, I], "glTexParameteri": [U, U, I]}.items():
    gl(n, None, *a)
gl("glOrtho", None, C.c_double, C.c_double, C.c_double, C.c_double, C.c_double, C.c_double)
for n in ("glVertexPointer", "glColorPointer", "glTexCoordPointer"): gl(n, None, I, U, I, P)
gl("glGenTextures", None, I, C.POINTER(U)); gl("glGenBuffers", None, I, C.POINTER(U))
gl("glBufferData", None, U, C.c_ssize_t, P, U)
gl("glTexImage2D", None, U, I, I, I, I, I, U, U, P); gl("glTexSubImage2D", None, U, I, I, I, I, I, U, U, P)
gl("glCopyTexSubImage2D", None, U, I, I, I, I, I, I, I); gl("glGetTexLevelParameteriv", None, U, I, U, C.POINTER(I))
gl("glGetFloatv", None, U, C.POINTER(F))
for n in ("glFogfv", "glLightModelfv"): gl(n, None, U, C.POINTER(F))
for n in ("glLightfv", "glTexEnvfv", "glTexGenfv"): gl(n, None, U, U, C.POINTER(F))
def fv(*v): return (F * len(v))(*v)
def px(x, y): return tuple(read(x, y, 1, 1))
def near(a, b, tol=3): return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))
RED, GREEN, BLUE, WHITE, BLACK = (255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255,) * 4, (0, 0, 0, 255)

def quad(x0, y0, x1, y1, z=0.0, rgba=(255, 255, 255, 255), uv=((0, 0), (1, 0), (1, 1), (0, 1)), lm=(0, 0)):
    # MC block layout: pos 3f | color 4ub | tex 2f | lightmap 2s  (stride 28), counter-clockwise
    return b"".join(struct.pack("<3f4B2f2h", x, y, z, *rgba, *t, *lm)
                    for (x, y), t in zip(((x0, y0), (x1, y0), (x1, y1), (x0, y1)), uv))
def arrays(addr, color=True, tex=True):
    glVertexPointer(3, G["GL_FLOAT"], 28, addr); glEnableClientState(G["GL_VERTEX_ARRAY"])
    for on, cap, setp in ((color, "GL_COLOR_ARRAY", lambda: glColorPointer(4, G["GL_UNSIGNED_BYTE"], 28, addr + 12)),
                          (tex, "GL_TEXTURE_COORD_ARRAY", lambda: glTexCoordPointer(2, G["GL_FLOAT"], 28, addr + 16))):
        if on: setp(); glEnableClientState(G[cap])
        else: glDisableClientState(G[cap])
def draw_client(data, **kw):
    buf = C.create_string_buffer(data, len(data))
    arrays(C.addressof(buf), **kw); glDrawArrays(G["GL_QUADS"], 0, len(data) // 28)
def texture(texels):  # texels: rows bottom-up of BGRA byte tuples (GL_BGRA + 8_8_8_8_REV, as MC uploads)
    t = U(); glGenTextures(1, C.byref(t)); glBindTexture(G["GL_TEXTURE_2D"], t)
    for pn in ("GL_TEXTURE_MIN_FILTER", "GL_TEXTURE_MAG_FILTER"): glTexParameteri(G["GL_TEXTURE_2D"], G[pn], G["GL_NEAREST"])
    h, w = len(texels), len(texels[0]); data = bytes(b for row in texels for t in row for b in t)
    glTexImage2D(G["GL_TEXTURE_2D"], 0, G["GL_RGBA"], w, h, 0, G["GL_BGRA"], G["GL_UNSIGNED_INT_8_8_8_8_REV"], None)
    glTexSubImage2D(G["GL_TEXTURE_2D"], 0, 0, 0, w, h, G["GL_BGRA"], G["GL_UNSIGNED_INT_8_8_8_8_REV"], data)
    return t
BGRA = {"r": (0, 0, 255, 255), "g": (0, 255, 0, 255), "b": (255, 0, 0, 255), "w": (255, 255, 255, 255)}
QUADRANTS = lambda: (px(12, 12), px(20, 12), px(12, 20), px(20, 20))

pb64 = eglCreatePbufferSurface(dpy, cfg, attribs(E["EGL_WIDTH"], 64, E["EGL_HEIGHT"], 64))
check("draw surface 64x64", pb64 and eglMakeCurrent(dpy, pb64, pb64, ctx2))
glViewport(0, 0, 64, 64)
glMatrixMode(G["GL_PROJECTION"]); glLoadIdentity(); glOrtho(0, 64, 0, 64, -1, 1)
glMatrixMode(G["GL_MODELVIEW"]); glLoadIdentity()
def clear(r=0.0, g=0.0, b=0.0): glClearColor(r, g, b, 1); glClear(G["GL_COLOR_BUFFER_BIT"] | G["GL_DEPTH_BUFFER_BIT"])

clear(); draw_client(quad(8, 8, 24, 24, rgba=RED), tex=False)
check("draw: client arrays GL_QUADS", px(16, 16) == RED and px(30, 30) == BLACK)

clear(); glEnable(G["GL_CULL_FACE"])
tri = lambda pts, col: b"".join(struct.pack("<3f4B2f2h", x, y, 0, *col, 0, 0, 0, 0) for x, y in pts)
for pts in (((8, 8), (24, 8), (16, 24)), ((40, 8), (48, 24), (56, 8))):
    data = tri(pts, GREEN); buf = C.create_string_buffer(data, len(data))
    arrays(C.addressof(buf), tex=False); glDrawArrays(G["GL_TRIANGLES"], 0, 3)
check("draw: back-face culling (CCW front)", px(16, 12) == GREEN and px(48, 12) == BLACK)
glDisable(G["GL_CULL_FACE"])

clear(); glEnable(G["GL_DEPTH_TEST"]); glDepthFunc(G["GL_LESS"])
draw_client(quad(8, 8, 24, 24, 0.5, BLUE), tex=False); draw_client(quad(8, 8, 24, 24, -0.5, RED), tex=False)
check("draw: depth test GL_LESS", px(16, 16) == BLUE)
glDisable(G["GL_DEPTH_TEST"])

tex = texture([[BGRA["r"], BGRA["g"]], [BGRA["b"], BGRA["w"]]])
clear(); glEnable(G["GL_TEXTURE_2D"]); draw_client(quad(8, 8, 24, 24))
check("draw: BGRA texture NEAREST + MODULATE", QUADRANTS() == (RED, GREEN, BLUE, WHITE), str(QUADRANTS()))
clear(); draw_client(quad(8, 8, 24, 24, rgba=(255, 0, 255, 255)))
check("draw: texture x vertex color", QUADRANTS() == (RED, BLACK, BLUE, (255, 0, 255, 255)))
glDisable(G["GL_TEXTURE_2D"])

vbo = U(); glGenBuffers(1, C.byref(vbo)); data = quad(8, 8, 24, 24, rgba=(255, 0, 255, 255))
glBindBuffer(G["GL_ARRAY_BUFFER"], vbo); glBufferData(G["GL_ARRAY_BUFFER"], len(data), data, G["GL_STATIC_DRAW"])
clear(); arrays(0, tex=False); glBindBuffer(G["GL_ARRAY_BUFFER"], 0); glDrawArrays(G["GL_QUADS"], 0, 4)
check("draw: VBO (glBufferData + offsets)", px(16, 16) == (255, 0, 255, 255))

clear(); glEnable(G["GL_BLEND"]); glBlendFunc(G["GL_SRC_ALPHA"], G["GL_ONE_MINUS_SRC_ALPHA"])
draw_client(quad(8, 8, 24, 24, rgba=(255, 255, 255, 128)), tex=False)
check("draw: blend SRC_ALPHA/ONE_MINUS_SRC_ALPHA", near(px(16, 16), (128, 128, 128, 191), 2), str(px(16, 16)))  # GL blends alpha too
glDisable(G["GL_BLEND"])

clear(); glEnable(G["GL_ALPHA_TEST"]); glAlphaFunc(G["GL_GREATER"], 0.5)
draw_client(quad(8, 8, 24, 24, rgba=(255, 255, 255, 102)), tex=False)
draw_client(quad(40, 8, 56, 24, rgba=(255, 255, 255, 200)), tex=False)
check("draw: alpha test GREATER 0.5", px(16, 16) == BLACK and px(48, 16)[:3] == WHITE[:3], str((px(16, 16), px(48, 16))))
glDisable(G["GL_ALPHA_TEST"])

m = (F * 16)(); clear(); glPushMatrix(); glTranslatef(32, 0, 0); glGetFloatv(G["GL_MODELVIEW_MATRIX"], m); moved = m[12]
draw_client(quad(8, 8, 24, 24, rgba=RED), tex=False); glPopMatrix(); glGetFloatv(G["GL_MODELVIEW_MATRIX"], m)
check("matrix: translate/push/pop + glGetFloatv", moved == 32 and m[12] == 0 and px(48, 16) == RED and px(16, 16) == BLACK)
glPushMatrix(); glRotatef(90, 0, 0, 1); glGetFloatv(G["GL_MODELVIEW_MATRIX"], m); glPopMatrix()
check("matrix: glRotatef", near((m[0], m[1], m[4], m[5]), (0, 1, -1, 0), 1e-6))

clear(); glColor4f(1, 1, 0, 1); glBegin(G["GL_TRIANGLE_STRIP"])
for x, y in ((8, 40), (24, 40), (8, 56), (24, 56)): glVertex3f(x, y, 0)
glEnd(); check("immediate: glBegin(GL_TRIANGLE_STRIP)", px(16, 48) == (255, 255, 0, 255))

clear(); glEnable(G["GL_FOG"]); glFogi(G["GL_FOG_MODE"], G["GL_LINEAR"]); glFogf(G["GL_FOG_START"], 0); glFogf(G["GL_FOG_END"], 1)
glFogfv(G["GL_FOG_COLOR"], fv(0, 1, 0, 1)); draw_client(quad(8, 8, 24, 24, -0.5, RED), tex=False)
check("fixed: linear fog", near(px(16, 16), (128, 128, 0, 255), 2), str(px(16, 16)))
glDisable(G["GL_FOG"])

clear(); glColor4f(1, 1, 1, 1)
for cap in ("GL_LIGHTING", "GL_LIGHT0", "GL_COLOR_MATERIAL"): glEnable(G[cap])
glColorMaterial(G["GL_FRONT_AND_BACK"], G["GL_AMBIENT_AND_DIFFUSE"])
glLightfv(G["GL_LIGHT0"], G["GL_POSITION"], fv(0, 0, 1, 0)); glLightfv(G["GL_LIGHT0"], G["GL_DIFFUSE"], fv(0.6, 0.6, 0.6, 1))
glLightfv(G["GL_LIGHT0"], G["GL_AMBIENT"], fv(0, 0, 0, 1)); glLightModelfv(G["GL_LIGHT_MODEL_AMBIENT"], fv(0.4, 0.4, 0.4, 1))
glNormal3f(0, 0, 1); draw_client(quad(8, 8, 24, 24), tex=False)
glNormal3f(1, 0, 0); draw_client(quad(40, 8, 56, 24), tex=False)
check("fixed: lighting (light0 + color material)", px(16, 16) == WHITE and near(px(48, 16), (102, 102, 102, 255), 1), str(px(48, 16)))
for cap in ("GL_LIGHTING", "GL_LIGHT0", "GL_COLOR_MATERIAL"): glDisable(G[cap])

clear(); glBindTexture(G["GL_TEXTURE_2D"], tex); glEnable(G["GL_TEXTURE_2D"]); TE = G["GL_TEXTURE_ENV"]
glTexEnvi(TE, G["GL_TEXTURE_ENV_MODE"], G["GL_COMBINE"]); glTexEnvi(TE, G["GL_COMBINE_RGB"], G["GL_REPLACE"])
glTexEnvi(TE, G["GL_SRC0_RGB"], G["GL_CONSTANT"]); glTexEnvfv(TE, G["GL_TEXTURE_ENV_COLOR"], fv(0, 0, 1, 1))
draw_client(quad(8, 8, 24, 24))
check("fixed: GL_COMBINE REPLACE(CONSTANT)", QUADRANTS() == (BLUE,) * 4)
glTexEnvi(TE, G["GL_TEXTURE_ENV_MODE"], G["GL_MODULATE"]); glDisable(G["GL_TEXTURE_2D"])

clear(); glActiveTexture(G["GL_TEXTURE0"] + 1); glBindTexture(G["GL_TEXTURE_2D"], tex); glEnable(G["GL_TEXTURE_2D"])
glMatrixMode(G["GL_TEXTURE"]); glLoadIdentity(); glScalef(1 / 16, 1 / 16, 1 / 16); glMatrixMode(G["GL_MODELVIEW"])
glActiveTexture(G["GL_TEXTURE0"])
for lm, x in (((4, 4), 8), ((12, 12), 40)):
    data = quad(x, 8, x + 16, 24, lm=lm); buf = C.create_string_buffer(data, len(data)); a = C.addressof(buf)
    arrays(a, tex=False); glClientActiveTexture(G["GL_TEXTURE0"] + 1)
    glTexCoordPointer(2, G["GL_SHORT"], 28, a + 24); glEnableClientState(G["GL_TEXTURE_COORD_ARRAY"])
    glClientActiveTexture(G["GL_TEXTURE0"]); glDrawArrays(G["GL_QUADS"], 0, 4)
check("fixed: lightmap unit 1 (GL_SHORT coords x texture matrix)", px(16, 16) == RED and px(48, 16) == WHITE)
glClientActiveTexture(G["GL_TEXTURE0"] + 1); glDisableClientState(G["GL_TEXTURE_COORD_ARRAY"]); glClientActiveTexture(G["GL_TEXTURE0"])
glActiveTexture(G["GL_TEXTURE0"] + 1); glDisable(G["GL_TEXTURE_2D"]); glActiveTexture(G["GL_TEXTURE0"])

clear(0, 1, 1); copy = texture([[BGRA["r"]] * 2] * 2)
glCopyTexSubImage2D(G["GL_TEXTURE_2D"], 0, 0, 0, 0, 0, 2, 2)
clear(); glEnable(G["GL_TEXTURE_2D"]); draw_client(quad(8, 8, 24, 24))
check("texture: glCopyTexSubImage2D from surface", QUADRANTS() == ((0, 255, 255, 255),) * 4)
glDisable(G["GL_TEXTURE_2D"])

wv = I(); P2D = G["GL_PROXY_TEXTURE_2D"]
glTexImage2D(P2D, 0, G["GL_RGBA"], 1024, 1024, 0, G["GL_BGRA"], G["GL_UNSIGNED_INT_8_8_8_8_REV"], None)
glGetTexLevelParameteriv(P2D, 0, G["GL_TEXTURE_WIDTH"], C.byref(wv)); fits = wv.value
glTexImage2D(P2D, 0, G["GL_RGBA"], 1 << 20, 1 << 20, 0, G["GL_BGRA"], G["GL_UNSIGNED_INT_8_8_8_8_REV"], None)
glGetTexLevelParameteriv(P2D, 0, G["GL_TEXTURE_WIDTH"], C.byref(wv))
check("texture: proxy + glGetTexLevelParameteriv", fits == 1024 and wv.value == 0)

clear(); glBindTexture(G["GL_TEXTURE_2D"], tex); glEnable(G["GL_TEXTURE_2D"])
for coord, plane in (("GL_S", (1 / 16, 0, 0, -0.5)), ("GL_T", (0, 1 / 16, 0, -0.5))):
    glTexGeni(G[coord], G["GL_TEXTURE_GEN_MODE"], G["GL_EYE_LINEAR"]); glTexGenfv(G[coord], G["GL_EYE_PLANE"], fv(*plane))
    glEnable(G["GL_TEXTURE_GEN_" + coord[3]])
draw_client(quad(8, 8, 24, 24), tex=False)
check("fixed: texgen EYE_LINEAR", QUADRANTS() == (RED, GREEN, BLUE, WHITE), str(QUADRANTS()))
for k in "ST": glDisable(G["GL_TEXTURE_GEN_" + k])
glDisable(G["GL_TEXTURE_2D"])
check("no GL errors in draw tests", glGetError() == 0)
check("back to pbuffer", eglMakeCurrent(dpy, pb, pb, ctx2) and eglDestroySurface(dpy, pb64))

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
