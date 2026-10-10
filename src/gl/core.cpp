#include "gl/caps.hpp"
#include "gl/context.hpp"
#include <cstring>
#include <string>

namespace {
template <typename T>
T clamp01(T v) {
    return v < T(0) ? T(0) : v > T(1) ? T(1) : v;
}

// Generic state query: up to 4 values, returns count (0 = unknown pname).
int query(gl::Context* c, GLenum pname, double* v) {
    const VkPhysicalDeviceLimits& lim = ovk::gpu.props.limits;
    switch (pname) {
    case GL_MAJOR_VERSION: v[0] = gl::kVersionMajor; return 1;
    case GL_MINOR_VERSION: v[0] = gl::kVersionMinor; return 1;
    case GL_NUM_EXTENSIONS: v[0] = gl::kNumExtensions; return 1;
    case GL_MAX_TEXTURE_SIZE: v[0] = lim.maxImageDimension2D; return 1;
    case GL_MAX_VIEWPORT_DIMS: v[0] = lim.maxViewportDimensions[0]; v[1] = lim.maxViewportDimensions[1]; return 2;
    case GL_VIEWPORT:
        for (int i = 0; i < 4; i++) v[i] = c->viewport[i];
        return 4;
    case GL_COLOR_CLEAR_VALUE:
        for (int i = 0; i < 4; i++) v[i] = c->clear_color[i];
        return 4;
    case GL_DEPTH_CLEAR_VALUE: v[0] = c->clear_depth; return 1;
    case GL_PACK_ALIGNMENT: v[0] = c->pack_alignment; return 1;
    case GL_UNPACK_ALIGNMENT: v[0] = c->unpack_alignment; return 1;
    case GL_RED_BITS:
    case GL_GREEN_BITS:
    case GL_BLUE_BITS:
    case GL_ALPHA_BITS: v[0] = 8; return 1;
    case GL_DEPTH_BITS: v[0] = ovk::gpu.depth_fmt == VK_FORMAT_D24_UNORM_S8_UINT ? 24 : 32; return 1;
    case GL_STENCIL_BITS: v[0] = 8; return 1;
    default: return 0;
    }
}

bool normalized(GLenum pname) { return pname == GL_COLOR_CLEAR_VALUE || pname == GL_DEPTH_CLEAR_VALUE; }
}

OGL_API GLenum glGetError() {
    gl::Context* c = gl::current();
    if (!c) return GL_NO_ERROR;
    const GLenum e = c->error;
    c->error = GL_NO_ERROR;
    return e;
}

OGL_API const GLubyte* glGetString(GLenum name) {
    gl::Context* c = gl::current();
    if (!c) return nullptr;
    const char* s = nullptr;
    switch (name) {
    case GL_VENDOR: s = gl::kVendor; break;
    case GL_RENDERER: s = ovk::gpu.renderer; break;
    case GL_VERSION: s = gl::kVersion; break;
    case GL_SHADING_LANGUAGE_VERSION: s = gl::kGlslVersion; break;
    case GL_EXTENSIONS: {
        static const std::string all = [] {
            std::string r;
            for (int i = 0; i < gl::kNumExtensions; i++) (r += gl::kExtensions[i]) += ' ';
            return r;
        }();
        s = all.c_str();
        break;
    }
    default: gl::error(c, GL_INVALID_ENUM); return nullptr;
    }
    return reinterpret_cast<const GLubyte*>(s);
}

OGL_API const GLubyte* glGetStringi(GLenum name, GLuint index) {
    gl::Context* c = gl::current();
    if (!c) return nullptr;
    if (name != GL_EXTENSIONS) {
        gl::error(c, GL_INVALID_ENUM);
        return nullptr;
    }
    if (index >= GLuint(gl::kNumExtensions)) {
        gl::error(c, GL_INVALID_VALUE);
        return nullptr;
    }
    return reinterpret_cast<const GLubyte*>(gl::kExtensions[index]);
}

OGL_API void glGetIntegerv(GLenum pname, GLint* data) {
    gl::Context* c = gl::current();
    if (!c) return;
    double v[4];
    const int n = query(c, pname, v);
    if (!n) return gl::error(c, GL_INVALID_ENUM);
    for (int i = 0; i < n; i++) {
        // Normalized values map [-1, 1] onto the full integer range.
        data[i] = normalized(pname) ? GLint(v[i] * 2147483647.0) : GLint(v[i]);
    }
}

OGL_API void glGetFloatv(GLenum pname, GLfloat* data) {
    gl::Context* c = gl::current();
    if (!c) return;
    double v[4];
    const int n = query(c, pname, v);
    if (!n) return gl::error(c, GL_INVALID_ENUM);
    for (int i = 0; i < n; i++) data[i] = GLfloat(v[i]);
}

OGL_API void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
    gl::Context* c = gl::current();
    if (!c) return;
    c->clear_color[0] = red;
    c->clear_color[1] = green;
    c->clear_color[2] = blue;
    c->clear_color[3] = alpha;
}

OGL_API void glClearDepth(GLdouble depth) {
    gl::Context* c = gl::current();
    if (!c) return;
    c->clear_depth = clamp01(depth);
}

OGL_API void glClear(GLbitfield mask) {
    gl::Context* c = gl::current();
    if (!c) return;
    if (mask & ~GLbitfield(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_ACCUM_BUFFER_BIT))
        return gl::error(c, GL_INVALID_VALUE);
    ovk::Target* t = c->draw;
    if (!t) return;
    VkClearAttachment att[2] = {};
    uint32_t n = 0;
    if (mask & GL_COLOR_BUFFER_BIT) {
        att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att[n].colorAttachment = 0;
        for (int i = 0; i < 4; i++) att[n].clearValue.color.float32[i] = clamp01(c->clear_color[i]);
        n++;
    }
    VkImageAspectFlags ds = 0;
    if (mask & GL_DEPTH_BUFFER_BIT) ds |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (mask & GL_STENCIL_BUFFER_BIT) ds |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (ds) {
        att[n].aspectMask = ds;
        att[n].clearValue.depthStencil = {float(c->clear_depth), 0};
        n++;
    }
    if (!n) return;
    VkClearRect rect{{{0, 0}, {t->w, t->h}}, 0, 1};
    ovk::vkCmdClearAttachments(c->rec.inside(*t), n, att, 1, &rect);
}

OGL_API void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    gl::Context* c = gl::current();
    if (!c) return;
    if (width < 0 || height < 0) return gl::error(c, GL_INVALID_VALUE);
    const GLint mw = GLint(ovk::gpu.props.limits.maxViewportDimensions[0]);
    const GLint mh = GLint(ovk::gpu.props.limits.maxViewportDimensions[1]);
    c->viewport[0] = x;
    c->viewport[1] = y;
    c->viewport[2] = width < mw ? width : mw;
    c->viewport[3] = height < mh ? height : mh;
}

OGL_API void glFlush() {
    if (gl::Context* c = gl::current()) c->rec.submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, false);
}

OGL_API void glFinish() {
    if (gl::Context* c = gl::current()) c->rec.submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, true);
}

OGL_API void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
    gl::Context* c = gl::current();
    if (!c) return;
    if (width < 0 || height < 0) return gl::error(c, GL_INVALID_VALUE);
    if ((format != GL_RGBA && format != GL_BGRA) || (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_INT_8_8_8_8_REV)) {
        OLOG("glReadPixels: format 0x%x type 0x%x not supported", format, type);
        return gl::error(c, GL_INVALID_ENUM);
    }
    ovk::Target* t = c->read;
    if (!t || !pixels) return;
    // Pixels outside the framebuffer are left untouched.
    const GLint x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    const GLint x1 = x + width > GLint(t->w) ? GLint(t->w) : x + width;
    const GLint y1 = y + height > GLint(t->h) ? GLint(t->h) : y + height;
    if (x1 <= x0 || y1 <= y0) return;
    const uint32_t w = uint32_t(x1 - x0), h = uint32_t(y1 - y0);
    const uint8_t* src = c->rec.readback(*t, x0, y0, w, h);
    if (!src) return gl::error(c, GL_OUT_OF_MEMORY);
    const size_t a = size_t(c->pack_alignment);
    const size_t stride = (size_t(width) * 4 + a - 1) / a * a;
    uint8_t* dst = static_cast<uint8_t*>(pixels) + size_t(y0 - y) * stride + size_t(x0 - x) * 4;
    for (uint32_t row = 0; row < h; row++, src += w * 4, dst += stride) {
        if (format == GL_RGBA) {
            std::memcpy(dst, src, w * 4);
            continue;
        }
        for (uint32_t i = 0; i < w * 4; i += 4) {
            dst[i + 0] = src[i + 2];
            dst[i + 1] = src[i + 1];
            dst[i + 2] = src[i + 0];
            dst[i + 3] = src[i + 3];
        }
    }
}
