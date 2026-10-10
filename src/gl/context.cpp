#include "gl/context.hpp"
#include <cstring>

#include "gl/gl_decl.inc"

namespace gl {
namespace {
thread_local Context* t_ctx = nullptr;

// Entry points LWJGL resolves but Minecraft never calls.
[[maybe_unused]] uintptr_t stub() {
    if (Context* c = t_ctx) error(c, GL_INVALID_OPERATION);
    return 0;
}

struct Proc {
    const char* name;
    void* fn;
};
#define ORYON_PROC(n) {#n, reinterpret_cast<void*>(&n)},
#define ORYON_STUB(n) {#n, reinterpret_cast<void*>(&stub)},
const Proc kProcs[] = {
#include "gl/procs.inc"
    {nullptr, nullptr}};
#undef ORYON_PROC
#undef ORYON_STUB
constexpr size_t kNumProcs = sizeof(kProcs) / sizeof(kProcs[0]) - 1;
}

Context* current() { return t_ctx; }
void make_current(Context* c) { t_ctx = c; }

void* lookup(const char* name) {
    size_t lo = 0, hi = kNumProcs;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const int d = std::strcmp(kProcs[mid].name, name);
        if (d == 0) return kProcs[mid].fn;
        if (d < 0) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}
}
