# Oryon Wrapper

Renderer Minecraft Java 1.12.2 untuk Android: OpenGL (LWJGL) → Vulkan 1.1 (header 131). Satu library `liboryon.so` berisi GL + EGL, dimuat launcher lewat `dlopen()` + `dlsym()`.

## Tools

```
pip install pyelftools capstone --break-system-packages
python3 -I tools/bootstrap.py <dir_upload> . index
```

`<dir_upload>` berisi file berakhiran `1.12.2.jar`, `lwjgl-glfw-classes.jar`, `libpojavexec.so`. Indeks ditulis ke `index/` (tidak di-commit); semua alat hanya mencetak ringkasan.

| Alat | Pemakaian | Fungsi |
|---|---|---|
| `bootstrap.py` | `<dir_upload> <root> <index>` | Semua indeks + validator |
| `jarindex.py` | `<lwjgl.jar> <1.12.2.jar> <index>` | Kelas LWJGL, call site MC, cross-reference |
| `reach.py` | `<lwjgl.jar> <index>` | Fungsi GL yang dijangkau MC + set `check_*` |
| `elfscan.py` | `<lib.so> <out.json>` | Ekspor, impor, string ELF |
| `vkindex.py` | `<dir_vulkan> <out.json>` | Indeks header Vulkan |
| `validate.py` | `<root> <index> [--quiet] [--so <lib>]` | Validasi `.cpp/.hpp` (+ ekspor `.so`); exit 1 jika error |
| `glconst.py` | `<index> <header> [GL_X ...]` | Tulis ulang `src/gl_enums.hpp` dari konstanta LWJGL |
| `hosttest.py` | `<lib.so> <index> [egl.h]` | Uji ala launcher (`dlopen`+`dlsym`, ctypes) di driver host |
| `jtrace.py` | `<jar> <kelas> [method ...] [--ints]` | Jejak bytecode ringkas |
| `a64trace.py` | `<lib.so> <fungsi ...>` | Jejak AArch64 ringkas |
| `a64dis.py` | `<lib.so> <fungsi ...>` | Disassembly AArch64 beranotasi |
| `axml.py` | `<apk> [entri] [--res]` | Dekode `AndroidManifest.xml` + `resources.arsc` dari APK |
| `jcls.py` | — | Modul parser class file |

Penanda yang dibaca `validate.py`: `OGL_API` (fungsi GL), `ORYON_PROC`/`ORYON_STUB` (tabel GetProcAddress, `src/proc_table.inc`), `ORYON_CAP_VERSION`/`ORYON_CAP_EXT` (klaim kapabilitas), `OEGL_API` (fungsi EGL), `constexpr T GL_X = v;` (konstanta). Simbol `vk*`/`Vk*`/`VK_*` wajib ada di header 131.

## Build & Uji

CI (GitHub Actions, 4 step): checkout → NDK r30 → `cmake` arm64-v8a (`android-29`: ELF TLS native) → unggah `build/liboryon.so`.

Sandbox (driver uji: Mesa 26.0.0 lavapipe, `-Dplatforms=wayland` agar WSI/headless aktif):

```
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ && cmake --build build/host
python3 -I tools/validate.py . index --so build/host/liboryon.so
VK_ICD_FILENAMES=<mesa>/share/vulkan/icd.d/lvp_icd.x86_64.json python3 -I tools/hosttest.py build/host/liboryon.so index
```

Di host, window surface = `VK_EXT_headless_surface` 640×480; di Android = `VK_KHR_android_surface`.

## Struktur Kode

| File | Isi |
|---|---|
| `src/oryon.hpp` | Makro `OGL_API`/`OEGL_API`, typedef GL, log |
| `src/gl_enums.hpp` | Konstanta GL (dibangkitkan `glconst.py`) |
| `src/gl_api.hpp` | Prototipe GL yang diekspor (dicek compiler + validator) |
| `src/vk.hpp/.cpp` | Loader Vulkan (`dlopen libvulkan.so`), daftar fungsi X-macro |
| `src/device.hpp/.cpp` | Instance/device 1.1, memori, cache render pass, barrier |
| `src/context.hpp/.cpp` | Konteks GL: rekaman command buffer (ring 2 frame), render pass lazy, clear tertunda → load op, present |
| `src/surface.cpp` | Default framebuffer (orientasi baris GL), swapchain, pbuffer |
| `src/egl.cpp` | 20 fungsi EGL launcher (prototipe Khronos `<EGL/egl.h>`) |
| `src/gl_core.cpp` | `glClear*`, `glViewport`, `glScissor`, `glEnable`, `glGet*`, `glReadPixels`, `glFlush/Finish` |
| `src/proc.hpp/.cpp`, `src/proc_table.inc` | Klaim versi (`ORYON_CAP_VERSION`) + tabel `eglGetProcAddress` terurut |

## Roadmap Sesi

1. **Fondasi** ✅ — loader Vulkan, EGL, swapchain/pbuffer, clear/viewport/get/readback, tabel proc, CI.
2. Jalur gambar inti — texture, VBO, client array, matrix stack, `glBegin/End`, uber-shader fixed-function, cache pipeline.
3. Display list, FBO (GL30 + alias ARB/EXT), query, tabel stub lengkap → `ORYON_CAP_VERSION(3, 0)`.
4. GLSL runtime (shader post-process MC) → SPIR-V.
5. Integrasi perangkat — recreate surface, pre-rotation, optimasi, uji MC lewat launcher.

## Konteks EGL

Oryon menyediakan EGL sendiri (bukan EGL sistem) dan memuat Vulkan dari driver bawaan perangkat: `dlopen("libvulkan.so")`; `VULKAN_PTR` diabaikan.

Alur launcher (`libpojavexec.so`):

1. `POJAV_RENDERER` berawalan `opengles` → jalur EGL. Library EGL = `dlopen(POJAVEXEC_EGL)`; jika gagal, diam-diam `libEGL.so` sistem.
2. 20 fungsi diambil lewat `eglGetProcAddress`, lalu `dlsym`: `eglBindAPI`, `eglChooseConfig`, `eglCreateContext`, `eglCreatePbufferSurface`, `eglCreateWindowSurface`, `eglDestroyContext`, `eglDestroySurface`, `eglGetConfigAttrib`, `eglGetCurrentContext`, `eglGetCurrentSurface`, `eglGetDisplay`, `eglGetError`, `eglGetProcAddress`, `eglInitialize`, `eglMakeCurrent`, `eglQuerySurface`, `eglReleaseThread`, `eglSwapBuffers`, `eglSwapInterval`, `eglTerminate`.
3. `eglGetDisplay(EGL_DEFAULT_DISPLAY)` → `eglInitialize` → `eglChooseConfig` (2×: hitung, lalu ambil 1) → `eglGetConfigAttrib(EGL_NATIVE_VISUAL_ID)` → `eglBindAPI` → `eglCreateContext` (share = konteks sebelumnya atau tidak ada).
4. `ANativeWindow_setBuffersGeometry(win, 0, 0, visualID)` → `eglCreateWindowSurface(win, NULL)`; tanpa window → `eglCreatePbufferSurface` → `eglMakeCurrent`.
5. Tiap frame `eglSwapBuffers`; error `EGL_BAD_SURFACE` → launcher menunggu window baru lalu membuat surface baru.

| Panggilan | Nilai dari launcher |
|---|---|
| `eglChooseConfig` | `EGL_BLUE_SIZE`=8, `EGL_GREEN_SIZE`=8, `EGL_RED_SIZE`=8, `EGL_ALPHA_SIZE`=8, `EGL_DEPTH_SIZE`=24, `EGL_SURFACE_TYPE`=5, `EGL_RENDERABLE_TYPE`=4 |
| `eglBindAPI` | `EGL_OPENGL_ES_API` (0x30A0), atau `EGL_OPENGL_API` (0x30A2) bila `POJAV_RENDERER` = `opengles3_desktopgl`; Oryon menerima keduanya |
| `eglCreateContext` | `EGL_CONTEXT_CLIENT_VERSION` = `LIBGL_ES` (dibaca tanpa cek NULL; di luar 0–32767 → 2) |
| `eglCreatePbufferSurface` | `EGL_WIDTH`=1, `EGL_HEIGHT`=1 |

Sisi LWJGL: `org.lwjgl.opengl.libname` (path absolut dimuat langsung); GetProcAddress dicari glX → egl → OSMesa → `dlsym`, jadi Oryon cukup menyediakan `eglGetProcAddress` untuk nama `egl*` dan `gl*`.
