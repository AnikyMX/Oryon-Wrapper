#pragma once

// Reported context version. The validator-enforced claim marker ORYON_CAP_VERSION(3, 0)
// replaces these once every GL function Minecraft 1.12.2 reaches is implemented.
namespace gl {
constexpr int kVersionMajor = 3;
constexpr int kVersionMinor = 0;
constexpr const char* kVersion = "3.0 Oryon";
constexpr const char* kGlslVersion = "1.30";
constexpr const char* kVendor = "Oryon";

// Advertised extensions; each entry is written as ORYON_CAP_EXT(GL_x) so validate.py
// checks the entry points that LWJGL's GLCapabilities requires for it.
#define ORYON_CAP_EXT(x) #x,
constexpr const char* kExtensions[] = {nullptr};
#undef ORYON_CAP_EXT
constexpr int kNumExtensions = int(sizeof(kExtensions) / sizeof(kExtensions[0])) - 1;
}
