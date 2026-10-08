#include "ProcessEnvironment.h"

#ifdef _WIN32
#  include <windows.h>

#  include <vector>
#else
#  include <cstdlib>
#endif

namespace bondriver {

std::string processEnvironmentValue(const char *name)
{
	if (name == nullptr || *name == '\0') {
		return std::string();
	}
#ifdef _WIN32
	std::wstring wide_name;
	for (const unsigned char *p = reinterpret_cast<const unsigned char *>(name); *p != 0; ++p) {
		if (*p > 0x7f) {
			return std::string();
		}
		wide_name.push_back(static_cast<wchar_t>(*p));
	}
	const DWORD needed = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
	if (needed == 0) {
		return std::string();
	}
	std::vector<wchar_t> value(needed);
	const DWORD length = GetEnvironmentVariableW(wide_name.c_str(), value.data(), needed);
	if (length == 0 || length >= needed) {
		return std::string();
	}
	const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(length),
	                                      nullptr, 0, nullptr, nullptr);
	if (bytes <= 0) {
		return std::string();
	}
	std::string out(static_cast<size_t>(bytes), '\0');
	if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(length), &out[0], bytes,
	                        nullptr, nullptr) != bytes) {
		return std::string();
	}
	return out;
#else
	const char *value = std::getenv(name);
	return value != nullptr ? std::string(value) : std::string();
#endif
}

} // namespace bondriver
