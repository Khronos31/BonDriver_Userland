// Loads real copies of the single library under several basenames and checks
// that the shared BonDriver_Userland.ini selects the matching [group.*], table
// and receiver.  Also loads two copies into one process to check that the
// factories/configuration do not interpose.
#include "consumer_abi.h"
#include "test_util.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <sys/stat.h>
#endif

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
	if (value == nullptr) {
		unsetenv(name);
	} else {
		setenv(name, value, 1);
	}
#endif
}

std::string u16ToAscii(const BON16CHAR *text)
{
	std::string out;
	for (; text != nullptr && *text != 0; ++text) {
		out.push_back(static_cast<char>(*text & 0xFF));
	}
	return out;
}

struct Api {
	void *handle = nullptr;
	CreateDriverFn createDriver = nullptr;
	CreateStructFn createStruct = nullptr;
};

Api loadApi(const std::filesystem::path &path)
{
	Api api;
#ifdef _WIN32
	api.handle = reinterpret_cast<void *>(LoadLibraryA(path.string().c_str()));
	if (api.handle != nullptr) {
		api.createDriver = reinterpret_cast<CreateDriverFn>(
		    GetProcAddress(static_cast<HMODULE>(api.handle), "CreateBonDriver"));
		api.createStruct = reinterpret_cast<CreateStructFn>(
		    GetProcAddress(static_cast<HMODULE>(api.handle), "CreateBonStruct"));
	}
#else
	api.handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
	if (api.handle != nullptr) {
		api.createDriver = reinterpret_cast<CreateDriverFn>(dlsym(api.handle, "CreateBonDriver"));
		api.createStruct = reinterpret_cast<CreateStructFn>(dlsym(api.handle, "CreateBonStruct"));
	}
#endif
	return api;
}

std::string receiverOf(const std::string &argv_file)
{
	const std::string text = testutil::readFile(argv_file);
	std::vector<std::string> args;
	size_t start = 0;
	while (start < text.size()) {
		const size_t end = text.find('\n', start);
		args.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
		if (end == std::string::npos) break;
		start = end + 1;
	}
	for (size_t i = 0; i + 1 < args.size(); ++i) {
		if (args[i] == "--receiver") return args[i + 1];
	}
	return std::string();
}

void checkGroup(const std::filesystem::path &lib, const std::string &name, const std::string &tuner,
                const std::string &receiver, const std::string &system, int spaces)
{
	Api api = loadApi(lib);
	expect(api.handle != nullptr, "load group copy " + name);
	expect(api.createDriver != nullptr && api.createStruct != nullptr, "exports in " + name);
	if (api.createDriver == nullptr) {
		return;
	}
	setEnv("FAKE_ARGV_FILE", (std::filesystem::path(lib).parent_path() / (name + ".argv")).string().c_str());
	setEnv("FAKE_MODE", "stream");
	IBonDriver2 *driver = api.createDriver();
	expect(driver != nullptr, "factory for " + name);
	if (driver == nullptr) {
		return;
	}
	expect(u16ToAscii(driver->GetTunerName()) == tuner, "tuner name for " + name);
	int count = 0;
	while (driver->EnumTuningSpace(static_cast<DWORD>(count)) != nullptr) {
		++count;
	}
	expect(count == spaces, "space count for " + name);
	expect(driver->OpenTuner() == TRUE, "open for " + name);
	expect(driver->SetChannel(0, 0) == TRUE, "tune for " + name);
	expect(driver->WaitTsStream(2000) == 0, "TS for " + name);
	const std::string argv_file = (std::filesystem::path(lib).parent_path() / (name + ".argv")).string();
	expect(receiverOf(argv_file) == receiver, "receiver for " + name);
	expect(testutil::readFile(argv_file).find("--system\n" + system + "\n") != std::string::npos,
	       "system for " + name);
	driver->CloseTuner();
	driver->Release();
	setEnv("FAKE_ARGV_FILE", nullptr);
}

} // namespace

int main(int argc, char **argv)
{
	expect(argc >= 3, "usage: test_groups LIB FAKE_CLI");
	if (argc < 3) {
		return testutil::report("test_groups");
	}
	const std::filesystem::path root = std::filesystem::path(testutil::tempRoot()) / "groups";
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	std::filesystem::create_directories(root / "runtime", ec);
	testutil::makeDir((root / "locks").string());
	const std::string fake = argv[2];
	const std::string lock = (root / "locks").string();
	const std::string runtime = (root / "runtime").string();

	testutil::writeFile((root / "t.tsv").string(),
	                    "space_id\tspace_name\tchannel_id\tchannel_name\tsystem\tfrequency_hz\tstream_id\tslot\n"
	                    "GR\tGR\tT13\tT13\tisdb-t\t473142857\t-\t-\n"
	                    "GR\tGR\tT14\tT14\tisdb-t\t479142857\t-\t-\n");
	testutil::writeFile((root / "s.tsv").string(),
	                    "space_id\tspace_name\tchannel_id\tchannel_name\tsystem\tfrequency_hz\tstream_id\tslot\n"
	                    "BS\tBS\tBS01_0\tBS01_0\tisdb-s\t1100000000\t1\t-\n");
	testutil::writeFile((root / "all.tsv").string(),
	                    "space_id\tspace_name\tchannel_id\tchannel_name\tsystem\tfrequency_hz\tstream_id\tslot\n"
	                    "GR\tGR\tT13\tT13\tisdb-t\t473142857\t-\t-\n"
	                    "BS\tBS\tBS01_0\tBS01_0\tisdb-s\t1100000000\t1\t-\n");
	testutil::writeFile(
	    (root / "BonDriver_Userland.ini").string(),
	    "[common]\nlock_dir = " + lock + "\ndiagnostics = discard\n"
	    "[client.main]\ncli_path = " + fake + "\nruntime_dir = " + runtime + "\n"
	    "[group.BonDriver_Userland_T]\ntuner_name = T\nchannel_table = t.tsv\n"
	    "receivers = main:t0:0:T; main:t1:1:T\n"
	    "[group.BonDriver_Userland_S]\ntuner_name = S\nchannel_table = s.tsv\n"
	    "receivers = main:s0:0:S\n"
	    "[group.BonDriver_Userland_TS]\ntuner_name = TS\nchannel_table = all.tsv\n"
	    "receivers = main:h0:0:TS; main:h1:1:TS\n"
	    "[group.BonDriver_Userland]\ntuner_name = All\nchannel_table = t.tsv\n"
	    "receivers = main:t0:0:T\n");

	const std::filesystem::path source = argv[1];
	const std::string extension = source.extension().string();
	const auto copy = [&](const std::string &name) {
		const std::filesystem::path dst = root / (name + extension);
		std::filesystem::copy_file(source, dst, std::filesystem::copy_options::overwrite_existing, ec);
		expect(!ec, "copy " + name);
		return dst;
	};

	setEnv("BONDRIVER_CONFIG", nullptr);
	checkGroup(copy("BonDriver_Userland_T"), "T", "T", "0", "isdb-t", 1);
	checkGroup(copy("BonDriver_Userland_S"), "S", "S", "0", "isdb-s", 1);
	checkGroup(copy("BonDriver_Userland_TS"), "TS", "TS", "0", "isdb-t", 2);
	checkGroup(copy("BonDriver_Userland_T_0"), "T_0", "T", "0", "isdb-t", 1);

	Api unknown = loadApi(copy("BonDriver_Userland_X"));
	expect(unknown.createDriver != nullptr, "unknown-group copy exports");
	if (unknown.createDriver != nullptr) {
		expect(unknown.createDriver() == nullptr, "unknown group is rejected");
	}
	expect(unknown.createStruct != nullptr && unknown.createStruct() == nullptr,
	       "unknown group struct is rejected");

	// Keep two module copies loaded together.  Each must keep its own group.
	Api t = loadApi(root / ("BonDriver_Userland_T" + extension));
	Api s = loadApi(root / ("BonDriver_Userland_S" + extension));
	IBonDriver2 *td = t.createDriver != nullptr ? t.createDriver() : nullptr;
	IBonDriver2 *sd = s.createDriver != nullptr ? s.createDriver() : nullptr;
	expect(td != nullptr && sd != nullptr, "two module copies create in one process");
	if (td != nullptr && sd != nullptr) {
		expect(u16ToAscii(td->GetTunerName()) == "T", "T copy keeps its group");
		expect(u16ToAscii(sd->GetTunerName()) == "S", "S copy keeps its group");
	}
	if (td != nullptr) td->Release();
	if (sd != nullptr) sd->Release();

	// Different groups/module copies that name the same physical receiver must
	// still be mutually exclusive.  The lease key is independent of the group.
	const std::filesystem::path all_lib = copy("BonDriver_Userland");
	Api t_copy = loadApi(root / ("BonDriver_Userland_T" + extension));
	Api all_copy = loadApi(all_lib);
	if (t_copy.createDriver != nullptr && all_copy.createDriver != nullptr) {
		IBonDriver2 *first = t_copy.createDriver();
		IBonDriver2 *second = all_copy.createDriver();
		expect(first != nullptr && second != nullptr, "cross-group drivers created");
		if (first != nullptr && second != nullptr) {
			expect(first->OpenTuner() == TRUE, "T group acquires the shared receiver");
			expect(second->OpenTuner() == FALSE,
			       "another group cannot acquire the same physical receiver");
			first->CloseTuner();
			expect(second->OpenTuner() == TRUE, "closing the T group releases the shared receiver");
			second->CloseTuner();
		}
		if (first != nullptr) first->Release();
		if (second != nullptr) second->Release();
	} else {
		expect(false, "cross-group drivers could not be loaded");
	}

	// The EDCB STRUCT_IBONDRIVER2 entry must expose the same selected group.
	Api ts = loadApi(root / ("BonDriver_Userland_TS" + extension));
	if (ts.createStruct != nullptr) {
		const STRUCT_IBONDRIVER *st = ts.createStruct();
		expect(st != nullptr, "struct entry for TS copy");
		if (st != nullptr) {
			const auto *st2 = reinterpret_cast<const STRUCT_IBONDRIVER2 *>(st);
			expect(u16ToAscii(st2->pF10(st->pCtx)) == "TS", "struct tuner name for TS copy");
			st2->st.pF09(st->pCtx);
		}
	}

	if (g_failures == 0) {
		std::fprintf(stderr, "PASS: test_groups\n");
		return 0;
	}
	std::fprintf(stderr, "FAILED: test_groups (%d)\n", g_failures);
	return 1;
}
