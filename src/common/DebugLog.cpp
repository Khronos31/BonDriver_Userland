#include "bondriver/DebugLog.h"

#include <mutex>

namespace bondriver {

namespace {
std::mutex g_log_mutex;
std::string g_log_tail;
constexpr size_t kLogTailLimit = 8192;
}

void appendDiagnosticText(const std::string &text)
{
	if (text.empty()) return;
	std::lock_guard<std::mutex> lock(g_log_mutex);
	g_log_tail.append(text);
	if (g_log_tail.size() > kLogTailLimit) g_log_tail.erase(0, g_log_tail.size() - kLogTailLimit);
}

std::string diagnosticTail()
{
	std::lock_guard<std::mutex> lock(g_log_mutex);
	return g_log_tail;
}

void logLine(const std::string &tag, const std::string &message)
{
	appendDiagnosticText("[" + tag + "] " + message + "\n");
}

} // namespace bondriver
