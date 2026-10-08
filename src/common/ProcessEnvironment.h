#pragma once

#include <string>

namespace bondriver {

// Read the process environment rather than a DLL-local CRT environment copy.
// Variable names used here are ASCII; returned values are UTF-8.
std::string processEnvironmentValue(const char *name);

} // namespace bondriver
