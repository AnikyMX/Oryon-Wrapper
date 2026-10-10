# Oryon Wrapper

Renderer Minecraft Java 1.12.2 untuk Android: OpenGL (LWJGL) → Vulkan 1.1 (header 131). Satu library `liboryon.so` berisi GL + EGL, dimuat launcher lewat `dlopen()` + `dlsym()`.

## Tools

```
pip install pyelftools capstone --break-system-packages
python3 -I tools/bootstrap.py third_party . index
python3 -I tools/gen_gl.py . index
python3 -I tools/validate.py . index
```

`<dir_upload>` berisi file berakhiran `1.12.2.jar`, `lwjgl-glfw-classes.jar`, dan opsional `libpojavexec.so`; tanpa `.so`, daftar EGL launcher diambil dari bagian *Konteks EGL* di bawah. Indeks ditulis ke `index/` (tidak di-commit); semua alat hanya mencetak ringkasan.

| Alat | Pemakaian | Fungsi |
|---|---|---|
| `bootstrap.py` | `<dir_upload> <root> <index>` | Semua indeks + validator |
| `jarindex.py` | `<lwjgl.jar> <1.12.2.jar> <index>` | Kelas LWJGL, call site MC, cross-reference |
| `reach.py` | `<lwjgl.jar> <index>` | Fungsi GL yang dijangkau MC + set `check_*` |
| `elfscan.py` | `<lib.so> <out.json>` | Ekspor, impor, string ELF |
| `vkindex.py` | `<dir_vulkan> <out.json>` | Indeks header Vulkan |
| `validate.py` | `<root> <index> [--quiet]` | Validasi `.cpp/.hpp`; exit 1 jika error |
| `gen_gl.py` | `<root> <index>` | Tulis `src/gl/gl_enums.hpp` (nilai konstanta dari LWJGL), `gl_decl.inc`, `procs.inc` (tabel `ORYON_PROC`/`ORYON_STUB`) |
| `hosttest.py` | `<root> <work> <mesa_prefix> [--ndk <ndk>]` | Build host + smoke test di Mesa lavapipe (+ validation layer); opsional build arm64 + cek ekspor ELF |
| `jtrace.py` | `<jar> <kelas> [method ...] [--ints]` | Jejak bytecode ringkas |
| `a64trace.py` | `<lib.so> <fungsi ...>` | Jejak AArch64 ringkas |
| `a64dis.py` | `<lib.so> <fungsi ...>` | Disassembly AArch64 beranotasi |
| `axml.py` | `<apk> [entri] [--res]` | Dekode `AndroidManifest.xml` + `resources.arsc` dari APK |
| `jcls.py` | — | Modul parser class file |

Penanda yang dibaca `validate.py`: `OGL_API` (fungsi GL), `ORYON_PROC`/`ORYON_STUB` (tabel GetProcAddress), `ORYON_CAP_VERSION`/`ORYON_CAP_EXT` (klaim kapabilitas), `OEGL_API` (fungsi EGL), `constexpr T GL_X = v;` (konstanta). Simbol `vk*`/`Vk*`/`VK_*` wajib ada di header 131.

## Build & tes

CI (`.github/workflows/build.yml`, 4 step): checkout → NDK r30 → CMake arm64-v8a (`android-26`, Release, stripped) → upload `build/liboryon.so`.

Lokal: `python3 -I tools/hosttest.py . <work> <mesa_prefix> --ndk <ndk>` memakai Mesa 26.0.0 lavapipe (`-Dvulkan-drivers=swrast -Dgallium-drivers=llvmpipe`) sebagai driver Vulkan sungguhan; `tests/host/smoke.cpp` memanggil `liboryon.so` persis seperti launcher (`dlopen` + `dlsym` + `eglGetProcAddress`).

## Struktur

| Path | Isi |
|---|---|
| `src/vk/` | Loader Vulkan (`dlopen`), device 1.1, framebuffer default + perekam command buffer, swapchain Android |
| `src/egl/` | 20 fungsi EGL launcher, satu display + satu config |
| `src/gl/` | State GL per konteks, fungsi `OGL_API`, file hasil `gen_gl.py` |

Framebuffer default disimpan bottom-up (baris image = y window GL): `glReadPixels`/salin tekstur tanpa flip, flip hanya saat present (blit).

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
