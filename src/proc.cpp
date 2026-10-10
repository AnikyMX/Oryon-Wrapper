#include "proc.hpp"
#include "gl_api.hpp"
#include <cstring>
#include <iterator>
#include <string_view>

namespace oryon {

namespace {

using Fn = __eglMustCastToProperFunctionPointerType;

[[maybe_unused]] void oryon_stub() {}

#define ORYON_EGL(n) #n,
#define ORYON_PROC(n) #n,
#define ORYON_STUB(n) #n,
constexpr const char* kNames[] = {
#include "proc_table.inc"
};
#undef ORYON_EGL
#undef ORYON_PROC
#undef ORYON_STUB

#define ORYON_EGL(n) reinterpret_cast<Fn>(&n),
#define ORYON_PROC(n) reinterpret_cast<Fn>(&n),
#define ORYON_STUB(n) reinterpret_cast<Fn>(&oryon_stub),
const Fn kFns[] = {
#include "proc_table.inc"
};
#undef ORYON_EGL
#undef ORYON_PROC
#undef ORYON_STUB

constexpr bool sorted() {
  for (std::size_t i = 1; i < std::size(kNames); ++i)
    if (!(std::string_view(kNames[i - 1]) < std::string_view(kNames[i]))) return false;
  return true;
}
static_assert(sorted(), "proc_table.inc must be sorted and unique");
static_assert(std::size(kNames) == std::size(kFns));

}  // namespace

Fn proc_lookup(const char* name) {
  std::size_t lo = 0, hi = std::size(kNames);
  while (lo < hi) {
    const std::size_t mid = (lo + hi) / 2;
    const int c = std::strcmp(kNames[mid], name);
    if (!c) return kFns[mid];
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return nullptr;
}

}  // namespace oryon
