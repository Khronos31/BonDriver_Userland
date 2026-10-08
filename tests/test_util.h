#pragma once

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#  include <direct.h>
#  include <sys/stat.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace testutil {

inline int &failures()
{
	static int count = 0;
	return count;
}

inline void expect(bool cond, const std::string &message)
{
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", message.c_str());
		++failures();
	}
}

inline int report(const char *name)
{
	if (failures() == 0) {
		std::fprintf(stderr, "PASS: %s\n", name);
		return 0;
	}
	std::fprintf(stderr, "FAILED: %s (%d failures)\n", name, failures());
	return 1;
}

inline bool writeFile(const std::string &path, const std::string &content)
{
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file) {
		return false;
	}
	file << content;
	return static_cast<bool>(file);
}

inline std::string readFile(const std::string &path)
{
	std::ifstream file(path, std::ios::binary);
	std::ostringstream ss;
	ss << file.rdbuf();
	return ss.str();
}

inline bool makeDir(const std::string &path)
{
#ifdef _WIN32
	return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
	return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

inline std::string tempRoot()
{
#ifdef _WIN32
	const char *base = std::getenv("TEMP");
	if (base == nullptr) {
		base = "C:/Windows/Temp";
	}
#else
	const char *base = std::getenv("TMPDIR");
	if (base == nullptr) {
		base = "/tmp";
	}
#endif
	return std::string(base) + "/bondriver-test";
}

} // namespace testutil
