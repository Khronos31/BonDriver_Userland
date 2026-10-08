// Cooperative inter-process lease backed by a lock file.
//
// POSIX uses flock(2); the lock is released by the kernel if the owning
// process dies, which is what lets a SIGKILLed parent's lease recover.
// Windows uses a zero-share CreateFile handle.
#pragma once

#include <string>

namespace bondriver {

class FileLock {
public:
	FileLock() = default;
	~FileLock();

	FileLock(const FileLock &) = delete;
	FileLock &operator=(const FileLock &) = delete;

	// Attempts a non-blocking exclusive acquire.  When the file already exists
	// and is locked by another owner, returns false and leaves this unlocked.
	bool acquire(const std::string &path, std::string &error);
	void release();
	// Drops this handle without unlocking, so a lease that could not be safely
	// released (child not reaped) stays held until process exit.
	void abandon();
	bool held() const;

private:
#ifdef _WIN32
	void *handle_ = nullptr;
#else
	int fd_ = -1;
#endif
};

// Creates the directory holding leases (and parents) if needed.
bool ensureDirectory(const std::string &path, std::string &error);

} // namespace bondriver
