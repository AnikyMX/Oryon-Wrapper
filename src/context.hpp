// GL context (state + Vulkan command recording) and EGL surface (default framebuffer + swapchain).
#pragma once
#include "device.hpp"
#include "oryon.hpp"
#include <EGL/egl.h>

namespace oryon {

constexpr int kUnits = 2;  // MC uses unit 0 (textures) and unit 1 (lightmap)
enum Attr : int { A_POS, A_COLOR, A_TEX0, A_TEX1, A_NORMAL, A_COUNT };

// glEnable caps tracked as bits (per-unit caps GL_TEXTURE_2D / GL_TEXTURE_GEN_* live in TexUnit).
inline constexpr GLenum kCaps[] = {GL_ALPHA_TEST,   GL_BLEND,          GL_COLOR_LOGIC_OP,      GL_COLOR_MATERIAL,
                                   GL_CULL_FACE,    GL_DEPTH_TEST,     GL_DITHER,              GL_FOG,
                                   GL_LIGHTING,     GL_LIGHT0,         GL_LIGHT1,              GL_LINE_SMOOTH,
                                   GL_MULTISAMPLE,  GL_NORMALIZE,      GL_POLYGON_OFFSET_FILL, GL_POLYGON_OFFSET_LINE,
                                   GL_RESCALE_NORMAL, GL_SCISSOR_TEST, GL_STENCIL_TEST};
constexpr int cap_bit(GLenum cap) {
  for (int i = 0; i < static_cast<int>(sizeof kCaps / sizeof kCaps[0]); ++i)
    if (kCaps[i] == cap) return i;
  return -1;
}
constexpr uint64_t cap_mask(GLenum cap) { return uint64_t(1) << cap_bit(cap); }

struct Mat4 {
  float m[16];  // column-major, as GL
};
struct MatrixStack {
  Mat4 s[32];
  int top = 0;
};

struct Block;  // suballocation block (gl_draw.cpp)
struct Range {
  Block* block = nullptr;
  VkDeviceSize offset = 0, size = 0;
};

struct Texture {
  Image image;
  uint32_t levels = 0;
  GLenum min_filter = GL_NEAREST_MIPMAP_LINEAR, mag_filter = GL_LINEAR, wrap_s = GL_REPEAT, wrap_t = GL_REPEAT;
  GLint max_level = 1000;
  GLfloat min_lod = -1000.0f, max_lod = 1000.0f, lod_bias = 0.0f;
  VkSampler sampler = VK_NULL_HANDLE;  // for the current parameters
};

struct GLBuffer {
  Range range;
};

struct Array {  // gl*Pointer + client state
  bool enabled = false;
  GLint size = 4;
  GLenum type = GL_FLOAT;
  GLsizei stride = 0;
  const uint8_t* pointer = nullptr;  // client address, or offset into `buffer`
  GLBuffer* buffer = nullptr;
};

struct TexUnit {
  GLuint name = 0;
  Texture* texture = nullptr;
  bool enabled = false;
  MatrixStack matrix;
  int32_t mode = 0;          // shader env mode (0 MODULATE .. 5 COMBINE)
  int32_t combine[2]{};      // rgb, alpha: function | src<<4.. | operand<<16..
  GLfloat env_color[4]{};
  GLfloat scale[2] = {1.0f, 1.0f};
  uint8_t gen_on = 0;        // GL_TEXTURE_GEN_S..Q bits
  GLenum gen_mode[4]{};
  GLfloat gen_object[4][4]{}, gen_eye[4][4]{};
};

// std140 mirror of the FF uniform block in shaders/ff.vert and shaders/ff.frag.
struct FFUniforms {
  float mvp[16], modelview[16], texmat[2][16];
  float texgen[4][4];
  float light_pos[2][4], light_diffuse[2][4], light_ambient[2][4];
  float scene_ambient[4], fog_color[4], fog[4];
  float env_color[2][4], env_scale[2][4];
  int32_t env[2][4];
  int32_t flags[4];
  float alpha_ref[4];
};

// Default framebuffer images keep GL row order (row 0 = bottom); the present blit flips Y.
struct Surface {
  bool window = false;
  void* native = nullptr;
  Image color, depth;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkExtent2D extent{};
  std::vector<VkImage> images;
  std::vector<VkSemaphore> rendered;           // per swapchain image, waited by present
  VkSemaphore acquired[2]{};                    // per presenting context frame slot
  const void* presenter = nullptr;              // context owning `acquired`
  int interval = 1;
  bool stale = false;  // swapchain must be recreated before the next acquire
  int bound = 0;       // contexts using it as draw/read
  bool doomed = false; // eglDestroySurface while bound
};

struct Context {
  struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool busy = false;
    std::vector<Buffer> ring;  // transient bytes (vertices, uniforms, staging), reset per recording
    size_t chunk = 0;
    VkDeviceSize used = 0;
    std::vector<VkDescriptorPool> dpools;
    size_t dpool = 0;
    std::vector<Image> dead_images;  // released once this slot's fence has signaled
    std::vector<Range> dead_ranges;
  } frames[2];
  uint32_t frame = 0;
  VkCommandBuffer cmd = VK_NULL_HANDLE;  // recording, or null
  Surface* draw = nullptr;
  Surface* read = nullptr;
  bool in_pass = false;
  uint32_t pending = 0;           // CLEAR_* bits folded into the next render pass load ops
  VkClearValue pending_values[2]{};
  Buffer readback;
  bool bound = false, doomed = false, fresh = true;
  EGLint client_version = 1;
  EGLenum api = EGL_OPENGL_ES_API;

  GLenum error = GL_NO_ERROR;
  GLfloat clear_color[4]{};
  GLfloat clear_depth = 1.0f;
  GLint clear_stencil = 0;
  GLint viewport[4]{};
  GLint scissor[4]{};
  uint64_t caps = 0;
  GLenum extra_caps[16]{};
  uint32_t extra_count = 0;
  char renderer[320]{};
  char version[48]{};

  // matrices
  GLenum matrix_mode = GL_MODELVIEW;
  MatrixStack modelview, projection;
  // textures / arrays / current attributes
  TexUnit units[kUnits];
  int active = 0, client_active = 0;
  Array arrays[A_COUNT];
  GLfloat current[A_COUNT][4]{};
  GLBuffer* array_buffer = nullptr;
  GLint unpack_alignment = 4, pack_alignment = 4;
  // raster state
  VkBlendFactor blend_src[2] = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE};   // rgb, alpha
  VkBlendFactor blend_dst[2] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO};
  VkBlendOp blend_eq = VK_BLEND_OP_ADD;
  GLboolean color_mask[4] = {1, 1, 1, 1}, depth_mask = 1;
  VkCompareOp depth_func = VK_COMPARE_OP_LESS, alpha_func = VK_COMPARE_OP_ALWAYS;
  VkCullModeFlags cull_face = VK_CULL_MODE_BACK_BIT;
  VkLogicOp logic_op = VK_LOGIC_OP_COPY;
  GLfloat alpha_ref = 0.0f, line_width = 1.0f, offset_factor = 0.0f, offset_units = 0.0f;
  // fixed-function lighting / fog
  GLenum color_material = GL_AMBIENT_AND_DIFFUSE, fog_mode = GL_EXP;
  GLfloat fog_density = 1.0f, fog_start = 0.0f, fog_end = 1.0f, fog_color[4]{};
  GLfloat light_pos[2][4]{}, light_diffuse[2][4]{}, light_ambient[2][4]{}, scene_ambient[4]{};
  // glBegin/glEnd
  bool in_begin = false;
  GLenum begin_mode = 0;
  std::vector<float> immediate;
  // bindings recorded in the current command buffer (reset by ctx_cmd)
  bool ff_dirty = true, tex_dirty = true;
  VkBuffer ubo_buffer = VK_NULL_HANDLE;
  uint32_t ubo_offset = 0;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
};

struct Alloc {
  VkBuffer buffer;
  VkDeviceSize offset;
  uint8_t* ptr;
};

extern thread_local constinit Context* current;  // constinit: direct TLS access, no wrapper call

inline void gl_error(Context* c, GLenum e) {
  if (c->error == GL_NO_ERROR) c->error = e;
}

void gl_defaults(Context& c);  // GL initial state (gl_core.cpp)
Context* ctx_create(EGLint client_version, EGLenum api);
void ctx_destroy(Context* c);
VkCommandBuffer ctx_cmd(Context& c);
Alloc ctx_alloc(Context& c, VkDeviceSize size, VkDeviceSize align);  // transient, valid for this recording
VkDescriptorSet ctx_descriptor_set(Context& c);
void ctx_discard(Context& c, Image& im);  // destroy once in-flight work is done
void ctx_discard(Context& c, Range& r);
void buf_free(const Range& r);            // gl_draw.cpp
VkDescriptorImageInfo tex_descriptor(Context& c, int unit);  // gl_texture.cpp; white texture when none
inline bool tex_ready(const TexUnit& u) { return u.texture && u.texture->image.image; }
inline Mat4& top(MatrixStack& m) { return m.s[m.top]; }
inline const Mat4& top(const MatrixStack& m) { return m.s[m.top]; }
Mat4 mat_identity();
Mat4 mat_mul(const Mat4& a, const Mat4& b);  // a * b
bool mat_invert(const Mat4& m, Mat4& out);
void ctx_begin_pass(Context& c);
void ctx_end_pass(Context& c);
void ctx_submit(Context& c, VkSemaphore wait, VkPipelineStageFlags stage, VkSemaphore signal, bool block);
void ctx_bind(Context& c, Surface* draw, Surface* read);
EGLint ctx_swap(Context& c, Surface& s);

Surface* surf_create(bool window, void* native, uint32_t w, uint32_t h);
void surf_destroy(Surface* s);
bool surf_swapchain(Surface& s);

}  // namespace oryon
