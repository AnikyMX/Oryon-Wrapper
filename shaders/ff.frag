#version 450
// Fixed-function fragment stage: texture environments (units 0-1), fog, alpha test.

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

layout(set = 0, binding = 1) uniform sampler2D tex0;
layout(set = 0, binding = 2) uniform sampler2D tex1;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec4 v_tex0;
layout(location = 2) in vec4 v_tex1;
layout(location = 3) in float v_fog;
layout(location = 0) out vec4 o_color;

const int TEX0 = 32, TEX1 = 64;
vec4 t0, t1;

vec4 source(int s, int unit, vec4 prev) {
  switch (s) {
    case 0: return unit == 0 ? t0 : t1;  // GL_TEXTURE
    case 1: return u.env_color[unit];    // GL_CONSTANT
    case 2: return v_color;              // GL_PRIMARY_COLOR
    case 3: return prev;                 // GL_PREVIOUS
    case 4: return t0;                   // GL_TEXTURE0
    default: return t1;                  // GL_TEXTURE1
  }
}

vec4 operand(vec4 v, int op) {
  switch (op) {
    case 0: return v;
    case 1: return 1.0 - v;
    case 2: return vec4(v.a);
    default: return vec4(1.0 - v.a);
  }
}

// cfg: function | src0<<4 | src1<<8 | src2<<12 | op0<<16 | op1<<20 | op2<<24
vec4 combine(int cfg, int unit, vec4 prev) {
  vec4 a0 = operand(source((cfg >> 4) & 15, unit, prev), (cfg >> 16) & 15);
  vec4 a1 = operand(source((cfg >> 8) & 15, unit, prev), (cfg >> 20) & 15);
  vec4 a2 = operand(source((cfg >> 12) & 15, unit, prev), (cfg >> 24) & 15);
  switch (cfg & 15) {
    case 0: return a0;                // GL_REPLACE
    case 1: return a0 * a1;           // GL_MODULATE
    case 2: return a0 + a1;           // GL_ADD
    case 3: return a0 + a1 - 0.5;     // GL_ADD_SIGNED
    case 4: return mix(a1, a0, a2);   // GL_INTERPOLATE
    default: return a0 - a1;          // GL_SUBTRACT
  }
}

vec4 env(int unit, vec4 prev) {
  vec4 t = unit == 0 ? t0 : t1;
  ivec4 e = u.env[unit];
  switch (e.x) {
    case 0: return prev * t;                                                        // GL_MODULATE
    case 1: return t;                                                               // GL_REPLACE
    case 2: return vec4(mix(prev.rgb, t.rgb, t.a), prev.a);                         // GL_DECAL
    case 3: return vec4(mix(prev.rgb, u.env_color[unit].rgb, t.rgb), prev.a * t.a); // GL_BLEND
    case 4: return vec4(prev.rgb + t.rgb, prev.a * t.a);                            // GL_ADD
    default:                                                                        // GL_COMBINE
      return clamp(vec4(combine(e.y, unit, prev).rgb * u.env_scale[unit].x,
                        combine(e.z, unit, prev).a * u.env_scale[unit].y), 0.0, 1.0);
  }
}

void main() {
  int f = u.flags.x;
  t0 = (f & TEX0) != 0 ? textureProj(tex0, v_tex0.xyw) : vec4(1.0);
  t1 = (f & TEX1) != 0 ? textureProj(tex1, v_tex1.xyw) : vec4(1.0);
  vec4 c = v_color;
  if ((f & TEX0) != 0) c = env(0, c);
  if ((f & TEX1) != 0) c = env(1, c);

  int fog = u.flags.z;  // 1 linear, 2 exp, 3 exp2
  if (fog != 0) {
    float k = fog == 1 ? (u.fog.y - v_fog) * u.fog.w : exp(-(fog == 2 ? u.fog.z * v_fog : pow(u.fog.z * v_fog, 2.0)));
    c.rgb = mix(u.fog_color.rgb, c.rgb, clamp(k, 0.0, 1.0));
  }

  int af = u.flags.y;
  float a = c.a, r = u.alpha_ref.x;
  bool pass = af == 7 || (af == 1 && a < r) || (af == 2 && a == r) || (af == 3 && a <= r) ||
              (af == 4 && a > r) || (af == 5 && a != r) || (af == 6 && a >= r);
  if (!pass) discard;
  o_color = c;
}
