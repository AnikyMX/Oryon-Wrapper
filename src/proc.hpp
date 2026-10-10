// Claimed GL version and the GetProcAddress table.
#pragma once
#include <EGL/egl.h>

// Claims every GLCapabilities.OpenGLxy flag up to M.m; tools/validate.py checks the proc table covers them.
#define ORYON_CAP_VERSION(M, m) inline constexpr int kCapMajor = M, kCapMinor = m;

namespace oryon {

ORYON_CAP_VERSION(1, 0)

__eglMustCastToProperFunctionPointerType proc_lookup(const char* name);

}  // namespace oryon
