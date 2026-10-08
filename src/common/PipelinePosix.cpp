// POSIX implementation of Pipeline using posix_spawn (no fork), non-blocking
// fds and poll(2).  No async-unsafe work happens after fork.
#include "bondriver/Pipeline.h"
#include "bondriver/DebugLog.h"
#include "ProcessEnvironment.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <new>
#include <memory>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace bondriver {

struct Pipeline::Impl {
	pid_t pid = -1;
	int stdout_fd = -1;
	int stderr_fd = -1;
	int stop_pipe[2] = {-1, -1};
	std::string diag_mode;
};

namespace {

// Bound the work done per poll iteration so a child that never stops writing
// cannot starve observation of the stop pipe.
constexpr size_t kMaxStdoutPerIteration = 1u << 20;
constexpr size_t kMaxStderrPerIteration = 1u << 16;
constexpr size_t kDiagTailLimit = 8192;

bool setNonBlocking(int fd)
{
	const int flags = ::fcntl(fd, F_GETFL, 0);
	if (flags < 0) {
		return false;
	}
	return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool makePipe(int fds[2], bool close_on_exec)
{
	if (::pipe(fds) != 0) {
		return false;
	}
	if (close_on_exec) {
		if (::fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0 || ::fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0) {
			::close(fds[0]);
			::close(fds[1]);
			return false;
		}
	}
	return true;
}

void closeIfValid(int &fd)
{
	if (fd >= 0) {
		::close(fd);
		fd = -1;
	}
}

// Returns the number of bytes read in this call; stops early when the caller's
// budget is exhausted or the pipe is drained.
void readerLoop(Pipeline *self, Pipeline::Impl *impl, const std::string &mode) try
{
	bool eof = false;
	bool read_error = false;
	char buf[65536];
	for (;;) {
		struct pollfd fds[3];
		fds[0].fd = impl->stdout_fd;
		fds[0].events = POLLIN;
		fds[0].revents = 0;
		fds[1].fd = impl->stderr_fd;
		fds[1].events = POLLIN;
		fds[1].revents = 0;
		fds[2].fd = impl->stop_pipe[0];
		fds[2].events = POLLIN;
		fds[2].revents = 0;

		const int r = ::poll(fds, 3, -1);
		if (r < 0) {
			if (errno == EINTR) {
				continue;
			}
			read_error = true;
			break;
		}
		if (fds[2].revents & POLLIN) {
			break;
		}
		if (fds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
			char errbuf[8192];
			std::string tail = self->readerGetDiagTail();
			size_t total = 0;
			while (total < kMaxStderrPerIteration) {
				const size_t want = sizeof errbuf < kMaxStderrPerIteration - total
				                        ? sizeof errbuf : kMaxStderrPerIteration - total;
				const ssize_t n = ::read(impl->stderr_fd, errbuf, want);
				if (n > 0) {
					appendDiagnosticText(std::string(errbuf, static_cast<size_t>(n)));
					tail.append(errbuf, static_cast<size_t>(n));
					if (tail.size() > kDiagTailLimit) tail.erase(0, tail.size() - kDiagTailLimit);
					total += static_cast<size_t>(n);
					continue;
				}
				if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
					closeIfValid(impl->stderr_fd);
					break;
				}
				if (n < 0 && errno == EINTR) continue;
				break;
			}
			self->readerSetDiagTail(tail);
		}
		if (fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
			size_t drained = 0;
			for (;;) {
				if (drained >= kMaxStdoutPerIteration) {
					break;
				}
				const ssize_t n = ::read(impl->stdout_fd, buf, sizeof buf);
				if (n > 0) {
					self->readerAppendTs(reinterpret_cast<const uint8_t *>(buf), static_cast<size_t>(n));
					drained += static_cast<size_t>(n);
					continue;
				}
				if (n == 0) {
					eof = true;
					break;
				}
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					break;
				}
				if (errno == EINTR) {
					continue;
				}
				read_error = true;
				eof = true;
				break;
			}
		}
		if (eof) {
			break;
		}
	}
	if (eof || read_error) {
		self->readerMarkClosed(eof, read_error);
	}
	(void)mode;
}
catch (...) { self->readerMarkClosed(true, true); }

bool waitReap(pid_t pid, uint64_t ms)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	for (;;) {
		int status = 0;
		const pid_t r = ::waitpid(pid, &status, WNOHANG);
		if (r == pid || (r < 0 && errno == ECHILD)) {
			return true;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			return false;
		}
		Pipeline::sleepMillis(10);
	}
}

bool terminateChild(pid_t pid, uint64_t stop_ms, uint64_t kill_ms)
{
	if (pid <= 0) {
		return true;
	}
	::kill(pid, SIGTERM);
	if (waitReap(pid, stop_ms)) {
		return true;
	}
	::kill(pid, SIGKILL);
	return waitReap(pid, kill_ms);
}

} // namespace

Pipeline::Pipeline() = default;

Pipeline::~Pipeline()
{
	try { stop(2000, 1000); } catch (...) {}
}

bool Pipeline::start(const SpawnSpec &spec, std::string &error)
{
	if (cleanupFailed()) {
		error = "pipeline cleanup previously failed";
		return false;
	}
	if (running_) {
		error = "pipeline already running";
		return false;
	}
	std::unique_ptr<Impl> impl_owner(new Impl());
	auto *impl = impl_owner.get();
	impl->diag_mode = spec.diagnostics;

	int out_pipe[2] = {-1, -1};
	int err_pipe[2] = {-1, -1};
	int stop_pipe[2] = {-1, -1};
	int child_diag_fd = -1;

	auto cleanupLocal = [&]() {
		closeIfValid(out_pipe[0]);
		closeIfValid(out_pipe[1]);
		closeIfValid(err_pipe[0]);
		closeIfValid(err_pipe[1]);
		closeIfValid(stop_pipe[0]);
		closeIfValid(stop_pipe[1]);
		closeIfValid(child_diag_fd);
		impl_owner.reset();
	};

	if (!makePipe(out_pipe, true)) {
		const int e = errno;
		cleanupLocal();
		try { error = std::string("stdout pipe failed: ") + std::strerror(e); } catch (...) {}
		return false;
	}
	if (!makePipe(err_pipe, true)) {
		const int e = errno;
		cleanupLocal();
		try { error = std::string("stderr pipe failed: ") + std::strerror(e); } catch (...) {}
		return false;
	}
	if (!makePipe(stop_pipe, true)) {
		const int e = errno;
		cleanupLocal();
		try { error = std::string("stop pipe failed: ") + std::strerror(e); } catch (...) {}
		return false;
	}
	if (!setNonBlocking(out_pipe[0]) || !setNonBlocking(err_pipe[0]) || !setNonBlocking(stop_pipe[0])) {
		const int e = errno;
		cleanupLocal();
		try { error = std::string("fcntl failed: ") + std::strerror(e); } catch (...) {}
		return false;
	}
	impl->stop_pipe[0] = stop_pipe[0];
	impl->stop_pipe[1] = stop_pipe[1];

	posix_spawn_file_actions_t fa;
	int action_rc = posix_spawn_file_actions_init(&fa);
	if (action_rc != 0) {
		cleanupLocal();
		try { error = "posix_spawn file-actions initialization failed"; } catch (...) {}
		return false;
	}
	auto addAction = [&](int rc) {
		if (action_rc == 0) action_rc = rc;
		return action_rc == 0;
	};
	bool actions_ok = addAction(posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0)) &&
	                  addAction(posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO)) &&
	                  addAction(posix_spawn_file_actions_adddup2(&fa, err_pipe[1], STDERR_FILENO));
	if (spec.diagnostics == "discard") {
		actions_ok = actions_ok && addAction(posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0));
	} else if (spec.diagnostics.rfind("file:", 0) == 0 && !spec.diagnostics_file.empty()) {
		child_diag_fd = ::open(spec.diagnostics_file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NONBLOCK, 0644);
		struct stat diag_stat{};
		if (child_diag_fd < 0 || ::fstat(child_diag_fd, &diag_stat) != 0 || !S_ISREG(diag_stat.st_mode)) {
			posix_spawn_file_actions_destroy(&fa);
			cleanupLocal();
			try { error = "diagnostics file must be an accessible regular file"; } catch (...) {}
			return false;
		}
		actions_ok = actions_ok && addAction(posix_spawn_file_actions_adddup2(&fa, child_diag_fd, STDERR_FILENO));
	}
	actions_ok = actions_ok && addAction(posix_spawn_file_actions_addclose(&fa, out_pipe[0]));
	actions_ok = actions_ok && addAction(posix_spawn_file_actions_addclose(&fa, out_pipe[1]));
	actions_ok = actions_ok && addAction(posix_spawn_file_actions_addclose(&fa, err_pipe[0]));
	actions_ok = actions_ok && addAction(posix_spawn_file_actions_addclose(&fa, err_pipe[1]));
	if (!actions_ok) {
		posix_spawn_file_actions_destroy(&fa);
		cleanupLocal();
		try { error = "posix_spawn file-actions setup failed"; } catch (...) {}
		return false;
	}
	// stop_pipe/diag fds carry FD_CLOEXEC, so they never reach the child; the
	// only inherited handles are the duplicated std handles.

	std::vector<char *> argv;
	try {
		argv.reserve(spec.argv.size() + 1);
		for (const std::string &arg : spec.argv) {
			argv.push_back(const_cast<char *>(arg.c_str()));
		}
		argv.push_back(nullptr);
	} catch (...) {
		posix_spawn_file_actions_destroy(&fa);
		cleanupLocal();
		try { error = "argument allocation failed before posix_spawn"; } catch (...) {}
		return false;
	}

	pid_t pid = -1;
	const int rc = posix_spawn(&pid, argv[0], &fa, nullptr, argv.data(), environ);
	posix_spawn_file_actions_destroy(&fa);
	closeIfValid(child_diag_fd);
	if (rc != 0) {
		cleanupLocal();
		try { error = std::string("posix_spawn failed: ") + std::strerror(rc); } catch (...) {}
		return false;
	}
	::close(out_pipe[1]);
	out_pipe[1] = -1;
	::close(err_pipe[1]);
	err_pipe[1] = -1;
	impl->pid = pid;
	impl->stdout_fd = out_pipe[0];
	impl->stderr_fd = err_pipe[0];
	out_pipe[0] = -1;
	err_pipe[0] = -1;
	stop_pipe[0] = -1;
	stop_pipe[1] = -1;
	impl_ = impl;
	impl_owner.release();

	{
		std::lock_guard<std::mutex> lock(mutex_);
		buffer_.purge();
		stdout_closed_ = false;
		read_error_ = false;
		reaped_ = false;
		running_ = true;
		last_logged_drops_ = 0;
		diag_tail_.clear();
	}
	try {
#ifdef BONDRIVER_ENABLE_TEST_FAULTS
		if (!processEnvironmentValue("BONDRIVER_FAULT_THREAD_START").empty()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			throw std::bad_alloc();
		}
#endif
		reader_ = std::thread(readerLoop, this, impl, spec.diagnostics);
	} catch (...) {
		const bool stopped = stop(2000, 1000);
		try {
			error = stopped ? "reader thread creation failed" :
			                  "reader thread creation failed and child cleanup failed";
		} catch (...) {}
		return false;
	}
	return true;
}

bool Pipeline::waitReady(uint64_t timeout_ms)
{
	std::unique_lock<std::mutex> lock(mutex_);
	if (buffer_.hasData()) {
		return true;
	}
	if (timeout_ms == 0) {
		return false;
	}
	cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
		return buffer_.hasData() || stdout_closed_ || read_error_ || !running_;
	});
	return buffer_.hasData();
}

bool Pipeline::stop(uint64_t stop_timeout_ms, uint64_t kill_timeout_ms)
{
	Impl *impl = impl_;
	if (impl == nullptr) {
		return !cleanupFailed();
	}
	if (impl->stop_pipe[1] >= 0) {
		const char c = 1;
		(void)!::write(impl->stop_pipe[1], &c, 1);
	}
	if (reader_.joinable()) {
		reader_.join();
	}
	closeIfValid(impl->stdout_fd);
	closeIfValid(impl->stderr_fd);
	closeIfValid(impl->stop_pipe[0]);
	closeIfValid(impl->stop_pipe[1]);
	bool ok = terminateChild(impl->pid, stop_timeout_ms, kill_timeout_ms);
#ifdef BONDRIVER_ENABLE_TEST_FAULTS
	if (ok && !processEnvironmentValue("BONDRIVER_FAULT_STOP").empty()) {
		// Test-only fault injection: report a cleanup failure even though the
		// owned child was reaped.  Used to verify that a cleanup-failure
		// history keeps the lease and does not advertise recovery.
		ok = false;
	}
#endif
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!ok) {
			read_error_ = true;
			cleanup_failed_ = true;
		}
		reaped_ = ok;
		running_ = false;
		stdout_closed_ = true;
		cv_.notify_all();
	}
	delete impl;
	impl_ = nullptr;
	return ok;
}

bool Pipeline::running() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return running_;
}

bool Pipeline::cleanupFailed() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return cleanup_failed_;
}

bool Pipeline::stdoutClosed() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return stdout_closed_;
}

bool Pipeline::readError() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return read_error_;
}

bool Pipeline::reaped() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return reaped_;
}

std::string Pipeline::lastDiagnostics() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return diag_tail_;
}

} // namespace bondriver
