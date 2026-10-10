#pragma once
#include "gl/gl_enums.hpp"
#include "oryon.hpp"
#include "vk/frame.hpp"

namespace gl {
struct Context {
    GLenum error = GL_NO_ERROR;
    GLfloat clear_color[4] = {0, 0, 0, 0};
    GLdouble clear_depth = 1.0;
    GLint viewport[4] = {0, 0, 0, 0};
    GLint pack_alignment = 4;
    GLint unpack_alignment = 4;
    bool initialized = false;  // viewport set from the first drawable
    ovk::Target* draw = nullptr;
    ovk::Target* read = nullptr;
    ovk::Recorder rec;
};

Context* current();
void make_current(Context* c);
void* lookup(const char* name);

inline void error(Context* c, GLenum e) {
    if (c->error == GL_NO_ERROR) c->error = e;
}
}
