// Raster and fixed-function state setters. Raster enums are stored as Vulkan values for the pipeline key.
#include "context.hpp"
#include "gl_api.hpp"
#include <algorithm>
#include <cstring>

using namespace oryon;

// GL compare / logic op enums are consecutive in the same order as VkCompareOp / VkLogicOp.
static_assert(GL_LESS - GL_NEVER == VK_COMPARE_OP_LESS && GL_EQUAL - GL_NEVER == VK_COMPARE_OP_EQUAL &&
              GL_LEQUAL - GL_NEVER == VK_COMPARE_OP_LESS_OR_EQUAL && GL_GREATER - GL_NEVER == VK_COMPARE_OP_GREATER &&
              GL_NOTEQUAL - GL_NEVER == VK_COMPARE_OP_NOT_EQUAL && GL_GEQUAL - GL_NEVER == VK_COMPARE_OP_GREATER_OR_EQUAL &&
              GL_ALWAYS - GL_NEVER == VK_COMPARE_OP_ALWAYS);
static_assert(GL_AND - GL_CLEAR == VK_LOGIC_OP_AND && GL_COPY - GL_CLEAR == VK_LOGIC_OP_COPY &&
              GL_OR_REVERSE - GL_CLEAR == VK_LOGIC_OP_OR_REVERSE && GL_SET - GL_CLEAR == VK_LOGIC_OP_SET);
static_assert(GL_T - GL_S == 1 && GL_R - GL_S == 2 && GL_Q - GL_S == 3);

namespace {

bool compare_op(GLenum func, VkCompareOp& out) {
  if (func < GL_NEVER || func > GL_ALWAYS) return false;
  out = static_cast<VkCompareOp>(func - GL_NEVER);
  return true;
}

bool blend_factor(GLenum f, VkBlendFactor& out) {
  switch (f) {
    case GL_ZERO: out = VK_BLEND_FACTOR_ZERO; return true;
    case GL_ONE: out = VK_BLEND_FACTOR_ONE; return true;
    case GL_SRC_COLOR: out = VK_BLEND_FACTOR_SRC_COLOR; return true;
    case GL_ONE_MINUS_SRC_COLOR: out = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR; return true;
    case GL_DST_COLOR: out = VK_BLEND_FACTOR_DST_COLOR; return true;
    case GL_ONE_MINUS_DST_COLOR: out = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; return true;
    case GL_SRC_ALPHA: out = VK_BLEND_FACTOR_SRC_ALPHA; return true;
    case GL_ONE_MINUS_SRC_ALPHA: out = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; return true;
    case GL_DST_ALPHA: out = VK_BLEND_FACTOR_DST_ALPHA; return true;
    case GL_ONE_MINUS_DST_ALPHA: out = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA; return true;
    case GL_CONSTANT_COLOR: out = VK_BLEND_FACTOR_CONSTANT_COLOR; return true;
    case GL_ONE_MINUS_CONSTANT_COLOR: out = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; return true;
    case GL_CONSTANT_ALPHA: out = VK_BLEND_FACTOR_CONSTANT_ALPHA; return true;
    case GL_ONE_MINUS_CONSTANT_ALPHA: out = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA; return true;
    case GL_SRC_ALPHA_SATURATE: out = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE; return true;
    default: return false;
  }
}

// Shader encodings (shaders/ff.frag): sources, operands, combine functions.
int env_source(GLenum v) {
  switch (v) {
    case GL_TEXTURE: return 0;
    case GL_CONSTANT: return 1;
    case GL_PRIMARY_COLOR: return 2;
    case GL_PREVIOUS: return 3;
    default: return v - GL_TEXTURE0 < kUnits ? 4 + int(v - GL_TEXTURE0) : -1;
  }
}

int env_operand(GLenum v) {
  switch (v) {
    case GL_SRC_COLOR: return 0;
    case GL_ONE_MINUS_SRC_COLOR: return 1;
    case GL_SRC_ALPHA: return 2;
    case GL_ONE_MINUS_SRC_ALPHA: return 3;
    default: return -1;
  }
}

int env_function(GLenum v) {
  switch (v) {
    case GL_REPLACE: return 0;
    case GL_MODULATE: return 1;
    case GL_ADD: return 2;
    case GL_ADD_SIGNED: return 3;
    case GL_INTERPOLATE: return 4;
    case GL_SUBTRACT: return 5;
    default: return -1;
  }
}

int env_mode(GLenum v) {
  switch (v) {
    case GL_MODULATE: return 0;
    case GL_REPLACE: return 1;
    case GL_DECAL: return 2;
    case GL_BLEND: return 3;
    case GL_ADD: return 4;
    case GL_COMBINE: return 5;
    default: return -1;
  }
}

void set_field(int32_t& cfg, int shift, int value) {
  cfg = (cfg & ~(15 << shift)) | (value << shift);
}

void tex_env(Context* c, GLenum target, GLenum pname, const GLfloat* f, GLint i) {
  if (target != GL_TEXTURE_ENV) return gl_error(c, GL_INVALID_ENUM);
  TexUnit& u = c->units[c->active];
  const GLenum v = static_cast<GLenum>(i);
  int x = -1;
  switch (pname) {
    case GL_TEXTURE_ENV_MODE: x = env_mode(v); if (x >= 0) u.mode = x; break;
    case GL_COMBINE_RGB: x = env_function(v); if (x >= 0) set_field(u.combine[0], 0, x); break;
    case GL_COMBINE_ALPHA: x = env_function(v); if (x >= 0) set_field(u.combine[1], 0, x); break;
    case GL_SRC0_RGB: case GL_SRC1_RGB: case GL_SRC2_RGB:
      x = env_source(v); if (x >= 0) set_field(u.combine[0], 4 + 4 * int(pname - GL_SRC0_RGB), x); break;
    case GL_SRC0_ALPHA: case GL_SRC1_ALPHA: case GL_SRC2_ALPHA:
      x = env_source(v); if (x >= 0) set_field(u.combine[1], 4 + 4 * int(pname - GL_SRC0_ALPHA), x); break;
    case GL_OPERAND0_RGB: case GL_OPERAND1_RGB: case GL_OPERAND2_RGB:
      x = env_operand(v); if (x >= 0) set_field(u.combine[0], 16 + 4 * int(pname - GL_OPERAND0_RGB), x); break;
    case GL_OPERAND0_ALPHA: case GL_OPERAND1_ALPHA: case GL_OPERAND2_ALPHA:
      x = env_operand(v) >= 2 ? env_operand(v) : -1;  // alpha takes SRC_ALPHA / ONE_MINUS_SRC_ALPHA only
      if (x >= 0) set_field(u.combine[1], 16 + 4 * int(pname - GL_OPERAND0_ALPHA), x);
      break;
    case GL_RGB_SCALE: case GL_ALPHA_SCALE:
      if (*f == 1.0f || *f == 2.0f || *f == 4.0f) u.scale[pname == GL_ALPHA_SCALE] = *f, x = 0; break;
    case GL_TEXTURE_ENV_COLOR:
      for (int k = 0; k < 4; ++k) u.env_color[k] = std::clamp(f[k], 0.0f, 1.0f);
      x = 0; break;
    default: return gl_error(c, GL_INVALID_ENUM);
  }
  if (x < 0) return gl_error(c, pname == GL_RGB_SCALE || pname == GL_ALPHA_SCALE ? GL_INVALID_VALUE : GL_INVALID_ENUM);
  c->ff_dirty = true;
}

void fog(Context* c, GLenum pname, const GLfloat* v) {
  switch (pname) {
    case GL_FOG_MODE: {
      const GLenum m = static_cast<GLenum>(v[0]);
      if (m != GL_LINEAR && m != GL_EXP && m != GL_EXP2) return gl_error(c, GL_INVALID_ENUM);
      c->fog_mode = m;
      break;
    }
    case GL_FOG_DENSITY:
      if (v[0] < 0.0f) return gl_error(c, GL_INVALID_VALUE);
      c->fog_density = v[0];
      break;
    case GL_FOG_START: c->fog_start = v[0]; break;
    case GL_FOG_END: c->fog_end = v[0]; break;
    case GL_FOG_COLOR:
      for (int k = 0; k < 4; ++k) c->fog_color[k] = std::clamp(v[k], 0.0f, 1.0f);
      break;
    default: return gl_error(c, GL_INVALID_ENUM);  // shortcut: GL_FOG_DISTANCE_MODE_NV etc. not supported
  }
  c->ff_dirty = true;
}

}  // namespace

OGL_API void glAlphaFunc(GLenum func, GLfloat ref) {
  Context* c = current;
  if (!c) return;
  if (!compare_op(func, c->alpha_func)) return gl_error(c, GL_INVALID_ENUM);
  c->alpha_ref = std::clamp(ref, 0.0f, 1.0f);
  c->ff_dirty = true;
}

OGL_API void glDepthFunc(GLenum func) {
  Context* c = current;
  if (c && !compare_op(func, c->depth_func)) gl_error(c, GL_INVALID_ENUM);
}

OGL_API void glDepthMask(GLboolean flag) {
  if (Context* c = current) c->depth_mask = flag;
}

OGL_API void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
  Context* c = current;
  if (!c) return;
  c->color_mask[0] = red;
  c->color_mask[1] = green;
  c->color_mask[2] = blue;
  c->color_mask[3] = alpha;
}

OGL_API void glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha) {
  Context* c = current;
  if (!c) return;
  VkBlendFactor f[4];
  if (!blend_factor(sfactorRGB, f[0]) || !blend_factor(dfactorRGB, f[1]) || !blend_factor(sfactorAlpha, f[2]) ||
      !blend_factor(dfactorAlpha, f[3]))
    return gl_error(c, GL_INVALID_ENUM);
  c->blend_src[0] = f[0];
  c->blend_dst[0] = f[1];
  c->blend_src[1] = f[2];
  c->blend_dst[1] = f[3];
}

OGL_API void glBlendFuncSeparateEXT(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha) {
  glBlendFuncSeparate(sfactorRGB, dfactorRGB, sfactorAlpha, dfactorAlpha);
}

OGL_API void glBlendFunc(GLenum sfactor, GLenum dfactor) {
  glBlendFuncSeparate(sfactor, dfactor, sfactor, dfactor);
}

OGL_API void glBlendEquation(GLenum mode) {
  Context* c = current;
  if (!c) return;
  switch (mode) {
    case GL_FUNC_ADD: c->blend_eq = VK_BLEND_OP_ADD; break;
    case GL_FUNC_SUBTRACT: c->blend_eq = VK_BLEND_OP_SUBTRACT; break;
    case GL_FUNC_REVERSE_SUBTRACT: c->blend_eq = VK_BLEND_OP_REVERSE_SUBTRACT; break;
    case GL_MIN: c->blend_eq = VK_BLEND_OP_MIN; break;
    case GL_MAX: c->blend_eq = VK_BLEND_OP_MAX; break;
    default: gl_error(c, GL_INVALID_ENUM);
  }
}

OGL_API void glCullFace(GLenum mode) {
  Context* c = current;
  if (!c) return;
  switch (mode) {
    case GL_FRONT: c->cull_face = VK_CULL_MODE_FRONT_BIT; break;
    case GL_BACK: c->cull_face = VK_CULL_MODE_BACK_BIT; break;
    case GL_FRONT_AND_BACK: c->cull_face = VK_CULL_MODE_FRONT_AND_BACK; break;
    default: gl_error(c, GL_INVALID_ENUM);
  }
}

OGL_API void glLogicOp(GLenum opcode) {
  Context* c = current;
  if (!c) return;
  if (opcode < GL_CLEAR || opcode > GL_SET) return gl_error(c, GL_INVALID_ENUM);
  c->logic_op = static_cast<VkLogicOp>(opcode - GL_CLEAR);
}

OGL_API void glLineWidth(GLfloat width) {
  Context* c = current;
  if (!c) return;
  if (width <= 0.0f) return gl_error(c, GL_INVALID_VALUE);
  c->line_width = width;
}

OGL_API void glPolygonOffset(GLfloat factor, GLfloat units) {
  Context* c = current;
  if (!c) return;
  c->offset_factor = factor;
  c->offset_units = units;
}

// shortcut: only GL_FILL is rasterized (MC only sets FILL); LINE/POINT are accepted and ignored.
OGL_API void glPolygonMode(GLenum face, GLenum mode) {
  Context* c = current;
  if (!c) return;
  if ((face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) ||
      (mode != GL_FILL && mode != GL_LINE && mode != GL_POINT))
    gl_error(c, GL_INVALID_ENUM);
}

// shortcut: GL_FLAT renders smooth (needs a flat-varying shader variant and last-vertex provoking).
OGL_API void glShadeModel(GLenum mode) {
  Context* c = current;
  if (c && mode != GL_FLAT && mode != GL_SMOOTH) gl_error(c, GL_INVALID_ENUM);
}

OGL_API void glFogf(GLenum pname, GLfloat param) {
  if (Context* c = current) fog(c, pname, &param);
}

OGL_API void glFogi(GLenum pname, GLint param) {
  const GLfloat f = static_cast<GLfloat>(param);
  if (Context* c = current) fog(c, pname, &f);
}

OGL_API void glFogfv(GLenum pname, const GLfloat *params) {
  if (Context* c = current; c && params) fog(c, pname, params);
}

OGL_API void glLightfv(GLenum light, GLenum pname, const GLfloat *params) {
  Context* c = current;
  if (!c || !params) return;
  const GLenum i = light - GL_LIGHT0;
  if (i > 1) return gl_error(c, GL_INVALID_ENUM);  // shortcut: lights 0-1 (MC's RenderHelper)
  switch (pname) {
    case GL_POSITION: {  // stored in eye space, transformed by the modelview current at this call
      const Mat4& m = top(c->modelview);
      for (int r = 0; r < 4; ++r)
        c->light_pos[i][r] = m.m[r] * params[0] + m.m[4 + r] * params[1] + m.m[8 + r] * params[2] + m.m[12 + r] * params[3];
      break;
    }
    case GL_DIFFUSE: std::memcpy(c->light_diffuse[i], params, 4 * sizeof(GLfloat)); break;
    case GL_AMBIENT: std::memcpy(c->light_ambient[i], params, 4 * sizeof(GLfloat)); break;
    case GL_SPECULAR: break;  // shortcut: no specular term (MC sets it to zero)
    default: return gl_error(c, GL_INVALID_ENUM);
  }
  c->ff_dirty = true;
}

OGL_API void glLightModelfv(GLenum pname, const GLfloat *params) {
  Context* c = current;
  if (!c || !params) return;
  if (pname != GL_LIGHT_MODEL_AMBIENT) return gl_error(c, GL_INVALID_ENUM);
  std::memcpy(c->scene_ambient, params, 4 * sizeof(GLfloat));
  c->ff_dirty = true;
}

OGL_API void glColorMaterial(GLenum face, GLenum mode) {
  Context* c = current;
  if (!c) return;
  if ((face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) ||
      (mode != GL_AMBIENT && mode != GL_DIFFUSE && mode != GL_AMBIENT_AND_DIFFUSE))
    return gl_error(c, GL_INVALID_ENUM);  // shortcut: SPECULAR/EMISSION tracking unsupported
  c->color_material = mode;
  c->ff_dirty = true;
}

OGL_API void glTexEnvi(GLenum target, GLenum pname, GLint param) {
  const GLfloat f = static_cast<GLfloat>(param);
  if (Context* c = current) tex_env(c, target, pname, &f, param);
}

OGL_API void glTexEnvf(GLenum target, GLenum pname, GLfloat param) {
  if (Context* c = current) tex_env(c, target, pname, &param, static_cast<GLint>(param));
}

OGL_API void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params) {
  if (Context* c = current; c && params) tex_env(c, target, pname, params, static_cast<GLint>(params[0]));
}

// shortcut: texgen is evaluated for texture unit 0 only (MC uses it there, for the end portal).
OGL_API void glTexGeni(GLenum coord, GLenum pname, GLint param) {
  Context* c = current;
  if (!c) return;
  const GLenum i = coord - GL_S, mode = static_cast<GLenum>(param);
  if (i > 3 || pname != GL_TEXTURE_GEN_MODE || (mode != GL_OBJECT_LINEAR && mode != GL_EYE_LINEAR))
    return gl_error(c, GL_INVALID_ENUM);
  c->units[c->active].gen_mode[i] = mode;
  c->ff_dirty = true;
}

OGL_API void glTexGenfv(GLenum coord, GLenum pname, const GLfloat *params) {
  Context* c = current;
  if (!c || !params) return;
  const GLenum i = coord - GL_S;
  if (i > 3) return gl_error(c, GL_INVALID_ENUM);
  TexUnit& u = c->units[c->active];
  if (pname == GL_OBJECT_PLANE) {
    std::memcpy(u.gen_object[i], params, 4 * sizeof(GLfloat));
  } else if (pname == GL_EYE_PLANE) {  // GL: eye plane = p * inverse(modelview at this call)
    Mat4 inv;
    if (!mat_invert(top(c->modelview), inv)) inv = mat_identity();
    for (int j = 0; j < 4; ++j)
      u.gen_eye[i][j] = params[0] * inv.m[j * 4] + params[1] * inv.m[j * 4 + 1] + params[2] * inv.m[j * 4 + 2] +
                        params[3] * inv.m[j * 4 + 3];
  } else {
    return gl_error(c, GL_INVALID_ENUM);
  }
  c->ff_dirty = true;
}
