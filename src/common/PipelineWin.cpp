// Windows implementation of Pipeline.
//
// Uses CreateProcessW with an explicit inheritable-handle list
// (PROC_THREAD_ATTRIBUTE_HANDLE_LIST) so only the child's std handles cross the
// process boundary, a Job object with KILL_ON_JOB_CLOSE to bound descendant
// cleanup, and a manual-reset stop event so the reader thread can be cancelled
// without closing a pipe under an in-flight read.
#include "bondriver/Pipeline.h"
#include "bondriver/DebugLog.h"

#include <windows.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace bondriver {

struct Pipeline::Impl {
	HANDLE process = nullptr;
	DWORD pid = 0;
	HANDLE stdout_read = nullptr;
	HANDLE stderr_read = nullptr;
	HANDLE stop_event = nullptr;
	HANDLE job = nullptr;
	std::string diag_mode;
};

namespace {

constexpr DWORD kMaxStdoutPerIteration = 1u << 20;
constexpr DWORD kMaxStderrPerIteration = 1u << 16;

std::wstring toWide(const std::string &utf8)
{
	if (utf8.empty()) {
		return std::wstring();
	}
	const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
	if (size <= 0) {
		return std::wstring();
	}
	std::wstring out(static_cast<size_t>(size), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &out[0], size);
	return out;
}

std::wstring quoteArg(const std::wstring &arg)
{
	std::wstring out = L"\"";
	size_t backslashes = 0;
	for (wchar_t c : arg) {
		if (c == L'\\') {
			++backslashes;
			continue;
		}
		if (c == L'"') {
			out.append(backslashes * 2 + 1, L'\\');
			out.push_back(L'"');
			backslashes = 0;
			continue;
		}
		out.append(backslashes, L'\\');
		backslashes = 0;
		out.push_back(c);
	}
	out.append(backslashes * 2, L'\\');
	out.push_back(L'"');
	return out;
}

std::wstring buildCommandLine(const std::vector<std::string> &argv)
{
	std::wstring line;
	for (size_t i = 0; i < argv.size(); ++i) {
		if (i != 0) {
			line.push_back(L' ');
		}
		line += quoteArg(toWide(argv[i]));
	}
	return line;
}

bool readAvailable(HANDLE pipe, char *buf, DWORD cap, DWORD &read, bool &broken)
{
	read = 0;
	broken = false;
	DWORD avail = 0;
	if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) {
		broken = true;
		return false;
	}
	if (avail == 0) {
		return true;
	}
	DWORD want = avail < cap ? avail : cap;
	if (!ReadFile(pipe, buf, want, &read, nullptr)) {
		if (GetLastError() == ERROR_BROKEN_PIPE) {
			broken = true;
		}
		return false;
	}
	return true;
}

void readerLoop(Pipeline *self, Pipeline::Impl *impl)
{
	char buf[65536];
	bool broken = false;
	for (;;) {
		if (WaitForSingleObject(impl->stop_event, 0) == WAIT_OBJECT_0) {
			break;
		}
		DWORD stdout_budget = kMaxStdoutPerIteration;
		DWORD stderr_budget = kMaxStderrPerIteration;
		bool stderr_broken = false;
		while (impl->stderr_read != nullptr && stderr_budget > 0) {
			const DWORD cap = stderr_budget < sizeof buf ? stderr_budget : static_cast<DWORD>(sizeof buf);
			DWORD got = 0;
			if (!readAvailable(impl->stderr_read, buf, cap, got, stderr_broken) || got == 0) break;
			appendDiagnosticText(std::string(buf, got));
			std::string tail = self->readerGetDiagTail();
			tail.append(buf, got);
			if (tail.size() > 8192) tail.erase(0, tail.size() - 8192);
			self->readerSetDiagTail(tail);
			stderr_budget -= got;
		}
		if (stderr_broken && impl->stderr_read != nullptr) {
			CloseHandle(impl->stderr_read);
			impl->stderr_read = nullptr;
		}
		while (stdout_budget > 0) {
			const DWORD cap = stdout_budget < sizeof buf ? stdout_budget : static_cast<DWORD>(sizeof buf);
			DWORD outRead = 0;
			if (!readAvailable(impl->stdout_read, buf, cap, outRead, broken) || outRead == 0) {
				break;
			}
			self->readerAppendTs(reinterpret_cast<const uint8_t *>(buf), outRead);
			stdout_budget -= outRead;
		}
		if (broken) {
			break;
		}
		WaitForSingleObject(impl->stop_event, 10);
	}
	self->readerMarkClosed(true, false);
}

} // namespace

Pipeline::Pipeline() = default;

Pipeline::~Pipeline()
{
	stop(2000, 1000);
}

bool Pipeline::start(const SpawnSpec &spec, std::string &error)
{
	if (running_) {
		error = "pipeline already running";
		return false;
	}
	auto *impl = new Impl();
	impl->diag_mode = spec.diagnostics;
	impl->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (impl->stop_event == nullptr) {
		error = "CreateEvent failed";
		delete impl;
		return false;
	}

	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof sa;
	sa.bInheritHandle = TRUE;
	HANDLE out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr, child_err = nullptr;
	if (!CreatePipe(&out_r, &out_w, &sa, 1 << 20)) {
		error = "CreatePipe(stdout) failed";
		CloseHandle(impl->stop_event);
		delete impl;
		return false;
	}
	SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
	if (!CreatePipe(&err_r, &err_w, &sa, 1 << 16)) {
		CloseHandle(out_r); CloseHandle(out_w); CloseHandle(impl->stop_event);
		error = "CreatePipe(stderr) failed";
		delete impl;
		return false;
	}
	SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
	SECURITY_ATTRIBUTES child_sa{};
	child_sa.nLength = sizeof child_sa;
	child_sa.bInheritHandle = TRUE;
	if (spec.diagnostics == "stderr") {
		child_err = err_w;
	} else if (spec.diagnostics == "discard") {
		child_err = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &child_sa,
		                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	} else if (spec.diagnostics.rfind("file:", 0) == 0 && !spec.diagnostics_file.empty()) {
		const std::wstring path = toWide(spec.diagnostics_file);
		child_err = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, &child_sa, OPEN_ALWAYS,
		                        FILE_ATTRIBUTE_NORMAL, nullptr);
		if (child_err != INVALID_HANDLE_VALUE && child_err != nullptr && GetFileType(child_err) != FILE_TYPE_DISK) {
			CloseHandle(child_err);
			child_err = nullptr;
		}
	} else {
		child_err = nullptr;
	}
	if (child_err == nullptr || child_err == INVALID_HANDLE_VALUE) {
		if (out_r) CloseHandle(out_r);
		if (out_w) CloseHandle(out_w);
		if (err_r) CloseHandle(err_r);
		if (err_w) CloseHandle(err_w);
		CloseHandle(impl->stop_event);
		error = "cannot prepare child diagnostic handle";
		delete impl;
		return false;
	}
	bool job_ok = false;
	impl->job = CreateJobObjectW(nullptr, nullptr);
	if (impl->job != nullptr) {
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
		info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		job_ok = SetInformationJobObject(impl->job, JobObjectExtendedLimitInformation, &info, sizeof info) != FALSE;
	}

	std::wstring command_line = buildCommandLine(spec.argv);
	std::vector<wchar_t> cmd(command_line.begin(), command_line.end());
	cmd.push_back(L'\0');

	STARTUPINFOEXW si{};
	si.StartupInfo.cb = sizeof si;
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
	si.StartupInfo.hStdOutput = out_w;
	si.StartupInfo.hStdError = child_err;

	HANDLE inherit_handles[2] = {out_w, child_err};
	SIZE_T attr_size = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
	std::vector<char> attr_buf(attr_size);
	si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
	bool list_ok = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size) != FALSE;
	if (list_ok) {
		list_ok = UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit_handles,
		                                    sizeof inherit_handles, nullptr, nullptr) != FALSE;
	}

	PROCESS_INFORMATION pi{};
	const BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
	                                    CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
	                                    nullptr, &si.StartupInfo, &pi);
	if (si.lpAttributeList != nullptr) {
		DeleteProcThreadAttributeList(si.lpAttributeList);
	}
	CloseHandle(out_w);
	if (child_err != err_w) CloseHandle(child_err);
	CloseHandle(err_w);

	auto failStartup = [&](const char *what) {
		if (created) {
			TerminateProcess(pi.hProcess, 1);
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}
		CloseHandle(out_r);
		CloseHandle(err_r);
		CloseHandle(impl->stop_event);
		if (impl->job != nullptr) {
			CloseHandle(impl->job);
		}
		error = std::string(what) + " failed: " + std::to_string(GetLastError());
		delete impl;
		return false;
	};

	if (!created || !list_ok) {
		return failStartup("CreateProcess");
	}
	// Job ownership is required for bounded descendant cleanup; failing it must
	// fail startup rather than run without a Job.
	if (!job_ok || !AssignProcessToJobObject(impl->job, pi.hProcess)) {
		return failStartup("Job assignment");
	}
	if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
		return failStartup("ResumeThread");
	}
	CloseHandle(pi.hThread);

	impl->process = pi.hProcess;
	impl->pid = pi.dwProcessId;
	impl->stdout_read = out_r;
	impl->stderr_read = err_r;
	impl_ = impl;
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
	reader_ = std::thread(readerLoop, this, impl);
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
		return true;
	}
	SetEvent(impl->stop_event);
	if (reader_.joinable()) {
		reader_.join();
	}
	if (impl->stdout_read != nullptr) {
		CloseHandle(impl->stdout_read);
	}
	if (impl->stderr_read != nullptr) {
		CloseHandle(impl->stderr_read);
	}
	if (impl->stop_event != nullptr) {
		CloseHandle(impl->stop_event);
	}
	bool ok = true;
	if (impl->process != nullptr) {
		if (WaitForSingleObject(impl->process, static_cast<DWORD>(stop_timeout_ms)) == WAIT_TIMEOUT) {
			TerminateProcess(impl->process, 1);
			if (WaitForSingleObject(impl->process, static_cast<DWORD>(kill_timeout_ms)) == WAIT_TIMEOUT) {
				ok = false;
			}
		}
		CloseHandle(impl->process);
	}
	if (impl->job != nullptr) {
		CloseHandle(impl->job);
	}
#ifdef BONDRIVER_ENABLE_TEST_FAULTS
	if (ok && std::getenv("BONDRIVER_FAULT_STOP") != nullptr) {
		// Test-only fault injection, mirroring the POSIX implementation.
		ok = false;
	}
#endif
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!ok) {
			read_error_ = true;
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
