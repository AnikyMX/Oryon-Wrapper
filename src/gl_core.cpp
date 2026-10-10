// State queries, clears, viewport/scissor and readback on the default framebuffer.
#include "context.hpp"
#include "gl_api.hpp"
#include "proc.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

using namespace oryon;

namespace {

static_assert(GL_TEXTURE_GEN_T - GL_TEXTURE_GEN_S == 1 && GL_TEXTURE_GEN_R - GL_TEXTURE_GEN_S == 2 &&
              GL_TEXTURE_GEN_Q - GL_TEXTURE_GEN_S == 3);

// Per-unit caps (GL_TEXTURE_2D, GL_TEXTURE_GEN_*) or nullptr.
bool* unit_cap(Context* c, GLenum cap, uint8_t& gen_bit) {
  TexUnit& u = c->units[c->active];
  if (cap == GL_TEXTURE_2D) return &u.enabled;
  if (cap - GL_TEXTURE_GEN_S < 4) gen_bit = uint8_t(1) << (cap - GL_TEXTURE_GEN_S);
  return nullptr;
}

void set_cap(Context* c, GLenum cap, bool on) {
  c->ff_dirty = true;
  uint8_t gen = 0;
  if (bool* flag = unit_cap(c, cap, gen)) {
    *flag = on;
    return;
  }
  if (gen) {
    uint8_t& g = c->units[c->active].gen_on;
    g = on ? g | gen : g & ~gen;
    return;
  }
  if (const int b = cap_bit(cap); b >= 0) {
    c->caps = on ? c->caps | (uint64_t(1) << b) : c->caps & ~(uint64_t(1) << b);
    return;
  }
  for (uint32_t i = 0; i < c->extra_count; ++i)
    if (c->extra_caps[i] == cap) {
      if (!on) c->extra_caps[i] = c->extra_caps[--c->extra_count];
      return;
    }
  if (on && c->extra_count < std::size(c->extra_caps)) c->extra_caps[c->extra_count++] = cap;
}

bool get_cap(Context* c, GLenum cap) {
  uint8_t gen = 0;
  if (const bool* flag = unit_cap(c, cap, gen)) return *flag;
  if (gen) return c->units[c->active].gen_on & gen;
  if (const int b = cap_bit(cap); b >= 0) return (c->caps >> b) & 1;
  return std::find(c->extra_caps, c->extra_caps + c->extra_count, cap) != c->extra_caps + c->extra_count;
}

// Fills v; returns the value count (0 = unknown pname). `color` marks normalized values.
int query(Context* c, GLenum pname, double* v, bool& color) {
  const VkPhysicalDeviceLimits& lim = dev().props.limits;
  const VkFormat depth = c->draw ? c->draw->depth.format : VK_FORMAT_UNDEFINED;
  color = false;
  switch (pname) {
    case GL_VIEWPORT: std::copy(c->viewport, c->viewport + 4, v); return 4;
    case GL_SCISSOR_BOX: std::copy(c->scissor, c->scissor + 4, v); return 4;
    case GL_MAX_TEXTURE_SIZE: v[0] = lim.maxImageDimension2D; return 1;
    case GL_MAX_VIEWPORT_DIMS: v[0] = lim.maxViewportDimensions[0]; v[1] = lim.maxViewportDimensions[1]; return 2;
    case GL_RED_BITS:
    case GL_GREEN_BITS:
    case GL_BLUE_BITS:
    case GL_ALPHA_BITS: v[0] = c->draw ? 8 : 0; return 1;
    case GL_DEPTH_BITS: v[0] = depth == VK_FORMAT_D24_UNORM_S8_UINT ? 24 : depth != VK_FORMAT_UNDEFINED ? 32 : 0; return 1;
    case GL_STENCIL_BITS: v[0] = format_has_stencil(depth) ? 8 : 0; return 1;
    case GL_COLOR_CLEAR_VALUE: std::copy(c->clear_color, c->clear_color + 4, v); color = true; return 4;
    case GL_DEPTH_CLEAR_VALUE: v[0] = c->clear_depth; color = true; return 1;
    case GL_STENCIL_CLEAR_VALUE: v[0] = c->clear_stencil; return 1;
    case GL_PACK_ALIGNMENT: v[0] = c->pack_alignment; return 1;
    case GL_UNPACK_ALIGNMENT: v[0] = c->unpack_alignment; return 1;
    case GL_MODELVIEW_MATRIX: std::copy(top(c->modelview).m, top(c->modelview).m + 16, v); return 16;
    case GL_PROJECTION_MATRIX: std::copy(top(c->projection).m, top(c->projection).m + 16, v); return 16;
    case GL_TEXTURE_MATRIX: {
      const Mat4& m = top(c->units[c->active].matrix);
      std::copy(m.m, m.m + 16, v);
      return 16;
    }
    case GL_MAJOR_VERSION: v[0] = kCapMajor; return kCapMajor >= 3;
    case GL_MINOR_VERSION: v[0] = kCapMinor; return kCapMajor >= 3;
    case GL_NUM_EXTENSIONS:
    case GL_CONTEXT_FLAGS: v[0] = 0; return kCapMajor >= 3;
    default: {
      uint8_t gen = 0;
      if (cap_bit(pname) < 0 && !unit_cap(c, pname, gen) && !gen) return 0;
      v[0] = get_cap(c, pname);
      return 1;
    }
  }
}

}  // namespace

void oryon::gl_defaults(Context& c) {
  c.caps = cap_mask(GL_DITHER) | cap_mask(GL_MULTISAMPLE);
  c.modelview.s[0] = c.projection.s[0] = mat_identity();
  const int32_t modulate = 1 | 0 << 4 | 3 << 8 | 1 << 12;  // MODULATE(TEXTURE, PREVIOUS, CONSTANT)
  for (TexUnit& u : c.units) {
    u.matrix.s[0] = mat_identity();
    u.combine[0] = modulate | 0 << 16 | 0 << 20 | 2 << 24;  // SRC_COLOR, SRC_COLOR, SRC_ALPHA
    u.combine[1] = modulate | 2 << 16 | 2 << 20 | 2 << 24;  // SRC_ALPHA x3
    for (int i = 0; i < 4; ++i) u.gen_mode[i] = GL_EYE_LINEAR;
    u.gen_object[0][0] = u.gen_eye[0][0] = 1.0f;  // S = (1,0,0,0), T = (0,1,0,0), R = Q = 0
    u.gen_object[1][1] = u.gen_eye[1][1] = 1.0f;
  }
  const GLfloat one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  std::copy(one, one + 4, c.current[A_COLOR]);
  c.current[A_TEX0][3] = c.current[A_TEX1][3] = 1.0f;
  c.current[A_NORMAL][2] = 1.0f;
  std::copy(one, one + 4, c.light_diffuse[0]);
  for (int i = 0; i < 2; ++i) {
    c.light_pos[i][2] = 1.0f;
    c.light_ambient[i][3] = c.light_diffuse[1][3] = 1.0f;
  }
  const GLfloat ambient[4] = {0.2f, 0.2f, 0.2f, 1.0f};
  std::copy(ambient, ambient + 4, c.scene_ambient);
}

OGL_API void glEnable(GLenum cap) {
  if (Context* c = current) set_cap(c, cap, true);
}

OGL_API void glDisable(GLenum cap) {
  if (Context* c = current) set_cap(c, cap, false);
}

OGL_API GLboolean glIsEnabled(GLenum cap) {
  Context* c = current;
  return c && get_cap(c, cap) ? GL_TRUE : GL_FALSE;
}

OGL_API GLenum glGetError(void) {
  Context* c = current;
  if (!c) return GL_NO_ERROR;
  const GLenum e = c->error;
  c->error = GL_NO_ERROR;
  return e;
}

OGL_API const GLubyte *glGetString(GLenum name) {
  Context* c = current;
  if (!c) return nullptr;
  const char* s;
  switch (name) {
    case GL_VENDOR: s = "Oryon"; break;
    case GL_RENDERER: s = c->renderer; break;
    case GL_VERSION: s = c->version; break;
    case GL_EXTENSIONS: s = ""; break;
    default: gl_error(c, GL_INVALID_ENUM); return nullptr;
  }
  return reinterpret_cast<const GLubyte*>(s);
}

OGL_API void glGetIntegerv(GLenum pname, GLint *data) {
  Context* c = current;
  if (!c || !data) return;
  double v[16];
  bool color;
  const int n = query(c, pname, v, color);
  if (!n) return gl_error(c, GL_INVALID_ENUM);
  for (int i = 0; i < n; ++i)
    data[i] = color ? static_cast<GLint>(std::clamp(std::llround(v[i] * 2147483647.0), -2147483647LL - 1, 2147483647LL))
                    : static_cast<GLint>(std::lround(v[i]));
}

OGL_API void glGetFloatv(GLenum pname, GLfloat *data) {
  Context* c = current;
  if (!c || !data) return;
  double v[16];
  bool color;
  const int n = query(c, pname, v, color);
  if (!n) return gl_error(c, GL_INVALID_ENUM);
  for (int i = 0; i < n; ++i) data[i] = static_cast<GLfloat>(v[i]);
}

OGL_API void glGetBooleanv(GLenum pname, GLboolean *data) {
  Context* c = current;
  if (!c || !data) return;
  double v[16];
  bool color;
  const int n = query(c, pname, v, color);
  if (!n) return gl_error(c, GL_INVALID_ENUM);
  for (int i = 0; i < n; ++i) data[i] = v[i] != 0.0 ? GL_TRUE : GL_FALSE;
}

OGL_API void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
  Context* c = current;
  if (!c) return;
  c->clear_color[0] = std::clamp(red, 0.0f, 1.0f);
  c->clear_color[1] = std::clamp(green, 0.0f, 1.0f);
  c->clear_color[2] = std::clamp(blue, 0.0f, 1.0f);
  c->clear_color[3] = std::clamp(alpha, 0.0f, 1.0f);
}

OGL_API void glClearDepth(GLdouble depth) {
  if (Context* c = current) c->clear_depth = static_cast<GLfloat>(std::clamp(depth, 0.0, 1.0));
}

OGL_API void glClearStencil(GLint s) {
  if (Context* c = current) c->clear_stencil = s;
}

OGL_API void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
  Context* c = current;
  if (!c) return;
  if (width < 0 || height < 0) return gl_error(c, GL_INVALID_VALUE);
  c->viewport[0] = x;
  c->viewport[1] = y;
  c->viewport[2] = width;
  c->viewport[3] = height;
}

OGL_API void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
  Context* c = current;
  if (!c) return;
  if (width < 0 || height < 0) return gl_error(c, GL_INVALID_VALUE);
  c->scissor[0] = x;
  c->scissor[1] = y;
  c->scissor[2] = width;
  c->scissor[3] = height;
}

// Full-surface clears become render pass load ops; scissored ones use vkCmdClearAttachments in the pass.
OGL_API void glClear(GLbitfield mask) {
  Context* c = current;
  if (!c) return;
  if (mask & ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) return gl_error(c, GL_INVALID_VALUE);
  Surface* s = c->draw;
  if (!s) return;
  uint32_t bits = (mask & GL_COLOR_BUFFER_BIT) ? CLEAR_COLOR : 0;
  if (s->depth.image) {
    if (mask & GL_DEPTH_BUFFER_BIT) bits |= CLEAR_DEPTH;
    if ((mask & GL_STENCIL_BUFFER_BIT) && format_has_stencil(s->depth.format)) bits |= CLEAR_STENCIL;
  }
  if (!bits) return;
  const int32_t w = static_cast<int32_t>(s->color.width), h = static_cast<int32_t>(s->color.height);
  int32_t x0 = 0, y0 = 0, x1 = w, y1 = h;
  if (c->caps & cap_mask(GL_SCISSOR_TEST)) {
    x0 = std::clamp(c->scissor[0], 0, w);
    y0 = std::clamp(c->scissor[1], 0, h);
    x1 = std::clamp(c->scissor[0] + c->scissor[2], x0, w);
    y1 = std::clamp(c->scissor[1] + c->scissor[3], y0, h);
    if (x0 == x1 || y0 == y1) return;
  }
  VkClearValue color, ds;
  std::memcpy(color.color.float32, c->clear_color, sizeof c->clear_color);
  ds.depthStencil.depth = c->clear_depth;
  ds.depthStencil.stencil = static_cast<uint32_t>(c->clear_stencil);
  if (!c->in_pass && x0 == 0 && y0 == 0 && x1 == w && y1 == h) {
    c->pending |= bits;
    if (bits & CLEAR_COLOR) c->pending_values[0] = color;
    if (bits & CLEAR_DEPTH) c->pending_values[1].depthStencil.depth = ds.depthStencil.depth;
    if (bits & CLEAR_STENCIL) c->pending_values[1].depthStencil.stencil = ds.depthStencil.stencil;
    return;
  }
  ctx_begin_pass(*c);
  VkClearAttachment att[2];
  uint32_t n = 0;
  if (bits & CLEAR_COLOR) att[n++] = {VK_IMAGE_ASPECT_COLOR_BIT, 0, color};
  const VkImageAspectFlags da = ((bits & CLEAR_DEPTH) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                ((bits & CLEAR_STENCIL) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
  if (da) att[n++] = {da, 0, ds};
  VkClearRect rect{.rect = {{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}},
                   .baseArrayLayer = 0,
                   .layerCount = 1};
  vkCmdClearAttachments(c->cmd, n, att, 1, &rect);
}

OGL_API void glFlush(void) {
  if (Context* c = current) ctx_submit(*c, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, false);
}

OGL_API void glFinish(void) {
  if (Context* c = current) ctx_submit(*c, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, true);
}

// GL_RGBA / GL_BGRA as bytes (the *_8_8_8_8_REV packing is byte-identical on little-endian).
OGL_API void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels) {
  Context* c = current;
  if (!c) return;
  if (width < 0 || height < 0) return gl_error(c, GL_INVALID_VALUE);
  if ((format != GL_RGBA && format != GL_BGRA) || (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_INT_8_8_8_8_REV))
    return gl_error(c, GL_INVALID_ENUM);
  Surface* s = c->read;
  if (!s || !pixels) return;
  const int32_t x0 = std::max(x, 0), y0 = std::max(y, 0);
  const int32_t x1 = std::min<int64_t>(int64_t(x) + width, s->color.width);
  const int32_t y1 = std::min<int64_t>(int64_t(y) + height, s->color.height);
  if (x0 >= x1 || y0 >= y1) return;
  const uint32_t cw = static_cast<uint32_t>(x1 - x0), ch = static_cast<uint32_t>(y1 - y0);
  const VkDeviceSize bytes = VkDeviceSize(cw) * ch * 4;
  if (c->readback.size < bytes) {
    dev_destroy(c->readback);
    if (!dev_host_buffer(c->readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                         VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
      return gl_error(c, GL_OUT_OF_MEMORY);
  }
  ctx_end_pass(*c);
  VkCommandBuffer cmd = ctx_cmd(*c);
  image_barrier(cmd, s->color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  VkBufferImageCopy region{.bufferOffset = 0,
                           .bufferRowLength = 0,
                           .bufferImageHeight = 0,
                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageOffset = {x0, y0, 0},
                           .imageExtent = {cw, ch, 1}};
  vkCmdCopyImageToBuffer(cmd, s->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->readback.buffer, 1, &region);
  image_barrier(cmd, s->color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  VkMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                       .pNext = nullptr,
                       .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                       .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0,
                       nullptr);
  ctx_submit(*c, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, true);
  if (!c->readback.coherent) {
    VkMappedMemoryRange range{.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                              .pNext = nullptr,
                              .memory = c->readback.memory,
                              .offset = 0,
                              .size = VK_WHOLE_SIZE};
    vkInvalidateMappedMemoryRanges(dev().dev, 1, &range);
  }
  const auto* src = static_cast<const uint8_t*>(c->readback.map);
  auto* dst = static_cast<uint8_t*>(pixels) + (size_t(y0 - y) * width + (x0 - x)) * 4;
  const size_t row = size_t(cw) * 4, stride = size_t(width) * 4;  // GL_PACK_ALIGNMENT 4 always holds for 4-byte texels
  for (uint32_t r = 0; r < ch; ++r, src += row, dst += stride) {
    if (format == GL_RGBA) {
      std::memcpy(dst, src, row);
      continue;
    }
    for (size_t i = 0; i < row; i += 4) {
      dst[i] = src[i + 2];
      dst[i + 1] = src[i + 1];
      dst[i + 2] = src[i];
      dst[i + 3] = src[i + 3];
    }
  }
}
