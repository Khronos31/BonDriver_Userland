// Owned child process pipeline: argc argv spawn (no shell), stdout as TS,
// stderr as diagnostics, bounded reader and bounded shutdown.
#pragma once

#include "bondriver/TsBuffer.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bondriver {

struct SpawnSpec {
	std::vector<std::string> argv;
	std::string diagnostics = "stderr"; // "stderr" | "discard" | "file:PATH"
	std::string diagnostics_file;       // resolved path when diagnostics is file:
};

class Pipeline {
public:
	Pipeline();
	~Pipeline();

	Pipeline(const Pipeline &) = delete;
	Pipeline &operator=(const Pipeline &) = delete;

	// Starts the child.  Returns false (and *error) if spawn fails.  Must not
	// be called while running.
	bool start(const SpawnSpec &spec, std::string &error);

	// Waits until at least one whole packet is buffered, the child closes
	// stdout, or the timeout elapses.  Returns true when data is ready.
	bool waitReady(uint64_t timeout_ms);

	// Bounded shutdown: SIGTERM (TerminateProcess on Windows), then SIGKILL
	// after stop_timeout_ms, reaping within kill_timeout_ms.  Returns true when
	// the owned child was reaped.
	bool stop(uint64_t stop_timeout_ms, uint64_t kill_timeout_ms);

	bool running() const;
	bool stdoutClosed() const;
	bool readError() const;
	bool reaped() const;
	std::string lastDiagnostics() const;

	// Buffer access.  Callers must hold mutex().
	TsBuffer &buffer() { return buffer_; }
	std::mutex &mutex() { return mutex_; }
	std::condition_variable &cv() { return cv_; }

	// Reader-thread helpers (lock internally).
	void readerAppendTs(const uint8_t *data, size_t n);
	void readerSetDiagTail(const std::string &tail);
	std::string readerGetDiagTail();
	void readerMarkClosed(bool eof, bool read_error);

	static void sleepMillis(uint64_t ms);

	// Platform implementation detail (defined in PipelinePosix/PipelineWin).
	struct Impl;

private:
	Impl *impl_ = nullptr;

	TsBuffer buffer_;
	mutable std::mutex mutex_;
	std::condition_variable cv_;
	bool stdout_closed_ = false;
	bool read_error_ = false;
	bool reaped_ = true;
	bool running_ = false;
	uint64_t last_logged_drops_ = 0;
	std::string diag_tail_;
	std::thread reader_;

	friend struct Pipeline::Impl;
};

} // namespace bondriver
