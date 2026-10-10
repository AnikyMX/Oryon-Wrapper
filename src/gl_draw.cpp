// Vertex input (client arrays, VBOs, glBegin/glEnd), fixed-function uniforms, pipeline cache and draws.
#include "context.hpp"
#include "gl_api.hpp"
#include <algorithm>
#include <cstring>
#include <map>
#include <type_traits>
#include <unordered_map>

using namespace oryon;

// ---- buffer suballocation: few large host-visible blocks instead of one allocation per GL buffer

struct oryon::Block {
  Buffer buffer;
  std::map<VkDeviceSize, VkDeviceSize> free;  // offset -> size
};

namespace {

constexpr VkDeviceSize kBlockSize = VkDeviceSize(32) << 20;
std::vector<Block*> blocks;

// shortcut: buffers live in host-visible memory and are written directly (ideal on UMA / Adreno, slower on dGPUs).
Range buf_alloc(VkDeviceSize size) {
  size = (size + 255) & ~VkDeviceSize(255);
  for (Block* b : blocks)
    for (auto it = b->free.begin(); it != b->free.end(); ++it)
      if (it->second >= size) {
        const Range r{b, it->first, size};
        const VkDeviceSize rest = it->second - size, at = it->first + size;
        b->free.erase(it);
        if (rest) b->free.emplace(at, rest);
        return r;
      }
  Block* b = new Block();
  if (!dev_host_buffer(b->buffer, std::max(kBlockSize, size),
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
    delete b;
    return {};
  }
  if (b->buffer.size > size) b->free.emplace(size, b->buffer.size - size);
  blocks.push_back(b);
  return {b, 0, size};
}

std::unordered_map<GLuint, GLBuffer*> buffers;  // shortcut: one global share group, GL thread only
GLuint next_buffer = 1;

}  // namespace

void oryon::buf_free(const Range& r) {
  auto& f = r.block->free;
  auto it = f.emplace(r.offset, r.size).first;
  if (auto next = std::next(it); next != f.end() && it->first + it->second == next->first) {
    it->second += next->second;
    f.erase(next);
  }
  if (it != f.begin())
    if (auto prev = std::prev(it); prev->first + prev->second == it->first) {
      prev->second += it->second;
      f.erase(it);
    }
}

OGL_API void glGenBuffers(GLsizei n, GLuint *buffers_) {
  Context* c = current;
  if (!c || !buffers_) return;
  if (n < 0) return gl_error(c, GL_INVALID_VALUE);
  for (GLsizei i = 0; i < n; ++i) buffers_[i] = next_buffer++;
}

OGL_API void glDeleteBuffers(GLsizei n, const GLuint *buffers_) {
  Context* c = current;
  if (!c || !buffers_) return;
  if (n < 0) return gl_error(c, GL_INVALID_VALUE);
  for (GLsizei i = 0; i < n; ++i) {
    auto it = buffers.find(buffers_[i]);
    if (it == buffers.end()) continue;
    GLBuffer* b = it->second;
    if (c->array_buffer == b) c->array_buffer = nullptr;
    for (Array& a : c->arrays)
      if (a.buffer == b) a = Array{};
    ctx_discard(*c, b->range);
    delete b;
    buffers.erase(it);
  }
}

// shortcut: GL_ELEMENT_ARRAY_BUFFER is accepted but unused (MC 1.12.2 never calls glDrawElements).
OGL_API void glBindBuffer(GLenum target, GLuint buffer) {
  Context* c = current;
  if (!c) return;
  if (target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER) return gl_error(c, GL_INVALID_ENUM);
  if (target != GL_ARRAY_BUFFER) return;
  if (!buffer) {
    c->array_buffer = nullptr;
    return;
  }
  GLBuffer*& b = buffers[buffer];
  if (!b) b = new GLBuffer();
  c->array_buffer = b;
}

// glBufferData orphans: in-flight draws keep the old range, new data goes to a fresh one.
OGL_API void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
  (void)usage;
  Context* c = current;
  if (!c) return;
  if (target != GL_ARRAY_BUFFER) return gl_error(c, target == GL_ELEMENT_ARRAY_BUFFER ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
  if (size < 0) return gl_error(c, GL_INVALID_VALUE);
  GLBuffer* b = c->array_buffer;
  if (!b) return gl_error(c, GL_INVALID_OPERATION);
  ctx_discard(*c, b->range);
  if (!size) return;
  b->range = buf_alloc(static_cast<VkDeviceSize>(size));
  if (!b->range.block) return gl_error(c, GL_OUT_OF_MEMORY);
  if (data) std::memcpy(static_cast<uint8_t*>(b->range.block->buffer.map) + b->range.offset, data, size_t(size));
}

OGL_API void glGenBuffersARB(GLsizei n, GLuint *buffers_) { glGenBuffers(n, buffers_); }
OGL_API void glDeleteBuffersARB(GLsizei n, const GLuint *buffers_) { glDeleteBuffers(n, buffers_); }
OGL_API void glBindBufferARB(GLenum target, GLuint buffer) { glBindBuffer(target, buffer); }
OGL_API void glBufferDataARB(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
  glBufferData(target, size, data, usage);
}

// ---- client arrays

namespace {

uint32_t type_size(GLenum type) {
  switch (type) {
    case GL_FLOAT: return 4;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT: return 2;
    case GL_BYTE:
    case GL_UNSIGNED_BYTE: return 1;
    default: return 0;
  }
}

// shortcut: 1/3-component byte/short arrays fetch a 2/4-component element (MC pads them); GL_INT/GL_DOUBLE unsupported.
VkFormat attr_format(int attr, GLenum type, GLint size) {
  const bool norm = attr == A_COLOR || attr == A_NORMAL, wide = size > 2;
  switch (type) {
    case GL_FLOAT: {
      constexpr VkFormat f[4] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
                                 VK_FORMAT_R32G32B32A32_SFLOAT};
      return f[size - 1];
    }
    case GL_UNSIGNED_BYTE:
      return wide ? (norm ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_USCALED)
                  : (norm ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8G8_USCALED);
    case GL_BYTE:
      return wide ? (norm ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_SSCALED)
                  : (norm ? VK_FORMAT_R8G8_SNORM : VK_FORMAT_R8G8_SSCALED);
    case GL_SHORT:
      return wide ? (norm ? VK_FORMAT_R16G16B16A16_SNORM : VK_FORMAT_R16G16B16A16_SSCALED)
                  : (norm ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_SSCALED);
    case GL_UNSIGNED_SHORT:
      return wide ? (norm ? VK_FORMAT_R16G16B16A16_UNORM : VK_FORMAT_R16G16B16A16_USCALED)
                  : (norm ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R16G16_USCALED);
    default: return VK_FORMAT_UNDEFINED;
  }
}

void pointer(int attr, GLint size, GLenum type, GLsizei stride, const void* ptr, GLint min_size) {
  Context* c = current;
  if (!c) return;
  if (size < min_size || size > 4 || stride < 0) return gl_error(c, GL_INVALID_VALUE);
  if (attr_format(attr, type, size) == VK_FORMAT_UNDEFINED) return gl_error(c, GL_INVALID_ENUM);
  Array& a = c->arrays[attr];
  a.size = size;
  a.type = type;
  a.stride = stride;
  a.pointer = static_cast<const uint8_t*>(ptr);
  a.buffer = c->array_buffer;
}

int client_array(Context* c, GLenum array) {
  switch (array) {
    case GL_VERTEX_ARRAY: return A_POS;
    case GL_COLOR_ARRAY: return A_COLOR;
    case GL_NORMAL_ARRAY: return A_NORMAL;
    case GL_TEXTURE_COORD_ARRAY: return A_TEX0 + c->client_active;
    default: return -1;
  }
}

void client_state(GLenum array, bool on) {
  Context* c = current;
  if (!c) return;
  const int a = client_array(c, array);
  if (a < 0) return gl_error(c, GL_INVALID_ENUM);
  c->arrays[a].enabled = on;
}

}  // namespace

OGL_API void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *pointer_) {
  pointer(A_POS, size, type, stride, pointer_, 2);
}
OGL_API void glColorPointer(GLint size, GLenum type, GLsizei stride, const void *pointer_) {
  pointer(A_COLOR, size, type, stride, pointer_, 3);
}
OGL_API void glNormalPointer(GLenum type, GLsizei stride, const void *pointer_) {
  pointer(A_NORMAL, 3, type, stride, pointer_, 3);
}
OGL_API void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *pointer_) {
  if (Context* c = current) pointer(A_TEX0 + c->client_active, size, type, stride, pointer_, 1);
}
OGL_API void glEnableClientState(GLenum array) { client_state(array, true); }
OGL_API void glDisableClientState(GLenum array) { client_state(array, false); }

OGL_API void glClientActiveTexture(GLenum texture) {
  Context* c = current;
  if (!c) return;
  if (texture - GL_TEXTURE0 >= static_cast<GLenum>(kUnits)) return gl_error(c, GL_INVALID_ENUM);
  c->client_active = static_cast<int>(texture - GL_TEXTURE0);
}
OGL_API void glClientActiveTextureARB(GLenum texture) { glClientActiveTexture(texture); }

// ---- current attributes and glBegin/glEnd (vertices collected, then drawn through the array path)

OGL_API void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
  if (Context* c = current) {
    GLfloat* v = c->current[A_COLOR];
    v[0] = red, v[1] = green, v[2] = blue, v[3] = alpha;
  }
}

OGL_API void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz) {
  if (Context* c = current) {
    GLfloat* v = c->current[A_NORMAL];
    v[0] = nx, v[1] = ny, v[2] = nz;
  }
}

OGL_API void glMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t) {
  Context* c = current;
  if (!c) return;
  if (target - GL_TEXTURE0 >= static_cast<GLenum>(kUnits)) return gl_error(c, GL_INVALID_ENUM);
  GLfloat* v = c->current[A_TEX0 + (target - GL_TEXTURE0)];
  v[0] = s, v[1] = t, v[2] = 0.0f, v[3] = 1.0f;
}
OGL_API void glMultiTexCoord2fARB(GLenum target, GLfloat s, GLfloat t) { glMultiTexCoord2f(target, s, t); }
OGL_API void glTexCoord2f(GLfloat s, GLfloat t) { glMultiTexCoord2f(GL_TEXTURE0, s, t); }

namespace {
constexpr int kImmFloats = 18;  // position 3, color 4, tex0 4, tex1 4, normal 3
void draw(Context& c, GLenum mode, GLint first, GLsizei count);
}  // namespace

OGL_API void glBegin(GLenum mode) {
  Context* c = current;
  if (!c) return;
  if (c->in_begin) return gl_error(c, GL_INVALID_OPERATION);
  if (mode > GL_POLYGON) return gl_error(c, GL_INVALID_ENUM);
  c->in_begin = true;
  c->begin_mode = mode;
  c->immediate.clear();
}

OGL_API void glVertex3f(GLfloat x, GLfloat y, GLfloat z) {
  Context* c = current;
  if (!c || !c->in_begin) return;
  const GLfloat* a = c->current[0];
  c->immediate.insert(c->immediate.end(), {x, y, z});
  c->immediate.insert(c->immediate.end(), a + 4 * A_COLOR, a + 4 * A_COLOR + 12);  // color, tex0, tex1
  c->immediate.insert(c->immediate.end(), a + 4 * A_NORMAL, a + 4 * A_NORMAL + 3);
}

OGL_API void glEnd(void) {
  Context* c = current;
  if (!c) return;
  if (!c->in_begin) return gl_error(c, GL_INVALID_OPERATION);
  c->in_begin = false;
  Array saved[A_COUNT];
  std::copy(c->arrays, c->arrays + A_COUNT, saved);
  static constexpr GLint offs[A_COUNT] = {0, 3, 7, 11, 15}, sizes[A_COUNT] = {3, 4, 4, 4, 3};
  for (int i = 0; i < A_COUNT; ++i)
    c->arrays[i] = Array{true, sizes[i], GL_FLOAT, kImmFloats * 4,
                         reinterpret_cast<const uint8_t*>(c->immediate.data() + offs[i]), nullptr};
  draw(*c, c->begin_mode, 0, static_cast<GLsizei>(c->immediate.size() / kImmFloats));
  std::copy(saved, saved + A_COUNT, c->arrays);
}

// ---- draws

namespace {

struct PipeKey {
  VkRenderPass pass;
  uint32_t topology;
  uint32_t format[A_COUNT];  // VK_FORMAT_UNDEFINED: constant attribute from the per-draw block
  uint32_t stride[A_COUNT];
  uint32_t blend, src[2], dst[2], op, mask;
  uint32_t depth_test, depth_write, depth_func;
  uint32_t cull, bias, logic, logic_op, pad;
  bool operator==(const PipeKey& o) const { return !std::memcmp(this, &o, sizeof *this); }
};
static_assert(std::has_unique_object_representations_v<PipeKey>, "PipeKey is hashed as raw bytes");

struct PipeHash {
  size_t operator()(const PipeKey& k) const {
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (const auto* p = reinterpret_cast<const uint8_t*>(&k); p != reinterpret_cast<const uint8_t*>(&k + 1); ++p)
      h = (h ^ *p) * 1099511628211ull;
    return static_cast<size_t>(h);
  }
};
std::unordered_map<PipeKey, VkPipeline, PipeHash> pipelines;

VkPipeline make_pipeline(const PipeKey& k) {
  const Device& d = dev();
  const VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .pNext = nullptr,
       .flags = 0,
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = d.vert,
       .pName = "main",
       .pSpecializationInfo = nullptr},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .pNext = nullptr,
       .flags = 0,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = d.frag,
       .pName = "main",
       .pSpecializationInfo = nullptr}};
  VkVertexInputBindingDescription bindings[A_COUNT + 1];
  VkVertexInputAttributeDescription attrs[A_COUNT];
  uint32_t nb = 0;
  for (uint32_t i = 0; i < A_COUNT; ++i) {
    if (k.format[i]) {
      bindings[nb++] = {i, k.stride[i], VK_VERTEX_INPUT_RATE_VERTEX};
      attrs[i] = {i, i, static_cast<VkFormat>(k.format[i]), 0};
    } else {
      attrs[i] = {i, A_COUNT, VK_FORMAT_R32G32B32A32_SFLOAT, 16 * i};
    }
  }
  bindings[nb++] = {A_COUNT, 16 * A_COUNT, VK_VERTEX_INPUT_RATE_INSTANCE};
  VkPipelineVertexInputStateCreateInfo vi{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                          .pNext = nullptr,
                                          .flags = 0,
                                          .vertexBindingDescriptionCount = nb,
                                          .pVertexBindingDescriptions = bindings,
                                          .vertexAttributeDescriptionCount = A_COUNT,
                                          .pVertexAttributeDescriptions = attrs};
  VkPipelineInputAssemblyStateCreateInfo ia{.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                            .pNext = nullptr,
                                            .flags = 0,
                                            .topology = static_cast<VkPrimitiveTopology>(k.topology),
                                            .primitiveRestartEnable = VK_FALSE};
  VkPipelineViewportStateCreateInfo vp{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                       .pNext = nullptr,
                                       .flags = 0,
                                       .viewportCount = 1,
                                       .pViewports = nullptr,
                                       .scissorCount = 1,
                                       .pScissors = nullptr};
  VkPipelineRasterizationStateCreateInfo rs{.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                            .pNext = nullptr,
                                            .flags = 0,
                                            .depthClampEnable = VK_FALSE,
                                            .rasterizerDiscardEnable = VK_FALSE,
                                            .polygonMode = VK_POLYGON_MODE_FILL,
                                            .cullMode = k.cull,
                                            .frontFace = VK_FRONT_FACE_CLOCKWISE,  // GL_CCW with GL-row-order framebuffers
                                            .depthBiasEnable = k.bias,
                                            .depthBiasConstantFactor = 0.0f,
                                            .depthBiasClamp = 0.0f,
                                            .depthBiasSlopeFactor = 0.0f,
                                            .lineWidth = 1.0f};
  VkPipelineMultisampleStateCreateInfo ms{.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                          .pNext = nullptr,
                                          .flags = 0,
                                          .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
                                          .sampleShadingEnable = VK_FALSE,
                                          .minSampleShading = 0.0f,
                                          .pSampleMask = nullptr,
                                          .alphaToCoverageEnable = VK_FALSE,
                                          .alphaToOneEnable = VK_FALSE};
  VkPipelineDepthStencilStateCreateInfo ds{.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                           .pNext = nullptr,
                                           .flags = 0,
                                           .depthTestEnable = k.depth_test,
                                           .depthWriteEnable = k.depth_write,
                                           .depthCompareOp = static_cast<VkCompareOp>(k.depth_func),
                                           .depthBoundsTestEnable = VK_FALSE,
                                           .stencilTestEnable = VK_FALSE,
                                           .front = {},
                                           .back = {},
                                           .minDepthBounds = 0.0f,
                                           .maxDepthBounds = 1.0f};
  VkPipelineColorBlendAttachmentState att{.blendEnable = k.blend,
                                          .srcColorBlendFactor = static_cast<VkBlendFactor>(k.src[0]),
                                          .dstColorBlendFactor = static_cast<VkBlendFactor>(k.dst[0]),
                                          .colorBlendOp = static_cast<VkBlendOp>(k.op),
                                          .srcAlphaBlendFactor = static_cast<VkBlendFactor>(k.src[1]),
                                          .dstAlphaBlendFactor = static_cast<VkBlendFactor>(k.dst[1]),
                                          .alphaBlendOp = static_cast<VkBlendOp>(k.op),
                                          .colorWriteMask = k.mask};
  VkPipelineColorBlendStateCreateInfo cb{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                         .pNext = nullptr,
                                         .flags = 0,
                                         .logicOpEnable = k.logic,
                                         .logicOp = static_cast<VkLogicOp>(k.logic_op),
                                         .attachmentCount = 1,
                                         .pAttachments = &att,
                                         .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f}};
  const VkDynamicState dyn[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH,
                                 VK_DYNAMIC_STATE_DEPTH_BIAS};
  VkPipelineDynamicStateCreateInfo dy{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                      .pNext = nullptr,
                                      .flags = 0,
                                      .dynamicStateCount = 4,
                                      .pDynamicStates = dyn};
  VkGraphicsPipelineCreateInfo ci{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                  .pNext = nullptr,
                                  .flags = 0,
                                  .stageCount = 2,
                                  .pStages = stages,
                                  .pVertexInputState = &vi,
                                  .pInputAssemblyState = &ia,
                                  .pTessellationState = nullptr,
                                  .pViewportState = &vp,
                                  .pRasterizationState = &rs,
                                  .pMultisampleState = &ms,
                                  .pDepthStencilState = &ds,
                                  .pColorBlendState = &cb,
                                  .pDynamicState = &dy,
                                  .layout = d.layout,
                                  .renderPass = k.pass,
                                  .subpass = 0,
                                  .basePipelineHandle = VK_NULL_HANDLE,
                                  .basePipelineIndex = -1};
  VkPipeline p = VK_NULL_HANDLE;
  if (vkCreateGraphicsPipelines(d.dev, d.cache, 1, &ci, nullptr, &p) != VK_SUCCESS)
    ORYON_LOG("vkCreateGraphicsPipelines failed");
  return p;
}

bool on(const Context& c, GLenum cap) { return c.caps & cap_mask(cap); }

void build_uniforms(const Context& c, FFUniforms& u) {
  const Mat4& mv = top(c.modelview);
  const Mat4 mvp = mat_mul(top(c.projection), mv);
  std::memcpy(u.mvp, mvp.m, sizeof u.mvp);
  std::memcpy(u.modelview, mv.m, sizeof u.modelview);
  int32_t flags = 0, gen = 0;
  for (int i = 0; i < kUnits; ++i) {
    const TexUnit& t = c.units[i];
    std::memcpy(u.texmat[i], top(t.matrix).m, sizeof u.texmat[i]);
    std::memcpy(u.env_color[i], t.env_color, sizeof u.env_color[i]);
    u.env_scale[i][0] = t.scale[0];
    u.env_scale[i][1] = t.scale[1];
    u.env[i][0] = t.mode;
    u.env[i][1] = t.combine[0];
    u.env[i][2] = t.combine[1];
    if (t.enabled && tex_ready(t)) flags |= 32 << i;
  }
  const TexUnit& t0 = c.units[0];
  for (int i = 0; i < 4; ++i)
    if (t0.gen_on >> i & 1) {
      const bool eye = t0.gen_mode[i] == GL_EYE_LINEAR;
      gen |= (eye ? 2 : 1) << (2 * i);
      std::memcpy(u.texgen[i], eye ? t0.gen_eye[i] : t0.gen_object[i], sizeof u.texgen[i]);
    }
  std::memcpy(u.light_pos, c.light_pos, sizeof u.light_pos);
  std::memcpy(u.light_diffuse, c.light_diffuse, sizeof u.light_diffuse);
  std::memcpy(u.light_ambient, c.light_ambient, sizeof u.light_ambient);
  std::memcpy(u.scene_ambient, c.scene_ambient, sizeof u.scene_ambient);
  std::memcpy(u.fog_color, c.fog_color, sizeof u.fog_color);
  u.fog[0] = c.fog_start;
  u.fog[1] = c.fog_end;
  u.fog[2] = c.fog_density;
  u.fog[3] = c.fog_end != c.fog_start ? 1.0f / (c.fog_end - c.fog_start) : 0.0f;
  if (on(c, GL_LIGHTING)) {
    flags |= 1 | (on(c, GL_LIGHT0) ? 2 : 0) | (on(c, GL_LIGHT1) ? 4 : 0);
    if (on(c, GL_COLOR_MATERIAL))
      flags |= (c.color_material != GL_DIFFUSE ? 8 : 0) | (c.color_material != GL_AMBIENT ? 16 : 0);
  }
  u.flags[0] = flags;
  u.flags[1] = on(c, GL_ALPHA_TEST) ? c.alpha_func : VK_COMPARE_OP_ALWAYS;
  u.flags[2] = !on(c, GL_FOG) ? 0 : c.fog_mode == GL_LINEAR ? 1 : c.fog_mode == GL_EXP ? 2 : 3;
  u.flags[3] = gen;
  u.alpha_ref[0] = c.alpha_ref;
}

void draw(Context& c, GLenum mode, GLint first, GLsizei count) {
  Surface* s = c.draw;
  if (!s || count <= 0 || first < 0 || !c.arrays[A_POS].enabled) return;  // GL draws nothing without positions
  VkPrimitiveTopology topology;
  bool quads = false;
  switch (mode) {
    case GL_POINTS: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case GL_LINES: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case GL_LINE_LOOP:  // shortcut: the closing segment is not drawn (MC never uses line loops)
    case GL_LINE_STRIP: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case GL_TRIANGLES: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case GL_TRIANGLE_STRIP:
    case GL_QUAD_STRIP: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case GL_TRIANGLE_FAN:
    case GL_POLYGON: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case GL_QUADS:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      quads = true;
      count -= count % 4;
      break;
    default: return gl_error(&c, GL_INVALID_ENUM);
  }
  if (!count) return;
  const int32_t w = static_cast<int32_t>(s->color.width), h = static_cast<int32_t>(s->color.height);
  VkRect2D scissor{{0, 0}, {s->color.width, s->color.height}};
  if (on(c, GL_SCISSOR_TEST)) {
    const int32_t x0 = std::clamp(c.scissor[0], 0, w), y0 = std::clamp(c.scissor[1], 0, h);
    scissor = {{x0, y0},
               {static_cast<uint32_t>(std::clamp(c.scissor[0] + c.scissor[2], x0, w) - x0),
                static_cast<uint32_t>(std::clamp(c.scissor[1] + c.scissor[3], y0, h) - y0)}};
  }
  if (!c.viewport[2] || !c.viewport[3] || !scissor.extent.width || !scissor.extent.height) return;

  const Device& d = dev();
  PipeKey k{};
  k.pass = dev_pass(s->color.format, s->depth.format, 0);
  k.topology = topology;
  VkBuffer bufs[A_COUNT + 1];
  VkDeviceSize offs[A_COUNT + 1];

  // Vertex sources: VBO ranges are bound in place; client arrays are copied (an interleaved span once).
  const uint8_t* base = nullptr;
  uint32_t span_stride = 0;
  bool interleaved = true;
  for (int i = 0; i < A_COUNT; ++i) {
    const Array& a = c.arrays[i];
    if (!a.enabled) continue;
    const uint32_t elem = static_cast<uint32_t>(a.size) * type_size(a.type);
    const uint32_t stride = a.stride ? static_cast<uint32_t>(a.stride) : elem;
    k.format[i] = attr_format(i, a.type, a.size);
    k.stride[i] = stride;
    if (a.buffer) {
      if (!a.buffer->range.block) return;
      bufs[i] = a.buffer->range.block->buffer.buffer;
      offs[i] = a.buffer->range.offset + reinterpret_cast<uintptr_t>(a.pointer) + VkDeviceSize(first) * stride;
      continue;
    }
    if (!a.pointer) return;
    if (!base) base = a.pointer, span_stride = stride;
    interleaved &= stride == span_stride;
    base = std::min(base, a.pointer);
  }
  if (base) {
    uint32_t reach = 0;  // bytes of one vertex covered by all client arrays, from `base`
    for (int i = 0; i < A_COUNT; ++i) {
      const Array& a = c.arrays[i];
      if (a.enabled && !a.buffer)
        reach = std::max<uint32_t>(reach, static_cast<uint32_t>(a.pointer - base) + a.size * type_size(a.type));
    }
    interleaved &= reach <= span_stride;
    if (interleaved) {
      const VkDeviceSize bytes = VkDeviceSize(count - 1) * span_stride + reach;
      const Alloc m = ctx_alloc(c, bytes + 16, 16);  // +16: wider fetches of padded 3-component elements
      if (!m.ptr) return gl_error(&c, GL_OUT_OF_MEMORY);
      std::memcpy(m.ptr, base + size_t(first) * span_stride, size_t(bytes));
      for (int i = 0; i < A_COUNT; ++i)
        if (c.arrays[i].enabled && !c.arrays[i].buffer) {
          bufs[i] = m.buffer;
          offs[i] = m.offset + static_cast<VkDeviceSize>(c.arrays[i].pointer - base);
        }
    } else {
      for (int i = 0; i < A_COUNT; ++i) {
        const Array& a = c.arrays[i];
        if (!a.enabled || a.buffer) continue;
        const VkDeviceSize bytes = VkDeviceSize(count - 1) * k.stride[i] + a.size * type_size(a.type);
        const Alloc m = ctx_alloc(c, bytes + 16, 16);
        if (!m.ptr) return gl_error(&c, GL_OUT_OF_MEMORY);
        std::memcpy(m.ptr, a.pointer + size_t(first) * k.stride[i], size_t(bytes));
        bufs[i] = m.buffer;
        offs[i] = m.offset;
      }
    }
  }
  const Alloc constants = ctx_alloc(c, sizeof c.current, 16);
  if (!constants.ptr) return gl_error(&c, GL_OUT_OF_MEMORY);
  std::memcpy(constants.ptr, c.current, sizeof c.current);
  for (int i = 0; i < A_COUNT; ++i)
    if (!k.format[i]) bufs[i] = constants.buffer, offs[i] = constants.offset;  // binding unused, must be valid
  bufs[A_COUNT] = constants.buffer;
  offs[A_COUNT] = constants.offset;

  if (c.ff_dirty || !c.ubo_buffer) {
    const Alloc u = ctx_alloc(c, sizeof(FFUniforms), d.props.limits.minUniformBufferOffsetAlignment);
    if (!u.ptr) return gl_error(&c, GL_OUT_OF_MEMORY);
    build_uniforms(c, *reinterpret_cast<FFUniforms*>(u.ptr));
    if (u.buffer != c.ubo_buffer) c.set = VK_NULL_HANDLE;
    c.ubo_buffer = u.buffer;
    c.ubo_offset = static_cast<uint32_t>(u.offset);
    c.ff_dirty = false;
  }
  if (!c.set || c.tex_dirty) {
    c.set = ctx_descriptor_set(c);
    if (!c.set) return gl_error(&c, GL_OUT_OF_MEMORY);
    const VkDescriptorBufferInfo ubo{c.ubo_buffer, 0, sizeof(FFUniforms)};
    const VkDescriptorImageInfo tex[kUnits] = {tex_descriptor(c, 0), tex_descriptor(c, 1)};
    const VkWriteDescriptorSet writes[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .pNext = nullptr,
                                             .dstSet = c.set,
                                             .dstBinding = 0,
                                             .dstArrayElement = 0,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                             .pImageInfo = nullptr,
                                             .pBufferInfo = &ubo,
                                             .pTexelBufferView = nullptr},
                                            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .pNext = nullptr,
                                             .dstSet = c.set,
                                             .dstBinding = 1,
                                             .dstArrayElement = 0,
                                             .descriptorCount = kUnits,  // bindings 1-2, consecutive
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = tex,
                                             .pBufferInfo = nullptr,
                                             .pTexelBufferView = nullptr}};
    vkUpdateDescriptorSets(d.dev, 2, writes, 0, nullptr);
    c.tex_dirty = false;
  }

  const bool depth = s->depth.image && on(c, GL_DEPTH_TEST);
  const bool logic = d.features.logicOp && on(c, GL_COLOR_LOGIC_OP);
  k.blend = on(c, GL_BLEND) && !logic;
  k.src[0] = c.blend_src[0];
  k.src[1] = c.blend_src[1];
  k.dst[0] = c.blend_dst[0];
  k.dst[1] = c.blend_dst[1];
  k.op = c.blend_eq;
  k.mask = (c.color_mask[0] ? VK_COLOR_COMPONENT_R_BIT : 0) | (c.color_mask[1] ? VK_COLOR_COMPONENT_G_BIT : 0) |
           (c.color_mask[2] ? VK_COLOR_COMPONENT_B_BIT : 0) | (c.color_mask[3] ? VK_COLOR_COMPONENT_A_BIT : 0);
  k.depth_test = depth;
  k.depth_write = depth && c.depth_mask;
  k.depth_func = c.depth_func;
  k.cull = on(c, GL_CULL_FACE) ? c.cull_face : VK_CULL_MODE_NONE;
  k.bias = on(c, GL_POLYGON_OFFSET_FILL);
  k.logic = logic;
  k.logic_op = logic ? c.logic_op : 0;
  VkPipeline& pipe = pipelines[k];
  if (!pipe) pipe = make_pipeline(k);
  if (!pipe) return;

  ctx_begin_pass(c);
  VkCommandBuffer cmd = c.cmd;
  if (pipe != c.pipeline) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    c.pipeline = pipe;
  }
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, 0, 1, &c.set, 1, &c.ubo_offset);
  vkCmdBindVertexBuffers(cmd, 0, A_COUNT + 1, bufs, offs);
  const VkViewport viewport{static_cast<float>(c.viewport[0]), static_cast<float>(c.viewport[1]),
                            static_cast<float>(c.viewport[2]), static_cast<float>(c.viewport[3]), 0.0f, 1.0f};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  const float* range = d.props.limits.lineWidthRange;
  vkCmdSetLineWidth(cmd, d.features.wideLines ? std::clamp(c.line_width, range[0], range[1]) : 1.0f);
  vkCmdSetDepthBias(cmd, k.bias ? c.offset_units : 0.0f, 0.0f, k.bias ? c.offset_factor : 0.0f);
  if (quads) {
    vkCmdBindIndexBuffer(cmd, d.quads.buffer, 0, VK_INDEX_TYPE_UINT16);
    for (GLsizei b = 0; b < count; b += static_cast<GLsizei>(kQuadBatch))
      vkCmdDrawIndexed(cmd, static_cast<uint32_t>(std::min<GLsizei>(count - b, kQuadBatch)) / 4 * 6, 1, 0, b, 0);
  } else {
    vkCmdDraw(cmd, static_cast<uint32_t>(count), 1, 0, 0);
  }
}

}  // namespace

OGL_API void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
  Context* c = current;
  if (!c) return;
  if (first < 0 || count < 0) return gl_error(c, GL_INVALID_VALUE);
  draw(*c, mode, first, count);
}
