#include "oryon.hpp"
#include <cstdarg>
#include <cstdio>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace oryon {
void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_INFO, "Oryon", fmt, ap);
#else
    std::fputs("[oryon] ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
#endif
    va_end(ap);
}
}
