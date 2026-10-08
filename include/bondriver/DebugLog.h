// In-memory diagnostic tail. No function in this interface writes to the
// host's shared stderr handle; that could block a caller or race other writers.
#pragma once

#include <string>

namespace bondriver {

// Appends a tagged line to the process-local bounded diagnostic tail.
void logLine(const std::string &tag, const std::string &message);

// Append raw UTF-8 child diagnostic text and read a snapshot of the same tail.
// This is an internal C++ helper, not part of the BonDriver public ABI.
void appendDiagnosticText(const std::string &text);
std::string diagnosticTail();

} // namespace bondriver
