#include "bondriver/ModulePath.h"

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace bondriver {

namespace {
void modulePathAnchor() {}
} // namespace

std::string currentModulePath()
{
#ifdef _WIN32
	wchar_t buffer[MAX_PATH];
	HMODULE module = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       reinterpret_cast<LPCWSTR>(&modulePathAnchor), &module)) {
		const DWORD n = GetModuleFileNameW(module, buffer, MAX_PATH);
		if (n > 0 && n < MAX_PATH) {
			const int size = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
			if (size > 0) {
				std::string out(static_cast<size_t>(size), '\0');
				WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(n), &out[0], size, nullptr, nullptr);
				return out;
			}
		}
	}
	return std::string();
#else
	Dl_info info;
	if (dladdr(reinterpret_cast<void *>(&modulePathAnchor), &info) != 0 && info.dli_fname != nullptr) {
		return std::string(info.dli_fname);
	}
	return std::string();
#endif
}

} // namespace bondriver
