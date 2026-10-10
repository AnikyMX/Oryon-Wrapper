// Exported GL entry points. Definitions use the same OGL_API prototypes (C linkage, compiler-checked).
#pragma once
#include "oryon.hpp"

OGL_API void glClear(GLbitfield mask);
OGL_API void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
OGL_API void glClearDepth(GLdouble depth);
OGL_API void glClearStencil(GLint s);
OGL_API void glDisable(GLenum cap);
OGL_API void glEnable(GLenum cap);
OGL_API void glFinish(void);
OGL_API void glFlush(void);
OGL_API void glGetBooleanv(GLenum pname, GLboolean *data);
OGL_API GLenum glGetError(void);
OGL_API void glGetFloatv(GLenum pname, GLfloat *data);
OGL_API void glGetIntegerv(GLenum pname, GLint *data);
OGL_API const GLubyte *glGetString(GLenum name);
OGL_API GLboolean glIsEnabled(GLenum cap);
OGL_API void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels);
OGL_API void glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
OGL_API void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
