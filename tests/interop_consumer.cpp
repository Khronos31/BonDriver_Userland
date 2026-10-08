// Loads BOTH shared libraries into one process with RTLD_GLOBAL and verifies
// that internal symbols do not interpose: each factory must keep its own
// backend and configuration.  This guards against exported helper/class names
// being coalesced between the two DSOs.
#include "consumer_abi.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#  include <process.h>
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

using CreateDriverFn = IBonDriver2 *(*)();

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
	return std::string(base) + "/bondriver-interop";
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
	return dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
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

} // namespace

int main(int argc, char **argv)
{
	expect(argc >= 4, "usage: interop FAKE_CLI SIANO_LIB PX4_LIB");
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
	writeFile(root + "/siano.ini",
	          "[common]\ntuner_name = Siano Tuner\ncli_path = " + fakeCli +
	              "\nchannel_table = siano-flowers.tsv\nlock_dir = " + root + "/siano-locks\ndiagnostics = discard\n[siano]\ndevice = 0\n");
	writeFile(root + "/px4.ini",
	          "[common]\ntuner_name = PX4 Tuner\ncli_path = " + fakeCli +
	              "\nchannel_table = px4.tsv\nlock_dir = " + root + "/px4-locks\ndiagnostics = discard\n[px4]\nruntime_dir = " +
	              root + "/runtime\ninstance = tok\nreceiver = 0\nsystems = TS\n");

	void *siano = libOpen(argv[2]);
	void *px4 = libOpen(argv[3]);
	expect(siano != nullptr, "open Siano library");
	expect(px4 != nullptr, "open PX4 library");
	if (siano == nullptr || px4 == nullptr) {
		return 1;
	}
	auto sianoCreate = reinterpret_cast<CreateDriverFn>(libSym(siano, "CreateBonDriver"));
	auto px4Create = reinterpret_cast<CreateDriverFn>(libSym(px4, "CreateBonDriver"));
	expect(sianoCreate != nullptr && px4Create != nullptr, "both factories resolved");
	if (sianoCreate == nullptr || px4Create == nullptr) {
		return 1;
	}

	setEnv("BONDRIVER_SIANO_CONFIG", (root + "/siano.ini").c_str());
	setEnv("BONDRIVER_PX4_CONFIG", (root + "/px4.ini").c_str());

	IBonDriver2 *s = sianoCreate();
	IBonDriver2 *p = px4Create();
	expect(s != nullptr && p != nullptr, "both factories create objects");
	if (s != nullptr) {
		expect(u16ToAscii(s->GetTunerName()) == "Siano Tuner", "Siano uses its own config/tuner name");
		expect(s->EnumTuningSpace(0) != nullptr, "Siano space 0 present");
		expect(s->EnumTuningSpace(1) == nullptr, "Siano has no satellite space");
		s->Release();
	}
	if (p != nullptr) {
		expect(u16ToAscii(p->GetTunerName()) == "PX4 Tuner", "PX4 uses its own config/tuner name");
		expect(p->EnumTuningSpace(1) != nullptr, "PX4 exposes the BS space");
		p->Release();
	}

	if (g_failures == 0) {
		std::fprintf(stderr, "PASS: interop_consumer\n");
		return 0;
	}
	std::fprintf(stderr, "FAILED: interop_consumer (%d)\n", g_failures);
	return 1;
}
