#include "bondriver/Config.h"
#include "ProcessEnvironment.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <limits>
#include <set>
#include <sstream>
#include <sys/stat.h>

#ifdef _WIN32
#  include <filesystem>
#  include <windows.h>
#else
#  include <limits.h>
#endif

namespace bondriver {

namespace {

std::string trim(const std::string &s)
{
	size_t b = 0;
	size_t e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) {
		++b;
	}
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) {
		--e;
	}
	return s.substr(b, e - b);
}

std::string dirName(const std::string &path)
{
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string baseName(const std::string &path)
{
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string stripLibraryExtension(const std::string &name)
{
	const size_t dot = name.find_last_of('.');
	if (dot == std::string::npos) {
		return name;
	}
	const std::string ext = name.substr(dot);
	if (ext == ".so" || ext == ".dylib" || ext == ".dll" || ext == ".bundle") {
		std::string base = name.substr(0, dot);
		return base;
	}
	return name;
}

bool isAbsolutePath(const std::string &p)
{
	if (p.empty()) {
		return false;
	}
	if (p[0] == '/' || p[0] == '\\') {
		return true;
	}
	return p.size() >= 2 && p[1] == ':';
}

std::string joinPath(const std::string &dir, const std::string &rel)
{
	if (rel.empty() || isAbsolutePath(rel)) {
		return rel;
	}
	if (dir.empty()) {
		return rel;
	}
	const char last = dir[dir.size() - 1];
	if (last == '/' || last == '\\') {
		return dir + rel;
	}
	return dir + "/" + rel;
}

// Lexically normalizes a path by collapsing '.' and '..' segments.  Does not
// require the path to exist.
std::string normalizePath(const std::string &path)
{
	if (path.empty()) {
		return path;
	}
	std::vector<std::string> parts;
	// Preserve a leading UNC prefix ("//server/share") instead of collapsing it.
	std::string prefix;
	size_t i = 0;
	if (path.size() >= 2 && path[0] == '/' && path[1] == '/') {
		prefix = "//";
		i = 2;
	} else if (path[0] == '/') {
		prefix = "/";
		i = 1;
	}
	const bool absolute = path[0] == '/';
	while (i <= path.size()) {
		const size_t slash = path.find('/', i);
		const std::string part = path.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
		if (part == "." || part.empty()) {
			// skip
		} else if (part == "..") {
			if (!parts.empty() && parts.back() != "..") {
				parts.pop_back();
			} else if (!absolute) {
				parts.push_back(part);
			}
		} else {
			parts.push_back(part);
		}
		if (slash == std::string::npos) {
			break;
		}
		i = slash + 1;
	}
	std::string out = prefix;
	for (size_t k = 0; k < parts.size(); ++k) {
		if (k != 0) {
			out += '/';
		}
		out += parts[k];
	}
	if (out.empty()) {
		return absolute ? "/" : ".";
	}
	return out;
}

#ifdef _WIN32
std::wstring utf8ToWide(const std::string &text)
{
	if (text.empty()) {
		return std::wstring();
	}
	const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
	if (size <= 0) {
		return std::wstring();
	}
	std::wstring out(static_cast<size_t>(size), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size);
	return out;
}

std::string wideToUtf8(const std::wstring &text)
{
	if (text.empty()) {
		return std::string();
	}
	const int size =
	    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if (size <= 0) {
		return std::string();
	}
	std::string out(static_cast<size_t>(size), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size, nullptr, nullptr);
	return out;
}
#endif

// Resolves an existing directory to its filesystem-canonical absolute path so
// symlink/junction aliases map to one receiver identity.  Fails when the path
// cannot be resolved, instead of silently forming a separate lexical identity.
bool canonicalizeExistingDirectory(const std::string &path, std::string &out, std::string &error)
{
#ifdef _WIN32
	const std::wstring wide = utf8ToWide(path);
	HANDLE h = CreateFileW(wide.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
	                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		error = "runtime_dir not found: " + path;
		return false;
	}
	FILE_STANDARD_INFO standard{};
	if (!GetFileInformationByHandleEx(h, FileStandardInfo, &standard, sizeof standard) || !standard.Directory) {
		CloseHandle(h);
		error = "runtime_dir is not a directory: " + path;
		return false;
	}
	DWORD need = GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if (need == 0) {
		CloseHandle(h);
		error = "runtime_dir canonicalization failed: " + path;
		return false;
	}
	std::wstring buffer(static_cast<size_t>(need), L'\0');
	const DWORD got = GetFinalPathNameByHandleW(h, &buffer[0], need, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	CloseHandle(h);
	if (got == 0 || got >= need) {
		error = "runtime_dir canonicalization failed: " + path;
		return false;
	}
	buffer.resize(got);
	std::wstring canonical = buffer;
	if (canonical.rfind(L"\\\\?\\UNC\\", 0) == 0) {
		canonical = L"\\\\" + canonical.substr(8);
	} else if (canonical.rfind(L"\\\\?\\", 0) == 0) {
		canonical = canonical.substr(4);
	}
	out = wideToUtf8(canonical);
	if (out.empty()) {
		error = "runtime_dir canonicalization failed: " + path;
		return false;
	}
	return true;
#else
	char *resolved = ::realpath(path.c_str(), nullptr);
	if (resolved == nullptr) {
		error = "runtime_dir not found: " + path;
		return false;
	}
	struct stat info{};
	const bool is_directory = ::stat(resolved, &info) == 0 && S_ISDIR(info.st_mode);
	if (!is_directory) {
		::free(resolved);
		error = "runtime_dir is not a directory: " + path;
		return false;
	}
	out = resolved;
	::free(resolved);
	return true;
#endif
}

bool parseUnsigned(const std::string &text, uint64_t min, uint64_t max, uint64_t &out)
{
	if (text.empty()) {
		return false;
	}
	errno = 0;
	char *end = nullptr;
	const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
	if (errno != 0 || end == text.c_str() || *end != '\0') {
		return false;
	}
	if (v < min || v > max) {
		return false;
	}
	out = static_cast<uint64_t>(v);
	return true;
}

struct IniFile {
	std::map<std::string, std::map<std::string, std::string>> sections;
};

bool parseIni(const std::string &text, IniFile &out, std::string &error)
{
	std::istringstream stream(text);
	std::string line;
	std::string section = "";
	int line_no = 0;
	while (std::getline(stream, line)) {
		++line_no;
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		const std::string stripped = trim(line);
		if (stripped.empty() || stripped[0] == '#' || stripped[0] == ';') {
			continue;
		}
		if (stripped[0] == '[') {
			const size_t close = stripped.find(']');
			if (close == std::string::npos || trim(stripped.substr(close + 1)).size() != 0) {
				error = "line " + std::to_string(line_no) + ": malformed section";
				return false;
			}
			section = trim(stripped.substr(1, close - 1));
			if (section.empty()) {
				error = "line " + std::to_string(line_no) + ": empty section name";
				return false;
			}
			continue;
		}
		const size_t eq = stripped.find('=');
		if (eq == std::string::npos) {
			error = "line " + std::to_string(line_no) + ": expected key = value";
			return false;
		}
		const std::string key = trim(stripped.substr(0, eq));
		const std::string value = trim(stripped.substr(eq + 1));
		if (key.empty()) {
			error = "line " + std::to_string(line_no) + ": empty key";
			return false;
		}
		const std::string full = section + "." + key;
		auto &table = out.sections[section];
		if (table.find(key) != table.end()) {
			error = "line " + std::to_string(line_no) + ": duplicate key '" + key + "' in [" + section + "]";
			return false;
		}
		table[key] = value;
	}
	return true;
}

bool readFile(const std::string &path, std::string &out, std::string &error)
{
	// u8path keeps UTF-8 paths intact on Windows (std::ifstream(std::string)
	// would use the active code page otherwise). POSIX ifstream accepts UTF-8
	// path bytes directly and avoids GCC 8's separate stdc++fs link dependency.
#ifdef _WIN32
	std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
#else
	std::ifstream file(path, std::ios::binary);
#endif
	if (!file) {
		error = "cannot open '" + path + "': " + std::strerror(errno);
		return false;
	}
	std::ostringstream ss;
	ss << file.rdbuf();
	out = ss.str();
	return true;
}

std::vector<std::string> splitList(const std::string &value, char sep)
{
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= value.size()) {
		const size_t pos = value.find(sep, start);
		const std::string item = trim(value.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
		if (!item.empty()) {
			out.push_back(item);
		}
		if (pos == std::string::npos) {
			break;
		}
		start = pos + 1;
	}
	return out;
}

bool checkAllowedKeys(const std::map<std::string, std::string> &table, const std::set<std::string> &allowed,
                      const std::string &section, std::string &error)
{
	for (const auto &kv : table) {
		if (allowed.find(kv.first) == allowed.end()) {
			error = "unknown option '" + kv.first + "' in [" + section + "]";
			return false;
		}
	}
	return true;
}

const std::map<std::string, std::string> *findSection(const IniFile &ini, const std::string &name)
{
	const auto it = ini.sections.find(name);
	return it == ini.sections.end() ? nullptr : &it->second;
}

std::string getOr(const std::map<std::string, std::string> &table, const std::string &key, const std::string &def)
{
	const auto it = table.find(key);
	return it == table.end() ? def : it->second;
}

bool parsePx4Target(const std::string &entry, Px4TargetConfig &out, std::string &error)
{
	// instance:receiver:systems  (systems is T, S or TS)
	const size_t c1 = entry.find(':');
	if (c1 == std::string::npos) {
		error = "invalid PX4 target '" + entry + "' (expected instance:receiver:systems)";
		return false;
	}
	const size_t c2 = entry.find(':', c1 + 1);
	if (c2 == std::string::npos) {
		error = "invalid PX4 target '" + entry + "' (expected instance:receiver:systems)";
		return false;
	}
	out.instance = trim(entry.substr(0, c1));
	const std::string receiver = trim(entry.substr(c1 + 1, c2 - c1 - 1));
	const std::string systems = trim(entry.substr(c2 + 1));
	if (out.instance.empty() || receiver.empty() || systems.empty()) {
		error = "invalid PX4 target '" + entry + "'";
		return false;
	}
	uint64_t rv = 0;
	if (!parseUnsigned(receiver, 0, 7, rv)) {
		error = "PX4 receiver out of range 0..7 in '" + entry + "'";
		return false;
	}
	out.receiver = static_cast<int>(rv);
	for (char c : systems) {
		if (c == 'T' || c == 't') {
			out.supports_t = true;
		} else if (c == 'S' || c == 's') {
			out.supports_s = true;
		} else {
			error = "invalid system character in PX4 target '" + entry + "'";
			return false;
		}
	}
	if (!out.supports_t && !out.supports_s) {
		error = "PX4 target '" + entry + "' supports no system";
		return false;
	}
	return true;
}

bool loadSiano(const IniFile &ini, const std::string &dir, SianoConfig &out, std::string &error)
{
	const std::map<std::string, std::string> empty;
	const auto *sec = findSection(ini, "siano");
	const std::map<std::string, std::string> &table = sec ? *sec : empty;
	if (!checkAllowedKeys(table, {"device", "devices", "firmware"}, "siano", error)) {
		return false;
	}
	const std::string device = getOr(table, "device", "");
	const std::string devices = getOr(table, "devices", "");
	if (device.empty() == devices.empty()) {
		error = "config [siano] requires exactly one of device or devices";
		return false;
	}
	if (!device.empty()) {
		out.device = device;
	} else {
		out.pool_mode = true;
		out.devices = splitList(devices, ',');
		if (out.devices.empty()) {
			error = "config [siano] devices is empty";
			return false;
		}
		std::set<std::string> seen;
		for (const std::string &d : out.devices) {
			if (!seen.insert(d).second) {
				error = "duplicate Siano selector '" + d + "'";
				return false;
			}
		}
	}
	const std::string firmware = getOr(table, "firmware", "");
	if (!firmware.empty()) {
		out.firmware = joinPath(dir, firmware);
	}
	return true;
}

bool loadPx4(const IniFile &ini, const std::string &dir, Px4Config &out, std::string &error)
{
	const std::map<std::string, std::string> empty;
	const auto *sec = findSection(ini, "px4");
	const std::map<std::string, std::string> &table = sec ? *sec : empty;
	if (!checkAllowedKeys(table,
	                      {"runtime_dir", "instance", "receiver", "receivers", "systems", "lnb_voltage", "allow_lnb_15"},
	                      "px4", error)) {
		return false;
	}
	const std::string runtime = getOr(table, "runtime_dir", "");
	if (runtime.empty()) {
		error = "config [px4] requires runtime_dir";
		return false;
	}
	if (!canonicalizeExistingDirectory(joinPath(dir, runtime), out.runtime_dir, error)) {
		return false;
	}

	const std::string instance = getOr(table, "instance", "");
	const std::string receiver = getOr(table, "receiver", "");
	const std::string receivers = getOr(table, "receivers", "");
	const bool fixed_mode = !instance.empty() || !receiver.empty();
	if (fixed_mode && !receivers.empty()) {
		error = "config [px4] cannot combine instance/receiver with receivers";
		return false;
	}
	if (!fixed_mode && receivers.empty()) {
		error = "config [px4] requires instance+receiver or receivers";
		return false;
	}
	if (fixed_mode) {
		if (instance.empty() || receiver.empty()) {
			error = "config [px4] fixed mode requires both instance and receiver";
			return false;
		}
		out.fixed.instance = instance;
		uint64_t rv = 0;
		if (!parseUnsigned(receiver, 0, 7, rv)) {
			error = "config [px4] receiver out of range 0..7";
			return false;
		}
		out.fixed.receiver = static_cast<int>(rv);
		const std::string systems = getOr(table, "systems", "TS");
		for (char c : systems) {
			if (c == 'T' || c == 't') {
				out.fixed.supports_t = true;
			} else if (c == 'S' || c == 's') {
				out.fixed.supports_s = true;
			} else {
				error = "config [px4] invalid systems '" + systems + "'";
				return false;
			}
		}
		if (!out.fixed.supports_t && !out.fixed.supports_s) {
			error = "config [px4] systems is empty";
			return false;
		}
	} else {
		out.pool_mode = true;
		for (const std::string &entry : splitList(receivers, ';')) {
			Px4TargetConfig target;
			if (!parsePx4Target(entry, target, error)) {
				return false;
			}
			out.targets.push_back(target);
		}
		if (out.targets.empty()) {
			error = "config [px4] receivers is empty";
			return false;
		}
	}
	{
		std::set<std::string> seen;
		const Px4TargetConfig *list = nullptr;
		size_t count = 0;
		Px4TargetConfig single;
		if (out.pool_mode) {
			list = out.targets.data();
			count = out.targets.size();
		} else {
			single = out.fixed;
			list = &single;
			count = 1;
		}
		for (size_t i = 0; i < count; ++i) {
			const std::string key = list[i].instance + "/" + std::to_string(list[i].receiver);
			if (!seen.insert(key).second) {
				error = "duplicate PX4 receiver '" + key + "'";
				return false;
			}
		}
	}

	uint64_t lnb = 0;
	if (!parseUnsigned(getOr(table, "lnb_voltage", "0"), 0, 15, lnb) || (lnb != 0 && lnb != 15)) {
		error = "config [px4] lnb_voltage must be 0 or 15";
		return false;
	}
	uint64_t allow = 0;
	if (!parseUnsigned(getOr(table, "allow_lnb_15", "0"), 0, 1, allow)) {
		error = "config [px4] allow_lnb_15 must be 0 or 1";
		return false;
	}
	out.lnb_voltage = static_cast<int>(lnb);
	out.allow_lnb_15 = allow != 0;
	if (out.lnb_voltage == 15 && !out.allow_lnb_15) {
		error = "config [px4] lnb_voltage 15 requires allow_lnb_15 = 1";
		return false;
	}
	return true;
}

bool loadCommon(const IniFile &ini, const std::string &dir, CommonConfig &out, std::string &error)
{
	const std::map<std::string, std::string> empty;
	const auto *sec = findSection(ini, "common");
	const std::map<std::string, std::string> &table = sec ? *sec : empty;
	if (!checkAllowedKeys(table,
	                      {"tuner_name", "cli_path", "channel_table", "lock_dir", "tune_timeout_ms",
	                       "stop_timeout_ms", "kill_timeout_ms", "buffer_limit_bytes", "diagnostics"},
	                      "common", error)) {
		return false;
	}
	out.tuner_name = getOr(table, "tuner_name", "");
	out.cli_path = getOr(table, "cli_path", "");
	const std::string channel_table = getOr(table, "channel_table", "");
	if (out.tuner_name.empty()) {
		error = "config [common] requires tuner_name";
		return false;
	}
	if (out.cli_path.empty()) {
		error = "config [common] requires cli_path";
		return false;
	}
	if (channel_table.empty()) {
		error = "config [common] requires channel_table";
		return false;
	}
	out.cli_path = joinPath(dir, out.cli_path);
	out.channel_table = joinPath(dir, channel_table);

	uint64_t v = 0;
	if (!parseUnsigned(getOr(table, "tune_timeout_ms", "4000"), 1, 3600000, v)) {
		error = "config [common] invalid tune_timeout_ms";
		return false;
	}
	out.tune_timeout_ms = v;
	if (!parseUnsigned(getOr(table, "stop_timeout_ms", "5000"), 1, 3600000, v)) {
		error = "config [common] invalid stop_timeout_ms";
		return false;
	}
	out.stop_timeout_ms = v;
	if (!parseUnsigned(getOr(table, "kill_timeout_ms", "2000"), 1, 3600000, v)) {
		error = "config [common] invalid kill_timeout_ms";
		return false;
	}
	out.kill_timeout_ms = v;
	if (!parseUnsigned(getOr(table, "buffer_limit_bytes", "8388608"), 188, 1ull << 34, v) ||
	    v > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
		error = "config [common] invalid buffer_limit_bytes";
		return false;
	}
	out.buffer_limit_bytes = static_cast<size_t>(v);

	out.diagnostics = getOr(table, "diagnostics", "stderr");
	if (out.diagnostics != "stderr" && out.diagnostics != "discard" && out.diagnostics.rfind("file:", 0) != 0) {
		error = "config [common] diagnostics must be stderr, discard or file:PATH";
		return false;
	}
	if (out.diagnostics.rfind("file:", 0) == 0) {
		const std::string file = out.diagnostics.substr(5);
		if (file.empty()) {
			error = "config [common] diagnostics file path is empty";
			return false;
		}
		out.diagnostics = "file:" + joinPath(dir, file);
	}

	const std::string lock_dir = getOr(table, "lock_dir", "");
	if (lock_dir.empty()) {
#ifdef _WIN32
		const std::string program_data = processEnvironmentValue("ProgramData");
		out.lock_dir = !program_data.empty()
		                   ? program_data + "/BonDriver_Userland"
		                   : std::string("C:/ProgramData/BonDriver_Userland");
#elif defined(__APPLE__)
		const char *tmp = std::getenv("TMPDIR");
		out.lock_dir = (tmp != nullptr && *tmp != '\0') ? std::string(tmp) + "/bondriver-userland"
		                                                : std::string("/tmp/bondriver-userland");
#else
		const char *xdg = std::getenv("XDG_RUNTIME_DIR");
		out.lock_dir = (xdg != nullptr && *xdg != '\0') ? std::string(xdg) + "/bondriver-userland"
		                                                : std::string("/run/bondriver-userland");
#endif
	} else {
		out.lock_dir = isAbsolutePath(lock_dir) ? normalizePath(lock_dir) : normalizePath(joinPath(dir, lock_dir));
	}
	return true;
}

bool validateAgainstBackend(const Config &config, std::string &error)
{
	bool has_t = false;
	bool has_s = false;
	if (config.kind == BackendKind::Siano) {
		has_t = true;
	} else {
		if (config.px4.pool_mode) {
			for (const auto &t : config.px4.targets) {
				has_t = has_t || t.supports_t;
				has_s = has_s || t.supports_s;
			}
		} else {
			has_t = config.px4.fixed.supports_t;
			has_s = config.px4.fixed.supports_s;
		}
	}
	for (const Space &space : config.spaces) {
		for (const ChannelEntry &ch : space.channels) {
			if (ch.system == System::IsdbS && !has_s) {
				error = "channel '" + ch.id + "' requires isdb-s but no receiver supports it";
				return false;
			}
			if (ch.system == System::IsdbT && !has_t) {
				error = "channel '" + ch.id + "' requires isdb-t but no receiver supports it";
				return false;
			}
		}
	}
	return true;
}

} // namespace

bool Config::findChannel(const std::string &id, size_t &space, size_t &channel) const
{
	for (size_t s = 0; s < spaces.size(); ++s) {
		for (size_t c = 0; c < spaces[s].channels.size(); ++c) {
			if (spaces[s].channels[c].id == id) {
				space = s;
				channel = c;
				return true;
			}
		}
	}
	return false;
}

bool parseChannelTable(const std::string &text, BackendKind kind, std::vector<Space> &out, std::string &error)
{
	out.clear();
	std::istringstream stream(text);
	std::string line;
	int line_no = 0;
	std::set<std::string> channel_ids;
	std::map<std::string, std::string> space_names;
	while (std::getline(stream, line)) {
		++line_no;
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.empty() || line[0] == '#') {
			continue;
		}
		std::vector<std::string> fields;
		size_t start = 0;
		while (start <= line.size()) {
			const size_t tab = line.find('\t', start);
			fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
			if (tab == std::string::npos) {
				break;
			}
			start = tab + 1;
		}
		if (fields.size() == 1 || (fields.size() >= 1 && trim(fields[0]) == "space_id")) {
			if (fields.size() >= 1 && trim(fields[0]) == "space_id") {
				continue; // header
			}
			error = "channel table line " + std::to_string(line_no) + ": expected 8 tab-separated fields";
			return false;
		}
		while (fields.size() < 8) {
			fields.push_back("");
		}
		if (fields.size() != 8) {
			error = "channel table line " + std::to_string(line_no) + ": expected 8 tab-separated fields";
			return false;
		}
		for (std::string &f : fields) {
			f = trim(f);
		}
		const std::string space_id = fields[0];
		const std::string space_name = fields[1];
		const std::string channel_id = fields[2];
		const std::string channel_name = fields[3];
		const std::string system = fields[4];
		const std::string frequency = fields[5];
		const std::string stream_id = fields[6];
		const std::string slot = fields[7];
		if (space_id.empty() || channel_id.empty()) {
			error = "channel table line " + std::to_string(line_no) + ": empty space or channel id";
			return false;
		}
		const auto existing = space_names.find(space_id);
		if (existing != space_names.end() && existing->second != space_name) {
			error = "channel table line " + std::to_string(line_no) + ": inconsistent name for space '" + space_id + "'";
			return false;
		}
		space_names[space_id] = space_name;
		if (!channel_ids.insert(channel_id).second) {
			error = "channel table line " + std::to_string(line_no) + ": duplicate channel id '" + channel_id + "'";
			return false;
		}
		ChannelEntry entry;
		entry.id = channel_id;
		entry.name = channel_name.empty() ? channel_id : channel_name;
		if (system == "isdb-t") {
			entry.system = System::IsdbT;
		} else if (system == "isdb-s") {
			entry.system = System::IsdbS;
		} else {
			error = "channel table line " + std::to_string(line_no) + ": unknown system '" + system + "'";
			return false;
		}
		uint64_t freq = 0;
		if (!parseUnsigned(frequency, 1, 10ull * 1000 * 1000 * 1000, freq)) {
			error = "channel table line " + std::to_string(line_no) + ": invalid frequency '" + frequency + "'";
			return false;
		}
		entry.frequency_hz = static_cast<int64_t>(freq);
		if (entry.system == System::IsdbT) {
			if (freq < 90ull * 1000 * 1000 || freq > 1000ull * 1000 * 1000) {
				error = "channel table line " + std::to_string(line_no) +
				        ": isdb-t frequency out of range (units must be Hz)";
				return false;
			}
		} else {
			if (freq < 950ull * 1000 * 1000 || freq > 3500ull * 1000 * 1000) {
				error = "channel table line " + std::to_string(line_no) +
				        ": isdb-s frequency out of range (units must be Hz)";
				return false;
			}
		}
		const bool has_stream = stream_id != "-" && !stream_id.empty();
		const bool has_slot = slot != "-" && !slot.empty();
		if (entry.system == System::IsdbT) {
			if (has_stream || has_slot) {
				error = "channel table line " + std::to_string(line_no) + ": isdb-t must not set stream_id/slot";
				return false;
			}
		} else {
			if (has_stream == has_slot) {
				error = "channel table line " + std::to_string(line_no) +
				        ": isdb-s requires exactly one of stream_id or slot";
				return false;
			}
			uint64_t v = 0;
			if (has_stream) {
				if (!parseUnsigned(stream_id, 0, 0xFFFF, v)) {
					error = "channel table line " + std::to_string(line_no) + ": invalid stream_id";
					return false;
				}
				entry.stream_id = static_cast<int>(v);
			}
			if (has_slot) {
				if (!parseUnsigned(slot, 0, 11, v)) {
					error = "channel table line " + std::to_string(line_no) + ": invalid slot";
					return false;
				}
				entry.slot = static_cast<int>(v);
			}
		}
		(void)kind;
		auto space_it = std::find_if(out.begin(), out.end(), [&](const Space &s) { return s.id == space_id; });
		if (space_it == out.end()) {
			Space s;
			s.id = space_id;
			s.name = space_name;
			out.push_back(s);
			space_it = out.end() - 1;
		}
		space_it->channels.push_back(entry);
	}
	if (out.empty()) {
		error = "channel table is empty";
		return false;
	}
	return true;
}

bool loadConfig(BackendKind kind, const std::string &module_path, const std::string &override_path, Config &out,
                std::string &error)
{
	std::string module_dir;
	std::string module_basename;
	std::string path = override_path;
	if (path.empty()) {
		path = resolveConfigPath(module_path, kind, module_dir, module_basename);
	} else {
		module_dir = dirName(module_path);
		module_basename = stripLibraryExtension(baseName(module_path));
	}
	out.kind = kind;
	// Relative paths always resolve against the directory of the INI that was
	// actually loaded, including the test-only explicit override path.
	out.module_dir = dirName(path);
	module_dir = out.module_dir;
	out.config_path = path;

	std::string text;
	if (!readFile(path, text, error)) {
		return false;
	}
	IniFile ini;
	if (!parseIni(text, ini, error)) {
		return false;
	}
	if (!loadCommon(ini, module_dir, out.common, error)) {
		return false;
	}
	if (kind == BackendKind::Siano) {
		if (!loadSiano(ini, module_dir, out.siano, error)) {
			return false;
		}
	} else {
		if (!loadPx4(ini, module_dir, out.px4, error)) {
			return false;
		}
	}
	std::string table_text;
	if (!readFile(out.common.channel_table, table_text, error)) {
		return false;
	}
	if (!parseChannelTable(table_text, kind, out.spaces, error)) {
		return false;
	}
	if (!validateAgainstBackend(out, error)) {
		return false;
	}
	return true;
}

std::string resolveConfigPath(const std::string &module_path, BackendKind kind, std::string &module_dir,
                              std::string &module_basename)
{
	module_dir = dirName(module_path);
	module_basename = stripLibraryExtension(baseName(module_path));
	std::string override_env = processEnvironmentValue(kind == BackendKind::Siano ? "BONDRIVER_SIANO_CONFIG"
	                                                                                : "BONDRIVER_PX4_CONFIG");
	if (override_env.empty()) {
		override_env = processEnvironmentValue("BONDRIVER_CONFIG");
	}
	if (!override_env.empty()) {
		return normalizePath(override_env);
	}
	return module_dir + "/" + module_basename + ".ini";
}

} // namespace bondriver
