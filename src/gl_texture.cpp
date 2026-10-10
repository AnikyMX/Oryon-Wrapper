// Texture objects: RGBA8 VkImage (+ mips), uploads through the transient ring, sampler cache.
#include "context.hpp"
#include "gl_api.hpp"
#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>

using namespace oryon;

namespace {

std::unordered_map<GLuint, Texture*> textures;  // shortcut: one global share group, GL thread only
GLuint next_name = 1;
GLsizei proxy[2] = {0, 0};

struct SamplerKey {
  VkFilter mag, min;
  VkSamplerMipmapMode mip;
  VkSamplerAddressMode u, v;
  float min_lod, max_lod, bias;
  bool operator<(const SamplerKey& o) const { return std::memcmp(this, &o, sizeof *this) < 0; }
};
std::map<SamplerKey, VkSampler> samplers;

Texture* lookup(GLuint name) {
  Texture*& t = textures[name];
  if (!t) t = new Texture();
  return t;
}

Texture& bound(Context* c) {
  TexUnit& u = c->units[c->active];
  if (!u.texture) u.texture = lookup(u.name);
  return *u.texture;
}

VkSamplerAddressMode address(GLenum wrap) {
  if (wrap == GL_REPEAT) return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  if (wrap == GL_MIRRORED_REPEAT) return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;  // shortcut: GL_CLAMP (border blend) samples as clamp-to-edge
}

VkSampler sampler(const Texture& t) {
  const VkPhysicalDeviceLimits& lim = dev().props.limits;
  const GLenum f = t.min_filter;
  const bool mip = f != GL_NEAREST && f != GL_LINEAR;
  const float top = static_cast<float>(std::max(0, std::min<int>(t.max_level, static_cast<int>(t.levels) - 1)));
  SamplerKey k{};
  k.mag = t.mag_filter == GL_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  k.min = (f == GL_NEAREST || f == GL_NEAREST_MIPMAP_NEAREST || f == GL_NEAREST_MIPMAP_LINEAR) ? VK_FILTER_NEAREST
                                                                                                : VK_FILTER_LINEAR;
  k.mip = (f == GL_NEAREST_MIPMAP_LINEAR || f == GL_LINEAR_MIPMAP_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                           : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  k.u = address(t.wrap_s);
  k.v = address(t.wrap_t);
  k.min_lod = mip ? std::clamp(t.min_lod, 0.0f, top) : 0.0f;
  k.max_lod = mip ? std::clamp(t.max_lod, k.min_lod, top) : 0.25f;  // 0.25: base level only, keeps min/mag choice
  k.bias = std::clamp(t.lod_bias, -lim.maxSamplerLodBias, lim.maxSamplerLodBias);
  VkSampler& s = samplers[k];
  if (!s) {
    VkSamplerCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                           .pNext = nullptr,
                           .flags = 0,
                           .magFilter = k.mag,
                           .minFilter = k.min,
                           .mipmapMode = k.mip,
                           .addressModeU = k.u,
                           .addressModeV = k.v,
                           .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                           .mipLodBias = k.bias,
                           .anisotropyEnable = VK_FALSE,
                           .maxAnisotropy = 1.0f,
                           .compareEnable = VK_FALSE,
                           .compareOp = VK_COMPARE_OP_ALWAYS,
                           .minLod = k.min_lod,
                           .maxLod = k.max_lod,
                           .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                           .unnormalizedCoordinates = VK_FALSE};
    vkCreateSampler(dev().dev, &ci, nullptr, &s);
  }
  return s;
}

bool pixel_format(GLenum format, GLenum type) {  // shortcut: 8-bit RGBA/BGRA only (all MC uploads)
  return (format == GL_RGBA || format == GL_BGRA) && (type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_INT_8_8_8_8_REV);
}

uint32_t level_size(uint32_t base, GLint level) { return std::max(1u, base >> level); }

void changed(Context* c) {
  c->tex_dirty = true;
  c->ff_dirty = true;
}

bool allocate(Context* c, Texture& t, uint32_t w, uint32_t h, uint32_t levels) {
  ctx_discard(*c, t.image);
  t.levels = 0;
  t.sampler = VK_NULL_HANDLE;
  changed(c);
  if (!dev_image(t.image, VK_FORMAT_R8G8B8A8_UNORM, w, h,
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT, levels))
    return false;
  t.levels = levels;
  ctx_end_pass(*c);
  image_barrier(ctx_cmd(*c), t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, levels);
  return true;
}

// Brackets transfer writes into one texture level; the image otherwise stays SHADER_READ_ONLY.
VkCommandBuffer begin_write(Context* c, const Texture& t, GLint level) {
  ctx_end_pass(*c);
  VkCommandBuffer cmd = ctx_cmd(*c);
  image_barrier(cmd, t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, static_cast<uint32_t>(level), 1);
  return cmd;
}

void end_write(VkCommandBuffer cmd, const Texture& t, GLint level) {
  image_barrier(cmd, t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, static_cast<uint32_t>(level), 1);
}

void upload(Context* c, Texture& t, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
            const void* pixels) {
  const size_t row = size_t(w) * 4, a = static_cast<size_t>(c->unpack_alignment);
  const size_t pitch = (row + a - 1) / a * a;
  const Alloc dst = ctx_alloc(*c, VkDeviceSize(row) * h, 16);
  if (!dst.ptr) return gl_error(c, GL_OUT_OF_MEMORY);
  const auto* src = static_cast<const uint8_t*>(pixels);
  for (GLsizei r = 0; r < h; ++r, src += pitch) {
    uint8_t* d = dst.ptr + row * r;
    if (format == GL_RGBA) {
      std::memcpy(d, src, row);
      continue;
    }
    for (size_t i = 0; i < row; i += 4) {  // BGRA -> RGBA
      d[i] = src[i + 2];
      d[i + 1] = src[i + 1];
      d[i + 2] = src[i];
      d[i + 3] = src[i + 3];
    }
  }
  VkCommandBuffer cmd = begin_write(c, t, level);
  VkBufferImageCopy region{.bufferOffset = dst.offset,
                           .bufferRowLength = 0,
                           .bufferImageHeight = 0,
                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(level), 0, 1},
                           .imageOffset = {x, y, 0},
                           .imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1}};
  vkCmdCopyBufferToImage(cmd, dst.buffer, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  end_write(cmd, t, level);
}

void tex_param(Context* c, GLenum target, GLenum pname, GLfloat f, GLint i) {
  if (target != GL_TEXTURE_2D) return gl_error(c, GL_INVALID_ENUM);
  Texture& t = bound(c);
  const GLenum e = static_cast<GLenum>(i);
  switch (pname) {
    case GL_TEXTURE_MIN_FILTER:
      if (e != GL_NEAREST && e != GL_LINEAR && e != GL_NEAREST_MIPMAP_NEAREST && e != GL_LINEAR_MIPMAP_NEAREST &&
          e != GL_NEAREST_MIPMAP_LINEAR && e != GL_LINEAR_MIPMAP_LINEAR)
        return gl_error(c, GL_INVALID_ENUM);
      t.min_filter = e;
      break;
    case GL_TEXTURE_MAG_FILTER:
      if (e != GL_NEAREST && e != GL_LINEAR) return gl_error(c, GL_INVALID_ENUM);
      t.mag_filter = e;
      break;
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T:
      if (e != GL_REPEAT && e != GL_CLAMP && e != GL_CLAMP_TO_EDGE && e != GL_MIRRORED_REPEAT)
        return gl_error(c, GL_INVALID_ENUM);
      (pname == GL_TEXTURE_WRAP_S ? t.wrap_s : t.wrap_t) = e;
      break;
    case GL_TEXTURE_MAX_LEVEL:
      if (i < 0) return gl_error(c, GL_INVALID_VALUE);
      t.max_level = i;
      break;
    case GL_TEXTURE_MIN_LOD: t.min_lod = f; break;
    case GL_TEXTURE_MAX_LOD: t.max_lod = f; break;
    case GL_TEXTURE_LOD_BIAS: t.lod_bias = f; break;
    default: return gl_error(c, GL_INVALID_ENUM);
  }
  t.sampler = VK_NULL_HANDLE;
  changed(c);
}

}  // namespace

VkDescriptorImageInfo oryon::tex_descriptor(Context& c, int unit) {
  Texture* t = c.units[unit].texture;
  if (!tex_ready(c.units[unit])) {
    static Texture plain = [] {
      Texture p;
      p.min_filter = p.mag_filter = GL_NEAREST;
      p.levels = 1;
      return p;
    }();
    return {sampler(plain), dev().white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  }
  if (!t->sampler) t->sampler = sampler(*t);
  return {t->sampler, t->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

OGL_API void glActiveTexture(GLenum texture) {
  Context* c = current;
  if (!c) return;
  if (texture - GL_TEXTURE0 >= static_cast<GLenum>(kUnits)) return gl_error(c, GL_INVALID_ENUM);
  c->active = static_cast<int>(texture - GL_TEXTURE0);
}

OGL_API void glActiveTextureARB(GLenum texture) {
  glActiveTexture(texture);
}

OGL_API void glGenTextures(GLsizei n, GLuint *textures_) {
  Context* c = current;
  if (!c || !textures_) return;
  if (n < 0) return gl_error(c, GL_INVALID_VALUE);
  for (GLsizei i = 0; i < n; ++i) textures_[i] = next_name++;
}

OGL_API void glDeleteTextures(GLsizei n, const GLuint *textures_) {
  Context* c = current;
  if (!c || !textures_) return;
  if (n < 0) return gl_error(c, GL_INVALID_VALUE);
  for (GLsizei i = 0; i < n; ++i) {
    auto it = textures.find(textures_[i]);
    if (!textures_[i] || it == textures.end()) continue;
    for (TexUnit& u : c->units)
      if (u.texture == it->second) {
        u.name = 0;
        u.texture = nullptr;
      }
    ctx_discard(*c, it->second->image);
    delete it->second;
    textures.erase(it);
  }
  changed(c);
}

OGL_API void glBindTexture(GLenum target, GLuint texture) {
  Context* c = current;
  if (!c) return;
  if (target != GL_TEXTURE_2D) return gl_error(c, GL_INVALID_ENUM);
  TexUnit& u = c->units[c->active];
  u.name = texture;
  u.texture = lookup(texture);
  changed(c);
}

OGL_API void glPixelStorei(GLenum pname, GLint param) {
  Context* c = current;
  if (!c) return;
  if (pname != GL_PACK_ALIGNMENT && pname != GL_UNPACK_ALIGNMENT)
    return gl_error(c, GL_INVALID_ENUM);  // shortcut: row length / skip parameters unsupported (MC uses alignment)
  if (param != 1 && param != 2 && param != 4 && param != 8) return gl_error(c, GL_INVALID_VALUE);
  (pname == GL_PACK_ALIGNMENT ? c->pack_alignment : c->unpack_alignment) = param;
}

OGL_API void glTexParameteri(GLenum target, GLenum pname, GLint param) {
  if (Context* c = current) tex_param(c, target, pname, static_cast<GLfloat>(param), param);
}

OGL_API void glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
  if (Context* c = current) tex_param(c, target, pname, param, static_cast<GLint>(param));
}

OGL_API void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                          GLint border, GLenum format, GLenum type, const void *pixels) {
  (void)internalformat;  // shortcut: storage is always RGBA8
  Context* c = current;
  if (!c) return;
  const GLsizei max = static_cast<GLsizei>(dev().props.limits.maxImageDimension2D);
  if (target == GL_PROXY_TEXTURE_2D) {
    const bool fits = level == 0 && width >= 0 && height >= 0 && width <= max && height <= max;
    proxy[0] = fits ? width : 0;
    proxy[1] = fits ? height : 0;
    return;
  }
  if (target != GL_TEXTURE_2D) return gl_error(c, GL_INVALID_ENUM);
  if (level < 0 || level > 15 || width < 0 || height < 0 || width > max || height > max || border)
    return gl_error(c, GL_INVALID_VALUE);
  if (!pixel_format(format, type)) return gl_error(c, GL_INVALID_ENUM);
  if (!width || !height) return;
  Texture& t = bound(c);
  const uint32_t w = static_cast<uint32_t>(width), h = static_cast<uint32_t>(height);
  const bool fits = t.image.image && static_cast<uint32_t>(level) < t.levels &&
                    level_size(t.image.width, level) == w && level_size(t.image.height, level) == h;
  if (!fits) {
    // shortcut: the mip count is fixed when the texture is (re)defined, from GL_TEXTURE_MAX_LEVEL (MC sets it first)
    const uint32_t bw = w << level, bh = h << level;
    uint32_t full = 1;
    while ((std::max(bw, bh) >> full) > 0) ++full;
    const uint32_t want = t.max_level >= 1000 ? 1 : static_cast<uint32_t>(t.max_level) + 1;
    if (!allocate(c, t, bw, bh, std::min(full, std::max(want, static_cast<uint32_t>(level) + 1))))
      return gl_error(c, GL_OUT_OF_MEMORY);
  }
  if (pixels) upload(c, t, level, 0, 0, width, height, format, pixels);
}

OGL_API void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                             GLenum format, GLenum type, const void *pixels) {
  Context* c = current;
  if (!c) return;
  if (target != GL_TEXTURE_2D || !pixel_format(format, type)) return gl_error(c, GL_INVALID_ENUM);
  Texture& t = bound(c);
  if (!t.image.image || level < 0 || static_cast<uint32_t>(level) >= t.levels) return gl_error(c, GL_INVALID_OPERATION);
  if (xoffset < 0 || yoffset < 0 || width < 0 || height < 0 ||
      uint32_t(xoffset) + uint32_t(width) > level_size(t.image.width, level) ||
      uint32_t(yoffset) + uint32_t(height) > level_size(t.image.height, level))
    return gl_error(c, GL_INVALID_VALUE);
  if (width && height && pixels) upload(c, t, level, xoffset, yoffset, width, height, format, pixels);
}

// Copies from the read surface; both images keep GL row order, so no flip.
OGL_API void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                                 GLsizei width, GLsizei height) {
  Context* c = current;
  if (!c) return;
  if (target != GL_TEXTURE_2D) return gl_error(c, GL_INVALID_ENUM);
  Texture& t = bound(c);
  Surface* s = c->read;
  if (!t.image.image || level < 0 || static_cast<uint32_t>(level) >= t.levels) return gl_error(c, GL_INVALID_OPERATION);
  if (xoffset < 0 || yoffset < 0 || width < 0 || height < 0 ||
      uint32_t(xoffset) + uint32_t(width) > level_size(t.image.width, level) ||
      uint32_t(yoffset) + uint32_t(height) > level_size(t.image.height, level))
    return gl_error(c, GL_INVALID_VALUE);
  if (!s) return;
  // Clip the source rectangle to the surface; texels outside keep their contents.
  const int32_t x0 = std::max(x, 0), y0 = std::max(y, 0);
  const int32_t x1 = std::min<int64_t>(int64_t(x) + width, s->color.width);
  const int32_t y1 = std::min<int64_t>(int64_t(y) + height, s->color.height);
  if (x0 >= x1 || y0 >= y1) return;
  VkCommandBuffer cmd = begin_write(c, t, level);
  image_barrier(cmd, s->color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  VkImageCopy region{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .srcOffset = {x0, y0, 0},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(level), 0, 1},
                     .dstOffset = {xoffset + (x0 - x), yoffset + (y0 - y), 0},
                     .extent = {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0), 1}};
  vkCmdCopyImage(cmd, s->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image.image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  image_barrier(cmd, s->color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  end_write(cmd, t, level);
}

OGL_API void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params) {
  Context* c = current;
  if (!c || !params) return;
  GLint w = 0, h = 0;
  if (target == GL_PROXY_TEXTURE_2D) {
    if (level == 0) w = proxy[0], h = proxy[1];
  } else if (target == GL_TEXTURE_2D) {
    const Texture& t = bound(c);
    if (t.image.image && level >= 0 && static_cast<uint32_t>(level) < t.levels)
      w = static_cast<GLint>(level_size(t.image.width, level)), h = static_cast<GLint>(level_size(t.image.height, level));
  } else {
    return gl_error(c, GL_INVALID_ENUM);
  }
  switch (pname) {
    case GL_TEXTURE_WIDTH: *params = w; break;
    case GL_TEXTURE_HEIGHT: *params = h; break;
    case GL_TEXTURE_INTERNAL_FORMAT: *params = w ? GLint(GL_RGBA8) : 1; break;
    default: gl_error(c, GL_INVALID_ENUM);
  }
}
