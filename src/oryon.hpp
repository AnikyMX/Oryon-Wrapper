// Oryon: OpenGL (LWJGL / Minecraft 1.12.2) translated to Vulkan 1.1.
#pragma once
#include <cstddef>
#include <cstdint>

#define OGL_API extern "C" __attribute__((visibility("default")))
#define OEGL_API extern "C" __attribute__((visibility("default")))

typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef signed char GLbyte;
typedef short GLshort;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef char GLchar;
typedef std::ptrdiff_t GLsizeiptr;
typedef std::ptrdiff_t GLintptr;

#include "gl_enums.hpp"

#if defined(__ANDROID__)
#include <android/log.h>
#define ORYON_LOG(...) __android_log_print(ANDROID_LOG_INFO, "Oryon", __VA_ARGS__)
#else
#include <cstdio>
#define ORYON_LOG(...) (std::fprintf(stderr, "[oryon] " __VA_ARGS__), std::fputc('\n', stderr))
#endif
