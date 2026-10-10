// Matrix stacks (modelview, projection, texture per unit), column-major like GL.
#include "context.hpp"
#include "gl_api.hpp"
#include <cmath>
#include <cstring>
#include <utility>

using namespace oryon;

Mat4 oryon::mat_identity() {
  Mat4 m{};
  m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0f;
  return m;
}

Mat4 oryon::mat_mul(const Mat4& a, const Mat4& b) {
  Mat4 r;
  for (int col = 0; col < 4; ++col)
    for (int row = 0; row < 4; ++row) {
      float v = 0.0f;
      for (int k = 0; k < 4; ++k) v += a.m[k * 4 + row] * b.m[col * 4 + k];
      r.m[col * 4 + row] = v;
    }
  return r;
}

bool oryon::mat_invert(const Mat4& m, Mat4& out) {  // Gauss-Jordan with partial pivoting
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      a[r][c] = m.m[c * 4 + r];
      a[r][4 + c] = r == c;
    }
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
    if (std::fabs(a[p][c]) < 1e-12) return false;
    std::swap(a[p], a[c]);
    const double inv = 1.0 / a[c][c];
    for (double& v : a[c]) v *= inv;
    for (int r = 0; r < 4; ++r)
      if (r != c) {
        const double k = a[r][c];
        for (int j = 0; j < 8; ++j) a[r][j] -= k * a[c][j];
      }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out.m[c * 4 + r] = static_cast<float>(a[r][4 + c]);
  return true;
}

namespace {

MatrixStack& stack(Context* c) {
  switch (c->matrix_mode) {
    case GL_PROJECTION: return c->projection;
    case GL_TEXTURE: return c->units[c->active].matrix;
    default: return c->modelview;
  }
}

void apply(const Mat4& m) {
  Context* c = current;
  if (!c) return;
  Mat4& t = top(stack(c));
  t = mat_mul(t, m);
  c->ff_dirty = true;
}

}  // namespace

OGL_API void glMatrixMode(GLenum mode) {
  Context* c = current;
  if (!c) return;
  if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE) return gl_error(c, GL_INVALID_ENUM);
  c->matrix_mode = mode;
}

OGL_API void glLoadIdentity(void) {
  Context* c = current;
  if (!c) return;
  top(stack(c)) = mat_identity();
  c->ff_dirty = true;
}

OGL_API void glPushMatrix(void) {
  Context* c = current;
  if (!c) return;
  MatrixStack& s = stack(c);
  if (s.top + 1 == static_cast<int>(sizeof s.s / sizeof s.s[0])) return gl_error(c, GL_STACK_OVERFLOW);
  s.s[s.top + 1] = s.s[s.top];
  ++s.top;
}

OGL_API void glPopMatrix(void) {
  Context* c = current;
  if (!c) return;
  MatrixStack& s = stack(c);
  if (s.top == 0) return gl_error(c, GL_STACK_UNDERFLOW);
  --s.top;
  c->ff_dirty = true;
}

OGL_API void glMultMatrixf(const GLfloat *m) {
  if (!m) return;
  Mat4 x;
  std::memcpy(x.m, m, sizeof x.m);
  apply(x);
}

OGL_API void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
  Mat4 t = mat_identity();
  t.m[12] = x;
  t.m[13] = y;
  t.m[14] = z;
  apply(t);
}

OGL_API void glTranslated(GLdouble x, GLdouble y, GLdouble z) {
  glTranslatef(static_cast<GLfloat>(x), static_cast<GLfloat>(y), static_cast<GLfloat>(z));
}

OGL_API void glScalef(GLfloat x, GLfloat y, GLfloat z) {
  Mat4 t = mat_identity();
  t.m[0] = x;
  t.m[5] = y;
  t.m[10] = z;
  apply(t);
}

OGL_API void glScaled(GLdouble x, GLdouble y, GLdouble z) {
  glScalef(static_cast<GLfloat>(x), static_cast<GLfloat>(y), static_cast<GLfloat>(z));
}

OGL_API void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
  const float len = std::sqrt(x * x + y * y + z * z);
  if (len == 0.0f) return;
  x /= len;
  y /= len;
  z /= len;
  const float r = angle * 3.14159265358979f / 180.0f, c = std::cos(r), s = std::sin(r), k = 1.0f - c;
  Mat4 t = mat_identity();
  t.m[0] = x * x * k + c;
  t.m[1] = y * x * k + z * s;
  t.m[2] = x * z * k - y * s;
  t.m[4] = x * y * k - z * s;
  t.m[5] = y * y * k + c;
  t.m[6] = y * z * k + x * s;
  t.m[8] = x * z * k + y * s;
  t.m[9] = y * z * k - x * s;
  t.m[10] = z * z * k + c;
  apply(t);
}

OGL_API void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top_, GLdouble zNear, GLdouble zFar) {
  Context* c = current;
  if (!c) return;
  if (left == right || bottom == top_ || zNear == zFar) return gl_error(c, GL_INVALID_VALUE);
  Mat4 t = mat_identity();
  t.m[0] = static_cast<float>(2.0 / (right - left));
  t.m[5] = static_cast<float>(2.0 / (top_ - bottom));
  t.m[10] = static_cast<float>(-2.0 / (zFar - zNear));
  t.m[12] = static_cast<float>(-(right + left) / (right - left));
  t.m[13] = static_cast<float>(-(top_ + bottom) / (top_ - bottom));
  t.m[14] = static_cast<float>(-(zFar + zNear) / (zFar - zNear));
  apply(t);
}
