#include "bondriver/FileLock.h"

#include <cerrno>
#include <cstring>
#include <sys/stat.h>

#ifdef _WIN32
#  include <windows.h>

#  include <vector>
#else
#  include <fcntl.h>
#  include <sys/file.h>
#  include <unistd.h>
#endif

namespace bondriver {

#ifdef _WIN32
namespace {
std::wstring utf8ToWide(const std::string &text)
{
	if (text.empty()) {
		return std::wstring();
	}
	const int size =
	    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
	if (size <= 0) {
		return std::wstring();
	}
	std::wstring out(static_cast<size_t>(size), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size);
	return out;
}
} // namespace
#endif

FileLock::~FileLock()
{
	release();
}

bool FileLock::acquire(const std::string &path, std::string &error)
{
	release();
#ifdef _WIN32
	const std::wstring wide = utf8ToWide(path);
	HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
	                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		const DWORD e = GetLastError();
		if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION) {
			return false;
		}
		error = "CreateFile failed: " + std::to_string(e);
		return false;
	}
	handle_ = h;
	return true;
#else
	int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0) {
		error = std::string("open lock failed: ") + std::strerror(errno);
		return false;
	}
	if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
		const int e = errno;
		::close(fd);
		if (e == EWOULDBLOCK) {
			return false;
		}
		error = std::string("flock failed: ") + std::strerror(e);
		return false;
	}
	fd_ = fd;
	return true;
#endif
}

void FileLock::release()
{
#ifdef _WIN32
	if (handle_ != nullptr) {
		CloseHandle(static_cast<HANDLE>(handle_));
		handle_ = nullptr;
	}
#else
	if (fd_ >= 0) {
		::flock(fd_, LOCK_UN);
		::close(fd_);
		fd_ = -1;
	}
#endif
}

void FileLock::abandon()
{
#ifdef _WIN32
	handle_ = nullptr;
#else
	fd_ = -1;
#endif
}

bool FileLock::held() const
{
#ifdef _WIN32
	return handle_ != nullptr;
#else
	return fd_ >= 0;
#endif
}

bool ensureDirectory(const std::string &path, std::string &error)
{
	if (path.empty()) {
		return true;
	}
	std::string current;
	size_t i = 0;
	if (path[0] == '/') {
		current = "/";
		i = 1;
	}
	while (i <= path.size()) {
		const size_t slash = path.find('/', i);
		const std::string part = path.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
		if (!part.empty()) {
			if (!current.empty() && current.back() != '/') {
				current += '/';
			}
			current += part;
#ifdef _WIN32
			const std::wstring wide = utf8ToWide(current);
			if (!CreateDirectoryW(wide.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
				error = "mkdir failed: " + current;
				return false;
			}
#else
			if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
				error = std::string("mkdir failed: ") + current + ": " + std::strerror(errno);
				return false;
			}
#endif
		}
		if (slash == std::string::npos) {
			break;
		}
		i = slash + 1;
	}
	return true;
}

} // namespace bondriver
