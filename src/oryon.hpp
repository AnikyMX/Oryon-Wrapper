#pragma once
#include <cstddef>
#include <cstdint>

// GL entry points are reached only through eglGetProcAddress; EGL is the only export.
#define OGL_API extern "C" __attribute__((visibility("hidden")))
#define OEGL_API extern "C" __attribute__((visibility("default")))

namespace oryon {
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
}
#define OLOG(...) ::oryon::log(__VA_ARGS__)
