#version 450
// Fixed-function vertex stage: transform, 2-light lighting with color material, texgen (unit 0), fog distance.

// Mirrors oryon::FFUniforms (src/context.hpp), std140.
layout(std140, set = 0, binding = 0) uniform FF {
  mat4 mvp;
  mat4 modelview;
  mat4 texmat[2];
  vec4 texgen[4];         // unit 0 planes S,T,R,Q (eye planes already times inverse modelview)
  vec4 light_pos[2];      // eye space
  vec4 light_diffuse[2];
  vec4 light_ambient[2];
  vec4 scene_ambient;
  vec4 fog_color;
  vec4 fog;               // start, end, density, 1/(end-start)
  vec4 env_color[2];
  vec4 env_scale[2];      // rgb scale, alpha scale
  ivec4 env[2];           // mode, rgb combine, alpha combine
  ivec4 flags;            // feature bits, alpha func (VkCompareOp order), fog mode, texgen modes
  vec4 alpha_ref;
} u;

layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec4 a_tex0;
layout(location = 3) in vec4 a_tex1;
layout(location = 4) in vec4 a_normal;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec4 v_tex0;
layout(location = 2) out vec4 v_tex1;
layout(location = 3) out float v_fog;

const int LIGHTING = 1, LIGHT0 = 2, CM_AMBIENT = 8, CM_DIFFUSE = 16;

void main() {
  vec4 eye = u.modelview * a_pos;
  gl_Position = u.mvp * a_pos;
  gl_Position.z = 0.5 * (gl_Position.z + gl_Position.w);  // GL clip z [-w,w] -> Vulkan [0,w]
  gl_PointSize = 1.0;

  vec4 c = a_color;
  int f = u.flags.x;
  if ((f & LIGHTING) != 0) {
    vec4 amb = (f & CM_AMBIENT) != 0 ? c : vec4(0.2, 0.2, 0.2, 1.0);
    vec4 dif = (f & CM_DIFFUSE) != 0 ? c : vec4(0.8, 0.8, 0.8, 1.0);
    vec3 n = normalize(mat3(u.modelview) * a_normal.xyz);  // shortcut: no inverse-transpose; MC scales uniformly
    vec3 sum = u.scene_ambient.rgb * amb.rgb;
    for (int i = 0; i < 2; ++i) {
      if ((f & (LIGHT0 << i)) == 0) continue;
      vec4 p = u.light_pos[i];
      vec3 l = normalize(p.w == 0.0 ? p.xyz : p.xyz - eye.xyz);
      sum += u.light_ambient[i].rgb * amb.rgb + max(dot(n, l), 0.0) * u.light_diffuse[i].rgb * dif.rgb;
    }
    c = vec4(clamp(sum, 0.0, 1.0), dif.a);
  }
  v_color = c;

  vec4 t0 = a_tex0;
  for (int i = 0; i < 4; ++i) {
    int m = (u.flags.w >> (2 * i)) & 3;  // 1 object linear, 2 eye linear
    if (m != 0) t0[i] = dot(u.texgen[i], m == 1 ? a_pos : eye);
  }
  v_tex0 = u.texmat[0] * t0;
  v_tex1 = u.texmat[1] * a_tex1;
  v_fog = abs(eye.z);
}
