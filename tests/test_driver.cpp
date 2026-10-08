// End-to-end driver behaviour through the exported ABI.  The library is
// dlopen'd exactly as a consumer would; only the fake CLI child is used.
#include "bondriver/Abi.h"
#include "bondriver/TsBuffer.h"
#include "test_util.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <process.h>
#  include <windows.h>
#else
#  include <csignal>
#  include <dlfcn.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

using CreateDriverFn = IBonDriver2 *(*)();
using CreateStructFn = const STRUCT_IBONDRIVER *(*)();

namespace {

struct Api {
	void *handle = nullptr;
	CreateDriverFn createDriver = nullptr;
	CreateStructFn createStruct = nullptr;
};

long processId()
{
#ifdef _WIN32
	return static_cast<long>(_getpid());
#else
	return static_cast<long>(::getpid());
#endif
}

std::string g_root;
std::string g_fakeCli;
std::string g_selfExe;
std::string g_lib;
std::string g_backend;
bool g_siano = true;
long g_bufferLimit = 8388608;

#ifdef _WIN32
std::wstring toWideLocal(const std::string &text)
{
	if (text.empty()) {
		return std::wstring();
	}
	const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
	std::wstring out(static_cast<size_t>(size), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size);
	return out;
}
#endif

std::string currentExePath()
{
#ifdef _WIN32
	wchar_t buf[MAX_PATH];
	const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		return std::string();
	}
	const int size = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
	std::string out(static_cast<size_t>(size), '\0');
	WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), &out[0], size, nullptr, nullptr);
	return out;
#else
	char buf[4096];
	const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
	if (n <= 0) {
		return std::string();
	}
	buf[n] = '\0';
	return std::string(buf);
#endif
}

std::string configTextForRuntime(const std::string &runtime_dir)
{
	return "[common]\n"
	       "tuner_name = Test Tuner\n"
	       "cli_path = " +
	       g_fakeCli + "\n"
	                   "channel_table = channels.tsv\n"
	                   "lock_dir = " +
	       g_root + "/locks\n"
	                "tune_timeout_ms = 800\n"
	                "stop_timeout_ms = 700\n"
	                "kill_timeout_ms = 400\n"
	                "buffer_limit_bytes = " +
	       std::to_string(g_bufferLimit) +
	       "\n"
	       "diagnostics = discard\n"
	       "[px4]\nruntime_dir = " +
	       runtime_dir + "\ninstance = tok\nreceiver = 0\nsystems = TS\n";
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

std::string configText()
{
	std::string common = "[common]\n"
	                     "tuner_name = Test Tuner\n"
	                     "cli_path = " +
	                     g_fakeCli + "\n"
	                     "channel_table = channels.tsv\n"
	                     "lock_dir = " +
	                     g_root + "/locks\n"
	                     "tune_timeout_ms = 800\n"
	                     "stop_timeout_ms = 700\n"
	                     "kill_timeout_ms = 400\n"
	                     "buffer_limit_bytes = " +
	                     std::to_string(g_bufferLimit) +
	                     "\n"
	                     "diagnostics = discard\n";
	if (g_siano) {
		return common + "[siano]\ndevice = 0\n";
	}
	return common + "[px4]\nruntime_dir = " + g_root + "/runtime\ninstance = tok\nreceiver = 0\nsystems = TS\n";
}

std::string tableText()
{
	std::string text =
	    "space_id\tspace_name\tchannel_id\tchannel_name\tsystem\tfrequency_hz\tstream_id\tslot\n"
	    "GR\t地上波\tT13\tT13\tisdb-t\t473142857\t-\t-\n"
	    "GR\t地上波\tT14\tT14\tisdb-t\t479142857\t-\t-\n";
	if (!g_siano) {
		text += "BS\tBS\tBS01_0\tBS01_0\tisdb-s\t1100000000\t1\t-\n";
	}
	return text;
}

Api loadApi(const std::string &lib)
{
	Api api;
#ifdef _WIN32
	api.handle = reinterpret_cast<void *>(LoadLibraryA(lib.c_str()));
	if (api.handle != nullptr) {
		api.createDriver = reinterpret_cast<CreateDriverFn>(
		    GetProcAddress(static_cast<HMODULE>(api.handle), "CreateBonDriver"));
		api.createStruct = reinterpret_cast<CreateStructFn>(
		    GetProcAddress(static_cast<HMODULE>(api.handle), "CreateBonStruct"));
	}
#else
	api.handle = dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (api.handle != nullptr) {
		api.createDriver = reinterpret_cast<CreateDriverFn>(dlsym(api.handle, "CreateBonDriver"));
		api.createStruct = reinterpret_cast<CreateStructFn>(dlsym(api.handle, "CreateBonStruct"));
	}
#endif
	return api;
}

bool pointerHasTag(const BYTE *data, DWORD size, const std::string &tag)
{
	if (size < tag.size()) {
		return false;
	}
	for (DWORD i = 0; i + tag.size() <= size; ++i) {
		if (std::memcmp(data + i, tag.data(), tag.size()) == 0) {
			return true;
		}
	}
	return false;
}

const std::string kTag0 = "473142857";
const std::string kTag1 = "479142857";

int scenarioBasic(const Api &api)
{
	IBonDriver2 *driver = api.createDriver();
	testutil::expect(driver != nullptr, "CreateBonDriver returns object");
	if (driver == nullptr) {
		return 1;
	}
	int rc = 0;
	testutil::expect(driver->OpenTuner() == TRUE, "OpenTuner succeeds");
	testutil::expect(driver->IsTunerOpening() == TRUE, "IsTunerOpening true");
	testutil::expect(driver->GetTunerName() != nullptr, "tuner name present");
	testutil::expect(driver->EnumTuningSpace(0) != nullptr, "space 0 present");
	testutil::expect(driver->EnumTuningSpace(99) == nullptr, "unknown space null");
	testutil::expect(driver->EnumChannelName(0, 0) != nullptr, "channel 0 present");
	testutil::expect(driver->EnumChannelName(0, 99) == nullptr, "unknown channel null");
	testutil::expect(driver->GetSignalLevel() == 0.0f, "CNR unknown reported as 0");
	testutil::expect(driver->GetCurSpace() == BONDRIVER_SPACE_INVALID, "no current space before tune");
	testutil::expect(driver->GetCurChannel() == BONDRIVER_CHANNEL_INVALID, "no current channel before tune");

	setEnv("FAKE_MODE", "stream");
	setEnv("FAKE_TAG", kTag0.c_str());
	testutil::expect(driver->SetChannel(0, 0) == TRUE, "tune channel 0");
	testutil::expect(driver->WaitTsStream(2000) == 0, "data becomes available");
	testutil::expect(driver->GetReadyCount() > 0, "ready buffer count positive");
	testutil::expect(driver->GetCurSpace() == 0, "current space after tune");
	testutil::expect(driver->GetCurChannel() == 0, "current channel after tune");

	{
		BYTE *data = nullptr;
		DWORD size = 0;
		DWORD remain = 0;
		testutil::expect(driver->GetTsStream(&data, &size, &remain) == TRUE, "pointer get succeeds");
		testutil::expect(data != nullptr && size % 188 == 0 && size > 0, "pointer get is packet aligned");
		testutil::expect(size <= bondriver::kMaxGetTsStreamBytes, "pointer block within public maximum");
	}
	{
		std::vector<BYTE> dst(bondriver::kMaxGetTsStreamBytes + 64, 0xEE);
		DWORD size = 0; // output-only contract: never read as capacity
		DWORD remain = 0;
		testutil::expect(driver->GetTsStream(dst.data(), &size, &remain) == TRUE || size == 0, "copy get returns");
		testutil::expect(size % 188 == 0, "copy get packet aligned");
		bool canary = true;
		for (size_t i = size; i < dst.size(); ++i) {
			if (dst[i] != 0xEE) {
				canary = false;
				break;
			}
		}
		testutil::expect(canary, "copy get respects reported size");
	}

	setEnv("FAKE_MODE", "stream");
	setEnv("FAKE_TAG", kTag1.c_str());
	testutil::expect(driver->SetChannel(0, 1) == TRUE, "retune channel 1");
	testutil::expect(driver->WaitTsStream(2000) == 0, "retuned data available");
	{
		BYTE *data = nullptr;
		DWORD size = 0;
		DWORD remain = 0;
		driver->GetTsStream(&data, &size, &remain);
		testutil::expect(!pointerHasTag(data, size, kTag0), "old channel TS not carried over");
	}

	setEnv("FAKE_TAG", kTag1.c_str());
	driver->PurgeTsStream();
	testutil::expect(driver->WaitTsStream(2000) == 0, "purge keeps stream alive");
	{
		BYTE *data = nullptr;
		DWORD size = 0;
		DWORD remain = 0;
		testutil::expect(driver->GetTsStream(&data, &size, &remain) == TRUE, "post-purge get");
		testutil::expect(size % 188 == 0, "post-purge aligned");
	}

	driver->CloseTuner();
	testutil::expect(driver->IsTunerOpening() == FALSE, "closed");
	testutil::expect(driver->GetCurSpace() == BONDRIVER_SPACE_INVALID, "invalid space after close");
	testutil::expect(driver->GetCurChannel() == BONDRIVER_CHANNEL_INVALID, "invalid channel after close");
	testutil::expect(driver->OpenTuner() == TRUE, "reopen succeeds");
	testutil::expect(driver->GetReadyCount() == 0, "reopen starts with empty TS queue");
	{
		BYTE *data = nullptr;
		DWORD size = 0;
		DWORD remain = 0;
		testutil::expect(driver->GetTsStream(&data, &size, &remain) == FALSE && size == 0,
		                 "reopen has no stale pointer TS");
		std::vector<BYTE> dst(bondriver::kMaxGetTsStreamBytes, 0);
		size = 0;
		remain = 0;
		testutil::expect(driver->GetTsStream(dst.data(), &size, &remain) == FALSE && size == 0,
		                 "reopen has no stale copied TS");
	}
	testutil::expect(driver->SetChannel(0, 0) == TRUE, "reopen tune");
	driver->Release();
	return rc;
}

int scenarioStartupTimeout(const Api &api)
{
	IBonDriver2 *driver = api.createDriver();
	if (driver == nullptr) {
		return 1;
	}
	testutil::expect(driver->OpenTuner() == TRUE, "open");
	setEnv("FAKE_MODE", "nooutput");
	testutil::expect(driver->SetChannel(0, 0) == FALSE, "tune fails on startup timeout");
	testutil::expect(driver->GetSignalLevel() == 0.0f, "no fake CNR on failure");
	testutil::expect(driver->WaitTsStream(100) == 0xFFFFFFFFu, "wait fails after tune failure");
	testutil::expect(driver->GetCurSpace() == BONDRIVER_SPACE_INVALID, "failed tune keeps no space");
	testutil::expect(driver->GetCurChannel() == BONDRIVER_CHANNEL_INVALID, "failed tune keeps no channel");
	driver->Release();
	return 0;
}

int scenarioDeadChild(const Api &api)
{
	IBonDriver2 *driver = api.createDriver();
	if (driver == nullptr) {
		return 1;
	}
	driver->OpenTuner();
	setEnv("FAKE_MODE", "dead");
	testutil::expect(driver->SetChannel(0, 0) == TRUE, "dead child emits initial data");
	DWORD last = 0;
	for (int i = 0; i < 40; ++i) {
		last = driver->WaitTsStream(200);
		if (last == 0xFFFFFFFFu) {
			break;
		}
		if (last == 0) {
			BYTE *data = nullptr;
			DWORD size = 0;
			DWORD remain = 0;
			driver->GetTsStream(&data, &size, &remain);
		}
	}
	testutil::expect(last == 0xFFFFFFFFu, "wait fails after child death");
	testutil::expect(driver->GetCurSpace() == BONDRIVER_SPACE_INVALID, "current space invalid after child EOF");
	testutil::expect(driver->GetCurChannel() == BONDRIVER_CHANNEL_INVALID, "current channel invalid after child EOF");
	setEnv("FAKE_MODE", "stream");
	setEnv("FAKE_TAG", "dead-child-recovery");
	testutil::expect(driver->SetChannel(0, 0) == TRUE, "same channel can restart after child EOF");
	testutil::expect(driver->WaitTsStream(2000) == 0, "restarted same channel produces TS");
	{
		BYTE *data = nullptr;
		DWORD size = 0;
		DWORD remain = 0;
		testutil::expect(driver->GetTsStream(&data, &size, &remain) == TRUE && size > 0,
		                 "restarted same channel exposes fresh TS");
	}
	driver->Release();
	return 0;
}

int scenarioLease(const Api &api)
{
	IBonDriver2 *parent = api.createDriver();
	if (parent == nullptr) {
		return 1;
	}
#ifndef _WIN32
	int sync[2];
	if (::pipe(sync) != 0) {
		return 1;
	}
	const pid_t child = ::fork();
	if (child == 0) {
		::close(sync[0]);
		IBonDriver2 *holder = api.createDriver();
		const char ok = (holder != nullptr && holder->OpenTuner() == TRUE) ? 1 : 0;
		(void)!::write(sync[1], &ok, 1);
		for (;;) {
			::pause();
		}
	}
	::close(sync[1]);
	char child_ok = 0;
	if (::read(sync[0], &child_ok, 1) != 1) {
		child_ok = 0;
	}
	::close(sync[0]);
	testutil::expect(child_ok == 1, "child process acquires the receiver lease");
	testutil::expect(parent->OpenTuner() == FALSE, "second process cannot double-claim receiver");

	::kill(child, SIGKILL);
	int status = 0;
	::waitpid(child, &status, 0);
	testutil::expect(parent->OpenTuner() == TRUE, "lease recovers after child SIGKILL");
#else
	// Real Windows process contention: run this binary again as a lease holder.
	const std::string ready = g_root + "/lease-ready";
	::DeleteFileA(ready.c_str());
	setEnv("FAKE_LEASE_READY", ready.c_str());
	const std::wstring exe = toWideLocal(g_selfExe);
	const std::wstring lib = toWideLocal(g_lib);
	const std::wstring backend = toWideLocal(g_backend);
	const std::wstring fake = toWideLocal(g_fakeCli);
	std::wstring cmd = L"\"" + exe + L"\" \"" + lib + L"\" " + backend + L" \"" + fake + L"\" lease-holder";
	std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
	cmdbuf.push_back(L'\0');
	STARTUPINFOW si{};
	si.cb = sizeof si;
	PROCESS_INFORMATION pi{};
	const BOOL created = CreateProcessW(nullptr, cmdbuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
	                                    nullptr, &si, &pi);
	testutil::expect(created != FALSE, "spawn Windows lease holder");
	if (!created) {
		parent->Release();
		return 1;
	}
	CloseHandle(pi.hThread);
	bool holder_ready = false;
	for (int i = 0; i < 100 && !holder_ready; ++i) {
		holder_ready = ::GetFileAttributesA(ready.c_str()) != INVALID_FILE_ATTRIBUTES;
		if (!holder_ready) {
			Sleep(30);
		}
	}
	testutil::expect(holder_ready, "Windows lease holder acquired the receiver");
	testutil::expect(parent->OpenTuner() == FALSE, "second Windows process cannot double-claim receiver");
	TerminateProcess(pi.hProcess, 1);
	WaitForSingleObject(pi.hProcess, 3000);
	CloseHandle(pi.hProcess);
	testutil::expect(parent->OpenTuner() == TRUE, "lease recovers after Windows holder termination");
#endif
	parent->Release();
	return 0;
}

int scenarioRuntimeAlias(const Api &api)
{
#ifdef _WIN32
	const std::string realDir = g_root + "/runtime-real";
	const std::string linkDir = g_root + "/runtime-junction";
	testutil::makeDir(realDir);
	const std::wstring wlink = toWideLocal(linkDir);
	const std::wstring wreal = toWideLocal(realDir);
	const std::wstring cmd = L"cmd.exe /d /c mklink /J \"" + wlink + L"\" \"" + wreal + L"\"";
	std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
	cmdline.push_back(L'\0');
	STARTUPINFOW csi{};
	csi.cb = sizeof csi;
	PROCESS_INFORMATION cpi{};
	const BOOL junction_started = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
	                                            CREATE_NO_WINDOW, nullptr, nullptr, &csi, &cpi);
	testutil::expect(junction_started != FALSE, "start junction fixture command");
	if (!junction_started) return 1;
	CloseHandle(cpi.hThread);
	WaitForSingleObject(cpi.hProcess, 5000);
	DWORD junction_exit = 1;
	GetExitCodeProcess(cpi.hProcess, &junction_exit);
	CloseHandle(cpi.hProcess);
	testutil::expect(junction_exit == 0, "create runtime directory junction");
	if (junction_exit != 0) return 1;

	testutil::writeFile(g_root + "/real.ini", configTextForRuntime(realDir));
	testutil::writeFile(g_root + "/link.ini", configTextForRuntime(linkDir));
	const std::string ready = g_root + "/alias-lease-ready";
	DeleteFileW(toWideLocal(ready).c_str());
	setEnv("BONDRIVER_PX4_CONFIG", (g_root + "/link.ini").c_str());
	setEnv("FAKE_LEASE_READY", ready.c_str());
	const std::wstring exe = toWideLocal(g_selfExe);
	const std::wstring lib = toWideLocal(g_lib);
	const std::wstring fake = toWideLocal(g_fakeCli);
	std::wstring holder_cmd = L"\"" + exe + L"\" \"" + lib + L"\" px4 \"" + fake + L"\" lease-holder";
	std::vector<wchar_t> holder_line(holder_cmd.begin(), holder_cmd.end());
	holder_line.push_back(L'\0');
	STARTUPINFOW hsi{};
	hsi.cb = sizeof hsi;
	PROCESS_INFORMATION hpi{};
	const BOOL holder_started = CreateProcessW(nullptr, holder_line.data(), nullptr, nullptr, FALSE,
	                                           CREATE_NO_WINDOW, nullptr, nullptr, &hsi, &hpi);
	testutil::expect(holder_started != FALSE, "spawn junction lease holder");
	if (!holder_started) return 1;
	CloseHandle(hpi.hThread);
	bool ready_seen = false;
	for (int i = 0; i < 100 && !ready_seen; ++i) {
		ready_seen = GetFileAttributesW(toWideLocal(ready).c_str()) != INVALID_FILE_ATTRIBUTES;
		if (!ready_seen) Sleep(30);
	}
	testutil::expect(ready_seen, "junction alias holder acquired receiver");
	setEnv("BONDRIVER_PX4_CONFIG", (g_root + "/real.ini").c_str());
	IBonDriver2 *parent = api.createDriver();
	testutil::expect(parent != nullptr, "canonical runtime config loads");
	if (parent != nullptr) {
		testutil::expect(parent->OpenTuner() == FALSE, "canonical runtime rejects junction alias holder");
	}
	TerminateProcess(hpi.hProcess, 1);
	WaitForSingleObject(hpi.hProcess, 3000);
	CloseHandle(hpi.hProcess);
	if (parent != nullptr) {
		testutil::expect(parent->OpenTuner() == TRUE, "canonical runtime acquires after junction holder exits");
		parent->Release();
	}
	DeleteFileW(toWideLocal(ready).c_str());
	return parent != nullptr ? 0 : 1;
#else
	const std::string realDir = g_root + "/runtime";
	const std::string linkDir = g_root + "/runtime-link";
	::unlink(linkDir.c_str());
	::symlink(realDir.c_str(), linkDir.c_str());
	testutil::writeFile(g_root + "/real.ini", configTextForRuntime(realDir));
	testutil::writeFile(g_root + "/link.ini", configTextForRuntime(linkDir));

	int sync[2];
	if (::pipe(sync) != 0) {
		return 1;
	}
	const pid_t child = ::fork();
	if (child == 0) {
		::close(sync[0]);
		setEnv("BONDRIVER_PX4_CONFIG", (g_root + "/link.ini").c_str());
		IBonDriver2 *alias = api.createDriver();
		const char ok = (alias != nullptr && alias->OpenTuner() == TRUE) ? 1 : 0;
		(void)!::write(sync[1], &ok, 1);
		for (;;) {
			::pause();
		}
	}
	::close(sync[1]);
	char child_ok = 0;
	if (::read(sync[0], &child_ok, 1) != 1) {
		child_ok = 0;
	}
	::close(sync[0]);
	testutil::expect(child_ok == 1, "symlink-alias runtime acquires the lease");

	setEnv("BONDRIVER_PX4_CONFIG", (g_root + "/real.ini").c_str());
	IBonDriver2 *parent = api.createDriver();
	testutil::expect(parent != nullptr, "canonical runtime config loads");
	if (parent == nullptr) {
		::kill(child, SIGKILL);
		int status = 0;
		::waitpid(child, &status, 0);
		return 1;
	}
	testutil::expect(parent->OpenTuner() == FALSE, "canonical runtime rejects the symlink-alias holder");
	::kill(child, SIGKILL);
	int status = 0;
	::waitpid(child, &status, 0);
	testutil::expect(parent->OpenTuner() == TRUE, "canonical runtime acquires after alias holder dies");
	parent->Release();
	return 0;
#endif
}

int scenarioCleanupFailure(const Api &api)
{
	IBonDriver2 *a = api.createDriver();
	if (a == nullptr) {
		return 1;
	}
	testutil::expect(a->OpenTuner() == TRUE, "open for cleanup-failure test");
	setEnv("FAKE_MODE", "stream");
	setEnv("FAKE_STREAM_MS", "5000");
	testutil::expect(a->SetChannel(0, 0) == TRUE, "tune for cleanup-failure test");
	setEnv("BONDRIVER_FAULT_STOP", "1");
	a->CloseTuner();
	testutil::expect(a->IsTunerOpening() == FALSE, "close reports not opening after failure");
	testutil::expect(a->OpenTuner() == FALSE, "reopen refused after cleanup failure");
	IBonDriver2 *b = api.createDriver();
	testutil::expect(b != nullptr, "second core created");
	if (b != nullptr) {
		testutil::expect(b->OpenTuner() == FALSE, "cleanup-failure history keeps the lease from reuse");
		b->Release();
	}
	setEnv("BONDRIVER_FAULT_STOP", nullptr);
	a->Release();
	return 0;
}

int scenarioMultipleFactory(const Api &api)
{
	IBonDriver2 *a = api.createDriver();
	IBonDriver2 *b = api.createDriver();
	testutil::expect(a != nullptr && b != nullptr && a != b, "independent objects per factory call");
	if (a == nullptr || b == nullptr) {
		return 1;
	}
	testutil::expect(a->OpenTuner() == TRUE, "first core acquires receiver");
	testutil::expect(b->OpenTuner() == FALSE, "second core cannot share a fixed receiver");
	a->Release();
	testutil::expect(b->OpenTuner() == TRUE, "second core acquires after first released");
	b->Release();
	return 0;
}

int scenarioStruct(const Api &api)
{
	const STRUCT_IBONDRIVER *st = api.createStruct();
	testutil::expect(st != nullptr, "CreateBonStruct returns struct");
	if (st == nullptr) {
		return 1;
	}
	const auto *st2 = reinterpret_cast<const STRUCT_IBONDRIVER2 *>(st);
	testutil::expect(static_cast<const void *>(st->pEnd) >= static_cast<const void *>(st2 + 1),
	                 "pEnd covers the IBonDriver2 structure");
	const STRUCT_IBONDRIVER *again = api.createStruct();
	testutil::expect(again == st, "struct entry is a singleton while active");
	testutil::expect(st2->pF11(st->pCtx) == FALSE, "struct IsTunerOpening false before open");
	testutil::expect(st2->st.pF00(st->pCtx) == TRUE, "struct OpenTuner via function pointer");
	testutil::expect(st2->pF14(st->pCtx, 0, 0) == TRUE, "struct SetChannel via function pointer");
	testutil::expect(st2->st.pF04(st->pCtx, 2000) == 0, "struct WaitTsStream via function pointer");
	BYTE *data = nullptr;
	DWORD size = 0;
	DWORD remain = 0;
	testutil::expect(st2->st.pF07(st->pCtx, &data, &size, &remain) == TRUE, "struct pointer get");
	testutil::expect(size % 188 == 0, "struct get aligned");
	st2->st.pF09(st->pCtx);
	const STRUCT_IBONDRIVER *fresh = api.createStruct();
	testutil::expect(fresh != nullptr, "struct can be recreated after release");
	reinterpret_cast<const STRUCT_IBONDRIVER2 *>(fresh)->st.pF09(fresh->pCtx);
	return 0;
}

int scenarioPurgePartial(const Api &api)
{
	IBonDriver2 *driver = api.createDriver();
	if (driver == nullptr) {
		return 1;
	}
	driver->OpenTuner();
	setEnv("FAKE_MODE", "partial");
	testutil::expect(driver->SetChannel(0, 0) == TRUE, "partial tune succeeds");
	testutil::expect(driver->WaitTsStream(2000) == 0, "partial data available");
	setEnv("FAKE_MODE", "stream");
	driver->PurgeTsStream();
	testutil::expect(driver->WaitTsStream(2000) == 0, "post purge data available");
	BYTE *data = nullptr;
	DWORD size = 0;
	DWORD remain = 0;
	testutil::expect(driver->GetTsStream(&data, &size, &remain) == TRUE, "post purge get");
	testutil::expect(size % 188 == 0, "post purge no dangling partial");
	driver->Release();
	return 0;
}

int scenarioBufferOverflow(const Api &api)
{
	IBonDriver2 *driver = api.createDriver();
	if (driver == nullptr) {
		return 1;
	}
	driver->OpenTuner();
	setEnv("FAKE_MODE", "burst");
	setEnv("FAKE_BURST", "4000");
	testutil::expect(driver->SetChannel(0, 0) == TRUE, "overflow tune succeeds");
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	// limit is smaller than one acquisition block, so the queue can hold at
	// most the current (partial) acquisition buffer.
	testutil::expect(driver->GetReadyCount() <= 1, "bounded queue caps acquisition buffers");
	BYTE *data = nullptr;
	DWORD size = 0;
	DWORD remain = 0;
	testutil::expect(driver->GetTsStream(&data, &size, &remain) == TRUE, "drain after overflow");
	testutil::expect(size % 188 == 0, "overflow drain stays packet aligned");
	testutil::expect(size <= g_bufferLimit, "copy respects the configured queue limit");
	setEnv("FAKE_MODE", "dead");
	driver->Release();
	return 0;
}

int scenarioMultipleBuffers(const Api &api)
{
	IBonDriver2 *driver = api.createDriver();
	if (driver == nullptr) {
		return 1;
	}
	driver->OpenTuner();
	setEnv("FAKE_MODE", "burst");
	setEnv("FAKE_BURST", "4000");
	testutil::expect(driver->SetChannel(0, 0) == TRUE, "burst tune succeeds");
	DWORD ready = 0;
	for (int i = 0; i < 200; ++i) {
		ready = driver->GetReadyCount();
		if (ready >= 2) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	testutil::expect(ready >= 2, "more than one acquisition buffer is retrievable");
	DWORD expected = ready;
	BYTE *data = nullptr;
	DWORD size = 0;
	DWORD remain = 0;
	testutil::expect(driver->GetTsStream(&data, &size, &remain) == TRUE, "get one buffer");
	testutil::expect(remain == expected - 1, "remain equals remaining acquisition buffers");
	driver->Release();
	return 0;
}

int scenarioWaitClose(const Api &api)
{
	IBonDriver2 *driver = api.createDriver();
	if (driver == nullptr) {
		return 1;
	}
	driver->OpenTuner();
	setEnv("FAKE_MODE", "nooutput");
	testutil::expect(driver->SetChannel(0, 0) == FALSE, "no data while waiting for close test");
	// Reopen with a live producer that goes silent after tuning, then block in
	// an INFINITE wait while another thread closes the tuner.
	setEnv("FAKE_MODE", "quiet");
	testutil::expect(driver->SetChannel(0, 0) == TRUE, "quiet tune succeeds");
	for (int i = 0; i < 50; ++i) {
		BYTE *data = nullptr;
		DWORD size = 0;
		DWORD remain = 0;
		if (driver->GetTsStream(&data, &size, &remain) != TRUE) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	DWORD result = 0x1234;
	const auto start = std::chrono::steady_clock::now();
	std::thread waiter([&]() { result = driver->WaitTsStream(0xFFFFFFFFu); });
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	driver->CloseTuner();
	waiter.join();
	const long elapsed =
	    static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
	testutil::expect(elapsed < 4000, "INFINITE wait is released by CloseTuner");
	testutil::expect(result == 0xFFFFFFFFu || result == 0 || result == 258u, "wait returns a defined result");
	driver->Release();
	return 0;
}

int scenarioInvalidConfig(const Api &api, const std::string &ini)
{
	// Overwrite with an invalid config (missing tuner_name) and expect no object.
	testutil::writeFile(ini,
	                    "[common]\ncli_path = " + g_fakeCli + "\nchannel_table = channels.tsv\n[siano]\ndevice = 0\n");
	IBonDriver2 *driver = api.createDriver();
	testutil::expect(driver == nullptr, "invalid config yields no driver object");
	const STRUCT_IBONDRIVER *st = api.createStruct();
	testutil::expect(st == nullptr, "invalid config yields no struct object");
	return 0;
}

} // namespace

int main(int argc, char **argv)
{
	testutil::expect(argc >= 5, "usage: test_driver LIB BACKEND FAKE_CLI SCENARIO");
	if (argc < 5) {
		return testutil::report("test_driver");
	}
	const std::string lib = argv[1];
	const std::string backend = argv[2];
	g_fakeCli = argv[3];
	const std::string scenario = argv[4];
	g_siano = backend == "siano";
	g_selfExe = currentExePath();
	g_lib = lib;
	g_backend = backend;
	const Api api = loadApi(lib);
	testutil::expect(api.createDriver != nullptr, "CreateBonDriver exported");
	testutil::expect(api.createStruct != nullptr, "CreateBonStruct exported");
	if (api.createDriver == nullptr) {
		return testutil::report("test_driver");
	}

#ifdef _WIN32
	if (scenario == "lease-holder") {
		// Preserve the parent's config and receiver identity. The helper uses a
		// finite lifetime in case the parent cannot terminate it.
		const char *ready = std::getenv("FAKE_LEASE_READY");
		if (ready == nullptr || *ready == '\0') {
			return 3;
		}
		IBonDriver2 *holder = api.createDriver();
		if (holder == nullptr || holder->OpenTuner() != TRUE) {
			if (holder != nullptr) holder->Release();
			return 3;
		}
		FILE *f = nullptr;
#ifdef _WIN32
		const std::wstring ready_w = toWideLocal(ready);
		_wfopen_s(&f, ready_w.c_str(), L"wb");
#else
		f = std::fopen(ready, "wb");
#endif
		if (f != nullptr) {
			std::fclose(f);
		}
		for (int i = 0; i < 300; ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		holder->CloseTuner();
		holder->Release();
		return 0;
	}
#endif

	if (scenario == "buffer_overflow") {
		g_bufferLimit = 188 * 20;
	} else if (scenario == "multiple_buffers") {
		g_bufferLimit = 8 * 1024 * 1024;
	}
	g_root = testutil::tempRoot() + "-driver-" + scenario + "-" + std::to_string(processId());
	testutil::makeDir(testutil::tempRoot());
	testutil::makeDir(g_root);
	testutil::makeDir(g_root + "/runtime");
	testutil::writeFile(g_root + "/channels.tsv", tableText());
	const std::string ini = g_root + "/config.ini";
	testutil::writeFile(ini, configText());
	setEnv(g_siano ? "BONDRIVER_SIANO_CONFIG" : "BONDRIVER_PX4_CONFIG", ini.c_str());

	int rc = 0;
	if (scenario == "basic") {
		rc = scenarioBasic(api);
	} else if (scenario == "startup_timeout") {
		rc = scenarioStartupTimeout(api);
	} else if (scenario == "dead_child") {
		rc = scenarioDeadChild(api);
	} else if (scenario == "lease") {
		rc = scenarioLease(api);
	} else if (scenario == "multiple_factory") {
		rc = scenarioMultipleFactory(api);
	} else if (scenario == "struct") {
		rc = scenarioStruct(api);
	} else if (scenario == "purge_partial") {
		rc = scenarioPurgePartial(api);
	} else if (scenario == "buffer_overflow") {
		rc = scenarioBufferOverflow(api);
	} else if (scenario == "multiple_buffers") {
		rc = scenarioMultipleBuffers(api);
	} else if (scenario == "wait_close") {
		rc = scenarioWaitClose(api);
	} else if (scenario == "invalid_config") {
		rc = scenarioInvalidConfig(api, ini);
	} else if (scenario == "runtime_alias") {
		rc = scenarioRuntimeAlias(api);
	} else if (scenario == "cleanup_failure") {
		rc = scenarioCleanupFailure(api);
	} else {
		testutil::expect(false, "unknown scenario " + scenario);
		rc = 1;
	}
	testutil::expect(rc == 0, "scenario '" + scenario + "' completed");
	return testutil::report("test_driver");
}
