// Deterministic stand-in for siano-ts / px4-ts used by isolation tests.
// Behaviour is selected with FAKE_MODE so the library argv contract stays the
// production one.  It never touches real hardware or daemons.
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace {

volatile std::sig_atomic_t g_stop = 0;

void onTerm(int)
{
	g_stop = 1;
}

const char *envOr(const char *name, const char *def)
{
	const char *v = std::getenv(name);
	return (v == nullptr || *v == '\0') ? def : v;
}

long envLong(const char *name, long def)
{
	const char *v = std::getenv(name);
	if (v == nullptr || *v == '\0') {
		return def;
	}
	return std::strtol(v, nullptr, 10);
}

std::string findArg(int argc, char **argv, const char *name)
{
	for (int i = 1; i + 1 < argc; ++i) {
		if (std::strcmp(argv[i], name) == 0) {
			return argv[i + 1];
		}
	}
	return std::string();
}

void sleepMs(long ms)
{
	if (ms > 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(ms));
	}
}

bool writeStdout(const uint8_t *data, size_t n)
{
	if (n == 0) {
		return true;
	}
	const size_t written = std::fwrite(data, 1, n, stdout);
	// ferror becomes set when the pipe breaks (EPIPE / ERROR_BROKEN_PIPE), so
	// the producer can exit gracefully when the consumer closes its end.
	return written == n && std::ferror(stdout) == 0;
}

void emitPacket(const std::string &tag, uint32_t seq)
{
	uint8_t packet[188];
	std::memset(packet, 0, sizeof packet);
	packet[0] = 0x47;
	packet[1] = static_cast<uint8_t>(seq >> 24);
	packet[2] = static_cast<uint8_t>(seq >> 16);
	packet[3] = static_cast<uint8_t>(seq >> 8);
	std::memset(packet + 4, 0xAA, 16);
	const size_t tag_len = tag.size() < 150 ? tag.size() : 150;
	std::memcpy(packet + 20, tag.data(), tag_len);
	packet[20 + tag_len] = static_cast<uint8_t>(seq & 0xFF);
	writeStdout(packet, sizeof packet);
}

void streamLoop(const std::string &tag, long deadline_ms, long burst_first)
{
	uint32_t seq = 0;
	for (long i = 0; i < burst_first; ++i) {
		emitPacket(tag, seq++);
	}
	std::fflush(stdout);
	const auto start = std::chrono::steady_clock::now();
	for (;;) {
		if (g_stop) {
			return;
		}
		if (std::ferror(stdout) != 0) {
			return; // stdout pipe broke: exit gracefully
		}
		emitPacket(tag, seq++);
		if (seq % 8 == 0) {
			std::fflush(stdout);
		}
		if (deadline_ms > 0) {
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			                         std::chrono::steady_clock::now() - start)
			                         .count();
			if (elapsed >= deadline_ms) {
				return;
			}
		}
		sleepMs(2);
	}
}

#ifdef _WIN32
int holdDescendant(const std::string &tag)
{
	// Spawn a grandchild that inherits this process's stdout handle and stays
	// alive, so the inherited write end outlives the direct child.  The
	// library must still cancel its reader and reap the direct child.
	wchar_t exe[MAX_PATH];
	const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
	SetHandleInformation(out, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	SetHandleInformation(err, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	if (n > 0 && n < MAX_PATH) {
		std::wstring cmd = std::wstring(L"\"") + exe + L"\" --hold-only";
		std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
		cmdbuf.push_back(L'\0');
		STARTUPINFOW si{};
		si.cb = sizeof si;
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		si.hStdOutput = out;
		si.hStdError = err;
		PROCESS_INFORMATION pi{};
		if (CreateProcessW(nullptr, cmdbuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
		                   &pi)) {
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}
	}
	for (int i = 0; i < 5; ++i) {
		emitPacket(tag, static_cast<uint32_t>(i));
		sleepMs(2);
	}
	std::fflush(stdout);
	return 0; // direct child exits while the grandchild holds the pipe
}
#else
int holdDescendant(const std::string &tag)
{
	const pid_t pid = ::fork();
	if (pid == 0) {
		// Grandchild keeps the inherited stdout pipe open longer than the
		// parent, so the reader must cancel via its own stop pipe.
		sleepMs(3000);
		::_exit(0);
	}
	for (int i = 0; i < 5; ++i) {
		emitPacket(tag, static_cast<uint32_t>(i));
		sleepMs(2);
	}
	std::fflush(stdout);
	return 0; // parent exits while grandchild still holds the pipe
}
#endif

} // namespace

int main(int argc, char **argv)
{
#ifdef _WIN32
	_setmode(_fileno(stdout), _O_BINARY);
#endif
	std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifndef _WIN32
#ifndef _WIN32
	std::signal(SIGPIPE, SIG_IGN);
#endif
#endif
	const std::string mode = envOr("FAKE_MODE", "stream");
	const std::string tag = envOr("FAKE_TAG", "");
	std::string effectiveTag = tag;
	if (effectiveTag.empty()) {
		const std::string freq = findArg(argc, argv, "--freq");
		const std::string khz = findArg(argc, argv, "--frequency-khz");
		const std::string channel = findArg(argc, argv, "--channel");
		effectiveTag = !freq.empty() ? freq : (!khz.empty() ? khz : channel);
	}
	if (effectiveTag.empty()) {
		effectiveTag = "default";
	}
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--hold-only") == 0) {
			// Descendant used by the hold_descendant fixture: hold inherited
			// handles open without producing data.
			sleepMs(3000);
			return 0;
		}
	}

	if (mode == "fail") {
		std::fprintf(stderr, "fake_cli: deliberate failure\n");
		return 3;
	}
	if (mode == "ignore_term") {
		std::signal(SIGTERM, SIG_IGN);
	} else {
		std::signal(SIGTERM, onTerm);
	}

	if (mode == "nooutput") {
		while (!g_stop) {
			sleepMs(20);
		}
		return 0;
	}
	if (mode == "quiet") {
		// Enough data to tune, then silence while staying alive.
		for (int i = 0; i < 8; ++i) {
			emitPacket(effectiveTag, static_cast<uint32_t>(i));
		}
		std::fflush(stdout);
		while (!g_stop) {
			sleepMs(20);
		}
		return 0;
	}

	// Continuous diagnostic flood, maintained until the process is stopped.
	std::thread flusher;
	if (mode == "flood_stderr") {
		flusher = std::thread([]() {
			long i = 0;
			while (!g_stop) {
				std::fprintf(stderr, "fake_cli flood line %ld tag=flood\n", i++);
				if (i % 200 == 0) {
					std::fflush(stderr);
				}
			}
		});
	}

	sleepMs(envLong("FAKE_SLEEP_BEFORE_MS", 0));
	if (mode == "long_diagnostic") {
		std::fwrite("selector=", 1, 9, stderr);
		for (int i = 0; i < 30000; ++i) {
			std::fputc('X', stderr);
		}
		std::fputc('\n', stderr);
		std::fflush(stderr);
		streamLoop(effectiveTag, envLong("FAKE_STREAM_MS", 0), 0);
		return 0;
	}
	if (mode == "hold_descendant") {
		return holdDescendant(effectiveTag);
	}
	if (mode == "dead") {
		for (int i = 0; i < 3; ++i) {
			emitPacket(effectiveTag, static_cast<uint32_t>(i));
		}
		std::fflush(stdout);
		return 0;
	}
	if (mode == "partial") {
		uint8_t half[188 * 2];
		std::memset(half, 0, sizeof half);
		half[0] = 0x47;
		writeStdout(half, 100);
		std::fflush(stdout);
		sleepMs(80);
		streamLoop(effectiveTag, 0, 0);
		return 0;
	}

	long deadline = envLong("FAKE_STREAM_MS", 0);
	long burst = envLong("FAKE_BURST_FIRST", 0);
	if (mode == "burst") {
		// Emit a fixed burst, then stay alive and quiet so buffer counts are
		// stable for the test.
		const long count = envLong("FAKE_BURST", 2000);
		for (long i = 0; i < count; ++i) {
			emitPacket(effectiveTag, static_cast<uint32_t>(i));
		}
		std::fflush(stdout);
		while (!g_stop) {
			sleepMs(20);
		}
	} else if (mode == "flood_stderr" || mode == "exit_on_broken") {
		streamLoop(effectiveTag, 0, burst);
	} else {
		streamLoop(effectiveTag, deadline, burst);
	}
	if (flusher.joinable()) {
		flusher.join();
	}
	return 0;
}
