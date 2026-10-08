#include "bondriver/Pipeline.h"
#include "test_util.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#ifndef _WIN32
#  include <csignal>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
#else
#  include <windows.h>
#endif

#include "bondriver/DebugLog.h"

using namespace bondriver;

namespace {

std::string g_cli;

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

SpawnSpec spec(const std::string &mode, const std::string &tag)
{
	setEnv("FAKE_MODE", mode.c_str());
	setEnv("FAKE_TAG", tag.c_str());
	SpawnSpec s;
	s.argv = {g_cli, "--freq", tag};
	s.diagnostics = "discard";
	return s;
}

long elapsedMs(const std::chrono::steady_clock::time_point &start)
{
	return static_cast<long>(
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

} // namespace

int main(int argc, char **argv)
{
	g_cli = argc > 1 ? argv[1] : std::getenv("FAKE_CLI");
	testutil::expect(!g_cli.empty(), "fake cli path provided");

	{
		setEnv("FAKE_STREAM_MS", "600");
		Pipeline pipeline;
		std::string error;
		SpawnSpec s = spec("stream", "A");
		testutil::expect(pipeline.start(s, error), "normal start: " + error);
		testutil::expect(pipeline.waitReady(2000), "normal stream produces data");
		testutil::expect(pipeline.stop(2000, 1000), "normal stop reaps child");
		testutil::expect(pipeline.reaped(), "reaped flag set");
		testutil::expect(!pipeline.running(), "not running after stop");
	}

	{
		setEnv("FAKE_STREAM_MS", nullptr);
		Pipeline pipeline;
		std::string error;
		testutil::expect(pipeline.start(spec("nooutput", "B"), error), "nooutput start: " + error);
		testutil::expect(!pipeline.waitReady(300), "no data before timeout");
		testutil::expect(pipeline.stop(500, 1000), "nooutput stop");
	}

	{
		setEnv("FAKE_STREAM_MS", nullptr);
		Pipeline pipeline;
		std::string error;
		testutil::expect(pipeline.start(spec("fail", "C"), error), "fail start: " + error);
		testutil::expect(!pipeline.waitReady(1500), "fail produces no data");
		testutil::expect(pipeline.stdoutClosed(), "failed child closes stdout");
		testutil::expect(pipeline.stop(1000, 1000), "fail stop");
	}

	{
		setEnv("FAKE_STREAM_MS", nullptr);
		Pipeline pipeline;
		std::string error;
		testutil::expect(pipeline.start(spec("dead", "D"), error), "dead start: " + error);
		testutil::expect(pipeline.waitReady(2000), "dead child emits before exit");
		for (int i = 0; i < 200 && !pipeline.stdoutClosed(); ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		testutil::expect(pipeline.stdoutClosed(), "dead child closes stdout");
		{
			std::lock_guard<std::mutex> lock(pipeline.mutex());
			size_t out = 0;
			size_t remain = 0;
			pipeline.buffer().takePointer(out, remain);
		}
		testutil::expect(!pipeline.waitReady(300), "no more data after dead child");
		testutil::expect(pipeline.stop(1000, 1000), "dead stop");
	}

	{
		setEnv("FAKE_STREAM_MS", "800");
		Pipeline pipeline;
		std::string error;
		testutil::expect(pipeline.start(spec("partial", "P"), error), "partial start: " + error);
		testutil::expect(pipeline.waitReady(2000), "partial produces data");
		{
			std::lock_guard<std::mutex> lock(pipeline.mutex());
			size_t out = 0;
			size_t remain = 0;
			pipeline.buffer().takePointer(out, remain);
			testutil::expect(out % 188 == 0, "partial read exposes packet-aligned bytes");
		}
		testutil::expect(pipeline.stop(1000, 1000), "partial stop");
	}

	{
		setEnv("FAKE_STREAM_MS", "5000");
		Pipeline pipeline;
		std::string error;
		testutil::expect(pipeline.start(spec("ignore_term", "I"), error), "ignore_term start: " + error);
		testutil::expect(pipeline.waitReady(2000), "ignore_term produces data");
		const auto start = std::chrono::steady_clock::now();
		testutil::expect(pipeline.stop(400, 2000), "ignore_term stopped via kill");
		testutil::expect(elapsedMs(start) < 4000, "ignore_term stop is bounded");
	}

	{
		setEnv("FAKE_STREAM_MS", "800");
		Pipeline pipeline;
		std::string error;
		testutil::expect(pipeline.start(spec("flood_stderr", "F"), error), "flood start: " + error);
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		testutil::expect(pipeline.stop(1000, 1000), "flood stop");
		testutil::expect(pipeline.lastDiagnostics().size() <= 8192, "stderr diagnostics bounded");
	}

	{
		setEnv("FAKE_STREAM_MS", nullptr);
		Pipeline pipeline;
		std::string error;
		SpawnSpec s = spec("hold_descendant", "H");
		s.diagnostics = "discard";
		testutil::expect(pipeline.start(s, error), "hold_descendant start: " + error);
		testutil::expect(pipeline.waitReady(1000), "hold_descendant emits data");
		const auto start = std::chrono::steady_clock::now();
		testutil::expect(pipeline.stop(500, 2000), "hold_descendant stop reaps owned child");
		testutil::expect(elapsedMs(start) < 4000, "descendant holding pipe does not block join");
	}

	{
		// Continuous stderr flood must not delay stop, and the retained tail
		// must stay bounded.
		setEnv("FAKE_STREAM_MS", nullptr);
		Pipeline pipeline;
		std::string error;
		testutil::expect(pipeline.start(spec("flood_stderr", "F2"), error), "continuous flood start: " + error);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		const auto start = std::chrono::steady_clock::now();
		testutil::expect(pipeline.stop(800, 1500), "continuous flood stop");
		testutil::expect(elapsedMs(start) < 4000, "continuous flood stop bounded");
		testutil::expect(pipeline.lastDiagnostics().size() <= 8192, "continuous flood tail bounded");
	}

	{
		// A single diagnostic longer than a typical pipe capacity must be
		// drained and retained without synchronously forwarding it to host stderr.
		setEnv("FAKE_STREAM_MS", "5000");
		Pipeline pipeline;
		std::string error;
		SpawnSpec s = spec("long_diagnostic", "LONG");
		s.diagnostics = "stderr";
		testutil::expect(pipeline.start(s, error), "long-diagnostic start: " + error);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (pipeline.lastDiagnostics().find(std::string(64, 'X')) == std::string::npos &&
		       std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		testutil::expect(pipeline.lastDiagnostics().size() <= 8192, "long diagnostic tail bounded");
		testutil::expect(pipeline.lastDiagnostics().find(std::string(64, 'X')) != std::string::npos,
		                 "long diagnostic drained into retained tail");
		testutil::expect(diagnosticTail().find(std::string(64, 'X')) != std::string::npos,
		                 "long diagnostic reaches bounded diagnostic tail");
		const auto start = std::chrono::steady_clock::now();
		testutil::expect(pipeline.stop(500, 1500), "long diagnostic stop bounded");
		testutil::expect(elapsedMs(start) < 3000, "long diagnostic cannot block reader join");
	}

#ifndef _WIN32
	{
		// Simulate the host's stderr as an empty, 4096-byte unread pipe. This test
		// process owns the temporary redirection; the external CTest timeout is
		// the independent watchdog and prints no signal-handler diagnostics.
		int sink[2];
		const bool pipe_ok = ::pipe(sink) == 0;
		int saved_stderr = -1;
		bool setup_ok = pipe_ok;
		if (setup_ok) {
#if defined(__linux__) && defined(F_SETPIPE_SZ)
			setup_ok = ::fcntl(sink[1], F_SETPIPE_SZ, 4096) == 4096;
#endif
			saved_stderr = ::dup(STDERR_FILENO);
			setup_ok = setup_ok && saved_stderr >= 0 && ::dup2(sink[1], STDERR_FILENO) == STDERR_FILENO;
		}
		bool start_ok = false;
		bool ready_ok = false;
		bool stop_ok = false;
		if (setup_ok) {
			setEnv("FAKE_STREAM_MS", "5000");
			Pipeline pipeline;
			std::string error;
			SpawnSpec s = spec("long_diagnostic", "BLOCKED-SINK");
			s.diagnostics = "stderr";
			start_ok = pipeline.start(s, error);
			ready_ok = start_ok && pipeline.waitReady(2000);
			const auto start = std::chrono::steady_clock::now();
			stop_ok = start_ok && pipeline.stop(500, 1500) && elapsedMs(start) < 3000;
		}
		if (saved_stderr >= 0) {
			::dup2(saved_stderr, STDERR_FILENO);
			::close(saved_stderr);
		}
		if (pipe_ok) { ::close(sink[0]); ::close(sink[1]); }
		testutil::expect(setup_ok, "prepare empty unread host stderr pipe");
		testutil::expect(start_ok, "blocked-host-sink pipeline starts");
		testutil::expect(ready_ok, "30,000-byte diagnostic does not block TS acquisition");
		testutil::expect(stop_ok, "blocked-host-sink close remains finite");
	}
#else
	{
		// Windows receives the same blocked-sink case through its inherited
		// stderr handle. The external CTest timeout watches this process.
		SECURITY_ATTRIBUTES sa{};
		sa.nLength = sizeof sa;
		sa.bInheritHandle = TRUE;
		HANDLE read_end = nullptr;
		HANDLE write_end = nullptr;
		const bool pipe_ok = CreatePipe(&read_end, &write_end, &sa, 4096) != FALSE;
		if (read_end != nullptr) SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
		const HANDLE saved_stderr = GetStdHandle(STD_ERROR_HANDLE);
		const bool setup_ok = pipe_ok && SetStdHandle(STD_ERROR_HANDLE, write_end) != FALSE;
		bool start_ok = false;
		bool ready_ok = false;
		bool stop_ok = false;
		if (setup_ok) {
			setEnv("FAKE_STREAM_MS", "5000");
			Pipeline pipeline;
			std::string error;
			SpawnSpec s = spec("long_diagnostic", "BLOCKED-SINK");
			s.diagnostics = "stderr";
			start_ok = pipeline.start(s, error);
			ready_ok = start_ok && pipeline.waitReady(2000);
			const auto start = std::chrono::steady_clock::now();
			stop_ok = start_ok && pipeline.stop(500, 1500) && elapsedMs(start) < 3000;
		}
		SetStdHandle(STD_ERROR_HANDLE, saved_stderr);
		if (read_end != nullptr) CloseHandle(read_end);
		if (write_end != nullptr) CloseHandle(write_end);
		testutil::expect(setup_ok, "prepare unread Windows stderr pipe");
		testutil::expect(start_ok, "Windows blocked-host-sink pipeline starts");
		testutil::expect(ready_ok, "Windows long diagnostic does not block TS acquisition");
		testutil::expect(stop_ok, "Windows blocked-host-sink close remains finite");
	}
#endif

	{
		const std::string path = testutil::tempRoot() + "-diagnostics-" +
		                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log";
		std::remove(path.c_str());
		setEnv("FAKE_STREAM_MS", "5000");
		Pipeline pipeline;
		std::string error;
		SpawnSpec s = spec("long_diagnostic", "FILE");
		s.diagnostics = "file:";
		s.diagnostics_file = path;
		testutil::expect(pipeline.start(s, error), "regular-file diagnostics start: " + error);
		testutil::expect(pipeline.waitReady(2000), "TS continues with regular-file diagnostics");
		testutil::expect(pipeline.stop(500, 1500), "regular-file diagnostics stop");
		const std::string contents = testutil::readFile(path);
		testutil::expect(contents.find("selector=") != std::string::npos && contents.size() >= 30009,
		                 "file diagnostics contains long child diagnostic");
		std::remove(path.c_str());
	}

	{
		// Diagnostics must not change the host stderr flags (dup shares the
		// open file description; F_SETFL would leak to the host).
#ifndef _WIN32
		const int flags_before = ::fcntl(STDERR_FILENO, F_GETFL, 0);
		const int fdflags_before = ::fcntl(STDERR_FILENO, F_GETFD, 0);
#endif
		setEnv("FAKE_STREAM_MS", nullptr);
		Pipeline pipeline;
		std::string error;
		SpawnSpec s = spec("fail", "S2");
		s.diagnostics = "stderr";
		testutil::expect(pipeline.start(s, error), "stderr-diagnostics start: " + error);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		testutil::expect(pipeline.stop(800, 1000), "stderr-diagnostics stop");
#ifndef _WIN32
		testutil::expect(::fcntl(STDERR_FILENO, F_GETFL, 0) == flags_before, "host stderr O_ flags unchanged");
		testutil::expect(::fcntl(STDERR_FILENO, F_GETFD, 0) == fdflags_before, "host stderr FD flags unchanged");
#endif
	}

#ifndef _WIN32
	{
		// A FIFO diagnostic sink with no reader must not block startup.
		const std::string fifo = std::string("/tmp/bondriver-test-fifo-") + std::to_string(::getpid());
		::unlink(fifo.c_str());
		testutil::expect(::mkfifo(fifo.c_str(), 0600) == 0, "create diagnostic FIFO");
		setEnv("FAKE_STREAM_MS", "3000");
		Pipeline pipeline;
		std::string error;
		SpawnSpec s = spec("stream", "Q");
		s.diagnostics = "file:";
		s.diagnostics_file = fifo;
		const auto start = std::chrono::steady_clock::now();
		testutil::expect(!pipeline.start(s, error), "FIFO diagnostics sink is rejected immediately");
		testutil::expect(elapsedMs(start) < 1000, "FIFO diagnostics rejection is bounded");
		::unlink(fifo.c_str());
	}

	{
		// The producer fixture exits on a broken stdout pipe (graceful stop
		// contract used by the Windows close path).
		int fds[2];
		testutil::expect(::pipe(fds) == 0, "broken-pipe fixture pipe");
		const pid_t pid = ::fork();
		if (pid == 0) {
			::close(fds[0]);
			::dup2(fds[1], STDOUT_FILENO);
			::close(fds[1]);
			setEnv("FAKE_MODE", "exit_on_broken");
			setEnv("FAKE_TAG", "Z");
			::execl(g_cli.c_str(), g_cli.c_str(), "--freq", "Z", static_cast<char *>(nullptr));
			::_exit(127);
		}
		::close(fds[1]);
		char buf[188];
		const ssize_t got = ::read(fds[0], buf, sizeof buf);
		testutil::expect(got > 0, "producer emitted before pipe break");
		::close(fds[0]);
		bool exited = false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		int status = 0;
		while (std::chrono::steady_clock::now() < deadline) {
			if (::waitpid(pid, &status, WNOHANG) == pid) {
				exited = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		testutil::expect(exited, "producer exits on broken stdout pipe");
		if (!exited) {
			::kill(pid, SIGKILL);
			::waitpid(pid, &status, 0);
		}
	}
#endif

	return testutil::report("test_process");
}
