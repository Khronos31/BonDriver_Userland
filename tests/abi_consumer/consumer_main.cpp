// Independent consumer harness: dlopen/LoadLibrary the produced libraries and
// exercise both entrypoints through declarations that are separate copies of
// the EDCB ABI.  Validates structure size, RTTI dynamic_cast, UTF-16 strings
// and enumeration without linking the implementation.
#include "consumer_abi.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <process.h>
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

static_assert(sizeof(BYTE) == 1, "BYTE must be 8-bit");
static_assert(sizeof(WORD) == 2, "WORD must be 16-bit");
static_assert(sizeof(DWORD) == 4, "DWORD must be 32-bit");
static_assert(sizeof(BOOL) == 4, "BOOL must be 32-bit");
static_assert(sizeof(BON16CHAR) == 2, "BON16CHAR must be 16-bit");
static_assert(offsetof(STRUCT_IBONDRIVER2, pF10) == sizeof(STRUCT_IBONDRIVER), "IBonDriver2 struct layout");
static_assert(offsetof(STRUCT_IBONDRIVER2, pF16) == sizeof(STRUCT_IBONDRIVER) + 6 * sizeof(void *),
              "IBonDriver2 function slot order");

using CreateDriverFn = IBonDriver2 *(*)();
using CreateStructFn = const STRUCT_IBONDRIVER *(*)();

namespace {

int g_failures = 0;

void expect(bool cond, const std::string &message)
{
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", message.c_str());
		++g_failures;
	}
}

void setEnv(const char *name, const char *value)
{
#ifdef _WIN32
	_putenv_s(name, value != nullptr ? value : "");
#else
	setenv(name, value, 1);
#endif
}

std::string tempDir()
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
	return std::string(base) + "/bondriver-abi";
}

bool writeFile(const std::string &path, const std::string &text)
{
	FILE *file = std::fopen(path.c_str(), "wb");
	if (file == nullptr) {
		return false;
	}
	std::fwrite(text.data(), 1, text.size(), file);
	std::fclose(file);
	return true;
}

std::string u16ToAscii(const BON16CHAR *text)
{
	std::string out;
	for (; text != nullptr && *text != 0; ++text) {
		out.push_back(static_cast<char>(*text & 0xFF));
	}
	return out;
}

void *libOpen(const std::string &path)
{
#ifdef _WIN32
	return reinterpret_cast<void *>(LoadLibraryA(path.c_str()));
#else
	return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void *libSym(void *handle, const char *name)
{
#ifdef _WIN32
	return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
	return dlsym(handle, name);
#endif
}

void testLibrary(const std::string &lib, const std::string &configPath, bool isSiano)
{
	void *handle = libOpen(lib);
	expect(handle != nullptr, "open " + lib);
	if (handle == nullptr) {
		return;
	}
	auto createDriver = reinterpret_cast<CreateDriverFn>(libSym(handle, "CreateBonDriver"));
	auto createStruct = reinterpret_cast<CreateStructFn>(libSym(handle, "CreateBonStruct"));
	expect(createDriver != nullptr, "CreateBonDriver symbol");
	expect(createStruct != nullptr, "CreateBonStruct symbol");
	setEnv(isSiano ? "BONDRIVER_SIANO_CONFIG" : "BONDRIVER_PX4_CONFIG", configPath.c_str());

	IBonDriver2 *driver2 = createDriver();
	expect(driver2 != nullptr, "CreateBonDriver object");
	if (driver2 != nullptr) {
		IBonDriver *asBase = static_cast<IBonDriver *>(driver2);
		IBonDriver2 *rtii = dynamic_cast<IBonDriver2 *>(asBase);
		expect(rtii == driver2, "RTTI dynamic_cast IBonDriver->IBonDriver2 across DSO");
		expect(u16ToAscii(driver2->GetTunerName()) == "ABI Tuner", "UTF-16 tuner name");
		expect(driver2->EnumTuningSpace(0) != nullptr, "enum space 0");
		expect(driver2->EnumChannelName(0, 0) != nullptr, "enum channel 0");
		driver2->Release();
	}

	const STRUCT_IBONDRIVER *st = createStruct();
	expect(st != nullptr, "CreateBonStruct object");
	if (st != nullptr) {
		const auto *st2 = reinterpret_cast<const STRUCT_IBONDRIVER2 *>(st);
		expect(static_cast<const char *>(st->pEnd) - reinterpret_cast<const char *>(st2) >=
		           static_cast<ptrdiff_t>(sizeof(STRUCT_IBONDRIVER2)),
		       "pEnd covers IBonDriver2 struct");
		expect(u16ToAscii(st2->pF10(st->pCtx)) == "ABI Tuner", "struct UTF-16 tuner name");
		expect(st2->pF12(st->pCtx, 0) != nullptr, "struct enum space");
		expect(st2->pF13(st->pCtx, 0, 0) != nullptr, "struct enum channel");
		st2->st.pF09(st->pCtx);
	}
}

} // namespace

int main(int argc, char **argv)
{
	expect(argc >= 4, "usage: consumer FAKE_CLI SIANO_LIB PX4_LIB");
	if (argc < 4) {
		return 1;
	}
	const std::string fakeCli = argv[1];
	const std::string root = tempDir();
#ifdef _WIN32
	CreateDirectoryA(root.c_str(), nullptr);
	CreateDirectoryA((root + "/runtime").c_str(), nullptr);
#else
	mkdir(root.c_str(), 0755);
	mkdir((root + "/runtime").c_str(), 0755);
#endif
	writeFile(root + "/siano-flowers.tsv",
	          "space_id\tspace_name\tchannel_id\tchannel_name\tsystem\tfrequency_hz\tstream_id\tslot\n"
	          "GR\tGR\tT13\tT13\tisdb-t\t473142857\t-\t-\n");
	writeFile(root + "/px4.tsv",
	          "space_id\tspace_name\tchannel_id\tchannel_name\tsystem\tfrequency_hz\tstream_id\tslot\n"
	          "GR\tGR\tT13\tT13\tisdb-t\t473142857\t-\t-\n"
	          "BS\tBS\tBS01_0\tBS01_0\tisdb-s\t1100000000\t1\t-\n");
	const std::string sianoIni = "[common]\ntuner_name = ABI Tuner\ncli_path = " + fakeCli +
	                             "\nchannel_table = siano-flowers.tsv\ndiagnostics = discard\n[siano]\ndevice = 0\n";
	const std::string px4Ini = "[common]\ntuner_name = ABI Tuner\ncli_path = " + fakeCli +
	                           "\nchannel_table = px4.tsv\ndiagnostics = discard\n[px4]\nruntime_dir = " + root +
	                           "/runtime\ninstance = tok\nreceiver = 0\nsystems = TS\n";
	writeFile(root + "/siano.ini", sianoIni);
	writeFile(root + "/px4.ini", px4Ini);

	testLibrary(argv[2], root + "/siano.ini", true);
	testLibrary(argv[3], root + "/px4.ini", false);

	if (g_failures == 0) {
		std::fprintf(stderr, "PASS: abi_consumer\n");
		return 0;
	}
	std::fprintf(stderr, "FAILED: abi_consumer (%d)\n", g_failures);
	return 1;
}
