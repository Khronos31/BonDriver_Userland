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
		return name.substr(0, dot);
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

bool isAsciiAlnum(unsigned char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Shared Core/Siano instance token rules: 1..64 ASCII bytes, start with an
// alphanumeric, then alphanumeric/_/./-, no trailing dot, no reserved names.
bool validInstanceToken(const std::string &text)
{
	if (text.empty() || text.size() > 64) {
		return false;
	}
	if (!isAsciiAlnum(static_cast<unsigned char>(text[0]))) {
		return false;
	}
	for (size_t i = 1; i < text.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		if (!(isAsciiAlnum(c) || c == '_' || c == '.' || c == '-')) {
			return false;
		}
	}
	if (text.back() == '.') {
		return false;
	}
	size_t stem = 0;
	while (stem < text.size() && text[stem] != '.') {
		++stem;
	}
	std::string folded = text.substr(0, stem);
	for (char &c : folded) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	const bool reserved_word =
	    folded == "con" || folded == "prn" || folded == "aux" || folded == "nul";
	const bool reserved_device =
	    folded.size() == 4 && (folded.compare(0, 3, "com") == 0 || folded.compare(0, 3, "lpt") == 0) &&
	    folded[3] >= '1' && folded[3] <= '9';
	return !reserved_word && !reserved_device;
}

struct IniFile {
	std::map<std::string, std::map<std::string, std::string>> sections;
};

bool parseIni(const std::string &text, IniFile &out, std::string &error)
{
	std::istringstream stream(text);
	std::string line;
	std::string section = "";
	std::set<std::string> seen_sections;
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
			if (!seen_sections.insert(section).second) {
				error = "line " + std::to_string(line_no) + ": duplicate section '" + section + "'";
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
		if (section.empty()) {
			error = "line " + std::to_string(line_no) + ": key outside a section";
			return false;
		}
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

bool loadCommon(const IniFile &ini, const std::string &dir, CommonConfig &out, std::string &error)
{
	const std::map<std::string, std::string> empty;
	const auto *sec = findSection(ini, "common");
	const std::map<std::string, std::string> &table = sec ? *sec : empty;
	if (!checkAllowedKeys(table,
	                      {"lock_dir", "tune_timeout_ms", "stop_timeout_ms", "kill_timeout_ms", "buffer_limit_bytes",
	                       "diagnostics"},
	                      "common", error)) {
		return false;
	}

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

	const std::string lock_dir = getOr(table, "lock_dir", "");
	if (lock_dir.empty()) {
#ifdef _WIN32
		const std::string program_data = processEnvironmentValue("ProgramData");
		out.lock_dir = !program_data.empty() ? program_data + "/BonDriver_Userland"
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
		out.lock_dir = normalizePath(joinPath(dir, lock_dir));
	}
	return true;
}

bool loadClients(const IniFile &ini, const std::string &dir, std::vector<ClientConfig> &out, std::string &error)
{
	for (const auto &section : ini.sections) {
		if (section.first.rfind("client.", 0) != 0) {
			continue;
		}
		const std::string name = section.first.substr(7);
		if (name.empty()) {
			error = "empty client label in [" + section.first + "]";
			return false;
		}
		if (name.find(':') != std::string::npos || name.find(';') != std::string::npos) {
			error = "client label must not contain ':' or ';': [" + section.first + "]";
			return false;
		}
		if (!checkAllowedKeys(section.second, {"cli_path", "runtime_dir"}, section.first, error)) {
			return false;
		}
		ClientConfig client;
		client.name = name;
		client.cli_path = getOr(section.second, "cli_path", "");
		client.runtime_dir = getOr(section.second, "runtime_dir", "");
		if (client.cli_path.empty()) {
			error = "config [" + section.first + "] requires cli_path";
			return false;
		}
		if (client.runtime_dir.empty()) {
			error = "config [" + section.first + "] requires runtime_dir";
			return false;
		}
		client.cli_path = joinPath(dir, client.cli_path);
		client.runtime_dir = normalizePath(joinPath(dir, client.runtime_dir));
		out.push_back(std::move(client));
	}
	if (out.empty()) {
		error = "config requires at least one [client.NAME] section";
		return false;
	}
	return true;
}

bool parseReceiverEntry(const std::string &entry, ReceiverConfig &out, std::string &error)
{
	// client:instance:receiver:systems
	const size_t c1 = entry.find(':');
	if (c1 == std::string::npos) {
		error = "invalid receiver '" + entry + "' (expected client:instance:receiver:systems)";
		return false;
	}
	const size_t c2 = entry.find(':', c1 + 1);
	if (c2 == std::string::npos) {
		error = "invalid receiver '" + entry + "' (expected client:instance:receiver:systems)";
		return false;
	}
	const size_t c3 = entry.find(':', c2 + 1);
	if (c3 == std::string::npos) {
		error = "invalid receiver '" + entry + "' (expected client:instance:receiver:systems)";
		return false;
	}
	out.client = trim(entry.substr(0, c1));
	out.instance = trim(entry.substr(c1 + 1, c2 - c1 - 1));
	const std::string receiver = trim(entry.substr(c2 + 1, c3 - c2 - 1));
	const std::string systems = trim(entry.substr(c3 + 1));
	if (out.client.empty() || out.instance.empty() || receiver.empty() || systems.empty()) {
		error = "invalid receiver '" + entry + "'";
		return false;
	}
	if (!validInstanceToken(out.instance)) {
		error = "invalid instance '" + out.instance + "' in receiver '" + entry + "'";
		return false;
	}
	uint64_t rv = 0;
	if (!parseUnsigned(receiver, 0, 7, rv)) {
		error = "receiver out of range 0..7 in '" + entry + "'";
		return false;
	}
	out.receiver = static_cast<int>(rv);
	for (char c : systems) {
		if (c == 'T' || c == 't') {
			out.supports_t = true;
		} else if (c == 'S' || c == 's') {
			out.supports_s = true;
		} else {
			error = "invalid system character in receiver '" + entry + "'";
			return false;
		}
	}
	if (!out.supports_t && !out.supports_s) {
		error = "receiver '" + entry + "' supports no system";
		return false;
	}
	out.hybrid = out.supports_t && out.supports_s;
	return true;
}

bool loadGroup(const IniFile &ini, const std::string &dir, const std::vector<ClientConfig> &clients,
               GroupConfig &out, std::string &error)
{
	const std::string *group_section = nullptr;
	for (const auto &section : ini.sections) {
		if (section.first.rfind("group.", 0) != 0) {
			continue;
		}
		if (group_section != nullptr) {
			error = "exactly one [group.NAME] is supported; found '" + *group_section + "' and '" + section.first + "'";
			return false;
		}
		group_section = &section.first;
	}
	if (group_section == nullptr) {
		error = "config requires exactly one [group.NAME] section";
		return false;
	}
	const std::string &name = *group_section;
	out.name = name.substr(6);
	if (out.name.empty()) {
		error = "empty group label in [" + name + "]";
		return false;
	}
	const auto &table = ini.sections.at(name);
	if (!checkAllowedKeys(table, {"tuner_name", "channel_table", "receivers"}, name, error)) {
		return false;
	}
	out.tuner_name = getOr(table, "tuner_name", "");
	const std::string channel_table = getOr(table, "channel_table", "");
	const std::string receivers = getOr(table, "receivers", "");
	if (out.tuner_name.empty()) {
		error = "config [" + name + "] requires tuner_name";
		return false;
	}
	if (channel_table.empty()) {
		error = "config [" + name + "] requires channel_table";
		return false;
	}
	out.channel_table = joinPath(dir, channel_table);
	if (receivers.empty()) {
		error = "config [" + name + "] requires receivers";
		return false;
	}
	for (const std::string &entry : splitList(receivers, ';')) {
		ReceiverConfig receiver;
		if (!parseReceiverEntry(entry, receiver, error)) {
			return false;
		}
		bool known = false;
		for (const ClientConfig &c : clients) {
			if (c.name == receiver.client) {
				known = true;
				break;
			}
		}
		if (!known) {
			error = "receiver '" + entry + "' references unknown client '" + receiver.client + "'";
			return false;
		}
		out.receivers.push_back(receiver);
	}
	if (out.receivers.empty()) {
		error = "config [" + name + "] receivers is empty";
		return false;
	}
	std::set<std::string> seen;
	for (const ReceiverConfig &r : out.receivers) {
		const std::string id = r.client + "/" + r.instance + "/" + std::to_string(r.receiver);
		if (!seen.insert(id).second) {
			error = "duplicate receiver '" + id + "' in [" + name + "]";
			return false;
		}
	}
	return true;
}

bool validateGroup(const Config &config, std::string &error)
{
	bool has_t = false;
	bool has_s = false;
	for (const ReceiverConfig &r : config.group.receivers) {
		has_t = has_t || r.supports_t;
		has_s = has_s || r.supports_s;
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

const ClientConfig *Config::findClient(const std::string &name) const
{
	for (const ClientConfig &c : clients) {
		if (c.name == name) {
			return &c;
		}
	}
	return nullptr;
}

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

bool canonicalizeRuntimeDir(const std::string &path, std::string &out, std::string &error)
{
	return canonicalizeExistingDirectory(path, out, error);
}

std::string receiverLeaseKey(const std::string &canonical_runtime_dir, const std::string &instance, int receiver)
{
	// Length prefixes keep the key unambiguous even if a directory name
	// contains the separator characters.
	return "userland:" + std::to_string(canonical_runtime_dir.size()) + ":" + canonical_runtime_dir + "|" +
	       std::to_string(instance.size()) + ":" + instance + "|" + std::to_string(receiver);
}

bool parseChannelTable(const std::string &text, std::vector<Space> &out, std::string &error)
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

bool loadConfig(const std::string &module_path, const std::string &override_path, Config &out, std::string &error)
{
	std::string module_dir;
	std::string module_basename;
	std::string path = override_path;
	if (path.empty()) {
		path = resolveConfigPath(module_path, module_dir, module_basename);
	} else {
		module_dir = dirName(module_path);
		module_basename = stripLibraryExtension(baseName(module_path));
	}
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
	for (const auto &section : ini.sections) {
		const std::string &name = section.first;
		if (name == "common" || name.rfind("client.", 0) == 0 || name.rfind("group.", 0) == 0) {
			continue;
		}
		error = "unknown section [" + name + "]";
		return false;
	}
	if (!loadCommon(ini, module_dir, out.common, error)) {
		return false;
	}
	if (!loadClients(ini, module_dir, out.clients, error)) {
		return false;
	}
	if (!loadGroup(ini, module_dir, out.clients, out.group, error)) {
		return false;
	}
	if (!out.common.diagnostics.empty() && out.common.diagnostics.rfind("file:", 0) == 0) {
		const std::string file = out.common.diagnostics.substr(5);
		if (file.empty()) {
			error = "config [common] diagnostics file path is empty";
			return false;
		}
		out.common.diagnostics = "file:" + joinPath(module_dir, file);
	}
	std::string table_text;
	if (!readFile(out.group.channel_table, table_text, error)) {
		return false;
	}
	if (!parseChannelTable(table_text, out.spaces, error)) {
		return false;
	}
	if (!validateGroup(out, error)) {
		return false;
	}
	return true;
}

std::string resolveConfigPath(const std::string &module_path, std::string &module_dir,
                              std::string &module_basename)
{
	module_dir = dirName(module_path);
	module_basename = stripLibraryExtension(baseName(module_path));
	const std::string override_env = processEnvironmentValue("BONDRIVER_CONFIG");
	if (!override_env.empty()) {
		return normalizePath(override_env);
	}
	return module_dir + "/" + module_basename + ".ini";
}

} // namespace bondriver
