#pragma once

#include <cstdint>
#include <string>

namespace bondriver {

// Converts UTF-8 to UTF-16 code units (BON16CHAR storage).  Invalid sequences
// are replaced with U+FFFD.
std::u16string utf8ToUtf16(const std::string &text);

} // namespace bondriver
