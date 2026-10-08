#include "bondriver/DriverCore.h"

#include "bondriver/DebugLog.h"
#include "bondriver/ModulePath.h"
#include "ProcessEnvironment.h"
#include "bondriver/Utf16.h"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <new>

namespace bondriver {

namespace {

constexpr DWORD kWaitObject0 = 0;
constexpr DWORD kWaitTimeout = 258;
constexpr DWORD kWaitFailed = 0xFFFFFFFFu;
// WaitTsStream never blocks the close path for longer than this, even for an
// INFINITE request; the API documents this bounded-poll behaviour.
constexpr DWORD kMaxWaitMs = 5000;

std::string lockPathFor(const std::string &dir, const std::string &key)
{
	uint64_t hash = 1469598103934665603ull;
	for (unsigned char c : key) {
		hash ^= c;
		hash *= 1099511628211ull;
	}
	char hex[32];
	std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(hash));
	return dir + "/" + hex + ".lock";
}

// ABI shims.  These are written for this implementation; they simply forward
// the C struct slots to the C++ interface so consumers can read the aggregate.
BOOL stOpen(void *p) { return static_cast<IBonDriver *>(p)->OpenTuner(); }
void stClose(void *p) { static_cast<IBonDriver *>(p)->CloseTuner(); }
BOOL stSetByte(void *p, BYTE b) { return static_cast<IBonDriver *>(p)->SetChannel(b); }
float stSignal(void *p) { return static_cast<IBonDriver *>(p)->GetSignalLevel(); }
DWORD stWait(void *p, DWORD t) { return static_cast<IBonDriver *>(p)->WaitTsStream(t); }
DWORD stReady(void *p) { return static_cast<IBonDriver *>(p)->GetReadyCount(); }
BOOL stCopy(void *p, BYTE *d, DWORD *s, DWORD *r) { return static_cast<IBonDriver *>(p)->GetTsStream(d, s, r); }
BOOL stPtr(void *p, BYTE **d, DWORD *s, DWORD *r) { return static_cast<IBonDriver *>(p)->GetTsStream(d, s, r); }
void stPurge(void *p) { static_cast<IBonDriver *>(p)->PurgeTsStream(); }
void stRelease(void *p) { static_cast<IBonDriver *>(p)->Release(); }
const BON16CHAR *stName(void *p)
{
	return static_cast<IBonDriver2 *>(static_cast<IBonDriver *>(p))->GetTunerName();
}
BOOL stOpening(void *p)
{
	return static_cast<IBonDriver2 *>(static_cast<IBonDriver *>(p))->IsTunerOpening();
}
const BON16CHAR *stSpace(void *p, DWORD s)
{
	return static_cast<IBonDriver2 *>(static_cast<IBonDriver *>(p))->EnumTuningSpace(s);
}
const BON16CHAR *stChannel(void *p, DWORD s, DWORD c)
{
	return static_cast<IBonDriver2 *>(static_cast<IBonDriver *>(p))->EnumChannelName(s, c);
}
BOOL stSet2(void *p, DWORD s, DWORD c)
{
	return static_cast<IBonDriver2 *>(static_cast<IBonDriver *>(p))->SetChannel(s, c);
}
DWORD stCurSpace(void *p)
{
	return static_cast<IBonDriver2 *>(static_cast<IBonDriver *>(p))->GetCurSpace();
}
DWORD stCurChannel(void *p)
{
	return static_cast<IBonDriver2 *>(static_cast<IBonDriver *>(p))->GetCurChannel();
}

void structFill(STRUCT_IBONDRIVER2 &st2, IBonDriver2 *core, const void *end)
{
	STRUCT_IBONDRIVER &base = st2.st;
	base.pCtx = core;
	base.pEnd = end != nullptr ? end : &st2 + 1;
	base.pF00 = stOpen;
	base.pF01 = stClose;
	base.pF02 = stSetByte;
	base.pF03 = stSignal;
	base.pF04 = stWait;
	base.pF05 = stReady;
	base.pF06 = stCopy;
	base.pF07 = stPtr;
	base.pF08 = stPurge;
	base.pF09 = stRelease;
	st2.pF10 = stName;
	st2.pF11 = stOpening;
	st2.pF12 = stSpace;
	st2.pF13 = stChannel;
	st2.pF14 = stSet2;
	st2.pF15 = stCurSpace;
	st2.pF16 = stCurChannel;
}

} // namespace

DriverCore::DriverCore(std::shared_ptr<const Config> config, std::shared_ptr<Backend> backend)
	: config_(std::move(config)), backend_(std::move(backend))
{
	{
		std::lock_guard<std::mutex> pl(pipeline_.mutex());
		pipeline_.buffer().setLimit(config_->common.buffer_limit_bytes);
	}
	tuner_name_ = utf8ToUtf16(config_->common.tuner_name);
	for (const Space &space : config_->spaces) {
		space_names_.push_back(utf8ToUtf16(space.name.empty() ? space.id : space.name));
		std::vector<std::u16string> channels;
		for (const ChannelEntry &ch : space.channels) {
			channels.push_back(utf8ToUtf16(ch.name));
		}
		channel_names_.push_back(std::move(channels));
	}
}

DriverCore::~DriverCore()
{
	try {
		// A previous failed cleanup already disposed the pipeline, so a second
		// stop would be vacuously true. Consult the persistent failure history
		// instead of trusting that second call, and never release the lease.
		if (cleanup_failed_) {
			logLine(backend_->tag(), "release with prior cleanup failure; receiver lease kept");
			lease_.abandon();
			return;
		}
		if (!pipeline_.stop(config_->common.stop_timeout_ms, config_->common.kill_timeout_ms)) {
			logLine(backend_->tag(), "cleanup failed during release; receiver lease kept");
			lease_.abandon();
			return;
		}
		releaseTargetLocked();
	} catch (...) {
		lease_.abandon();
	}
}

bool DriverCore::openTunerLocked()
{
	if (cleanup_failed_) {
		logLine(backend_->tag(), "OpenTuner refused: previous cleanup failed");
		return false;
	}
	const std::string &lock_dir = config_->common.lock_dir;
	std::string err;
	if (!ensureDirectory(lock_dir, err)) {
		logLine(backend_->tag(), "cannot create lock dir: " + err);
		return false;
	}
	if (!leased_) {
		if (!claimTargetLocked(false, false)) {
			logLine(backend_->tag(), "no free receiver available");
			return false;
		}
	}
	opened_ = true;
	have_channel_ = false;
	failed_ = false;
	return true;
}

void DriverCore::closeTunerLocked()
{
	if (cleanup_failed_) {
		// A previous cleanup failure already disposes the pipeline; retrying
		// here would be vacuously true and would release an uncertain lease.
		logLine(backend_->tag(), "close after prior cleanup failure; receiver lease kept");
		return;
	}
	if (!pipeline_.stop(config_->common.stop_timeout_ms, config_->common.kill_timeout_ms)) {
		cleanup_failed_ = true;
		failed_ = true;
		have_channel_ = false;
		opened_ = false;
		logLine(backend_->tag(), "cleanup failed during close; receiver lease kept");
		return;
	}
	{
		// The reader is joined by stop(). Clear queued, partial, and handed-out
		// TS data before a later OpenTuner can expose this pipeline again.
		std::lock_guard<std::mutex> pl(pipeline_.mutex());
		pipeline_.buffer().purge();
	}
	releaseTargetLocked();
	opened_ = false;
	have_channel_ = false;
	failed_ = false;
}

bool DriverCore::claimTargetLocked(bool want_t, bool want_s)
{
	const std::string &lock_dir = config_->common.lock_dir;
	const std::vector<ReceiverTarget> &targets = backend_->targets();
	for (int pass = 0; pass < 2; ++pass) {
		for (const ReceiverTarget &target : targets) {
			const bool hybrid = target.hybrid || (target.supports_t && target.supports_s);
			if ((pass == 0 && hybrid) || (pass == 1 && !hybrid)) {
				continue;
			}
			if (want_t && !target.supports_t) {
				continue;
			}
			if (want_s && !target.supports_s) {
				continue;
			}
			std::string err;
			if (lease_.acquire(lockPathFor(lock_dir, target.key), err)) {
				try {
#ifdef BONDRIVER_ENABLE_TEST_FAULTS
					if (!processEnvironmentValue("BONDRIVER_FAULT_AFTER_LEASE").empty()) throw std::bad_alloc();
#endif
					target_ = target;
				} catch (...) {
					lease_.release();
					throw;
				}
				leased_ = true;
				return true;
			}
			if (!err.empty()) {
				logLine(backend_->tag(), "lease error: " + err);
			}
		}
	}
	return false;
}

void DriverCore::releaseTargetLocked()
{
	lease_.release();
	leased_ = false;
	target_ = ReceiverTarget{};
}

void DriverCore::recoverFailedChannelNoThrow() noexcept
{
	try {
		std::lock_guard<std::mutex> lock(mu_);
		failed_ = true;
		have_channel_ = false;
		if (!pipeline_.stop(config_->common.stop_timeout_ms, config_->common.kill_timeout_ms)) {
			cleanup_failed_ = true;
			opened_ = false;
			lease_.abandon();
			return;
		}
		std::lock_guard<std::mutex> pl(pipeline_.mutex());
		pipeline_.buffer().purge();
	} catch (...) {
		cleanup_failed_ = true;
		opened_ = false;
		failed_ = true;
		have_channel_ = false;
		lease_.abandon();
	}
}

void DriverCore::recoverFailedCloseNoThrow() noexcept
{
	try {
		std::lock_guard<std::mutex> lock(mu_);
		opened_ = false;
		failed_ = true;
		have_channel_ = false;
		if (!pipeline_.stop(config_->common.stop_timeout_ms, config_->common.kill_timeout_ms)) {
			cleanup_failed_ = true;
			lease_.abandon();
			return;
		}
		{
			std::lock_guard<std::mutex> pl(pipeline_.mutex());
			pipeline_.buffer().purge();
		}
		releaseTargetLocked();
		cleanup_failed_ = false;
		failed_ = false;
	} catch (...) {
		cleanup_failed_ = true;
		opened_ = false;
		failed_ = true;
		have_channel_ = false;
		lease_.abandon();
	}
}

const BOOL DriverCore::OpenTuner(void) try
{
	std::lock_guard<std::mutex> lock(mu_);
	if (opened_) {
		return TRUE;
	}
	return openTunerLocked() ? TRUE : FALSE;
}
catch (...) {
	recoverFailedCloseNoThrow();
	return FALSE;
}

void DriverCore::CloseTuner(void) try
{
	std::lock_guard<std::mutex> lock(mu_);
	if (!opened_ && !leased_) {
		return;
	}
	closeTunerLocked();
}
catch (...) { recoverFailedCloseNoThrow(); }

bool DriverCore::startChannelLocked(size_t space, size_t channel)
{
	if (cleanup_failed_) {
		return false;
	}
	if (space >= config_->spaces.size() || channel >= config_->spaces[space].channels.size()) {
		failed_ = true;
		return false;
	}
	const ChannelEntry &entry = config_->spaces[space].channels[channel];
	const bool want_t = entry.system == System::IsdbT;
	const bool want_s = entry.system == System::IsdbS;

	if (!pipeline_.stop(config_->common.stop_timeout_ms, config_->common.kill_timeout_ms)) {
		cleanup_failed_ = true;
		failed_ = true;
		have_channel_ = false;
		logLine(backend_->tag(), "cleanup failed before retune; not reusing receiver");
		return false;
	}
	{
		std::lock_guard<std::mutex> pl(pipeline_.mutex());
		pipeline_.buffer().purge();
	}

	if (leased_ && ((want_t && !target_.supports_t) || (want_s && !target_.supports_s))) {
		releaseTargetLocked();
	}
	if (!leased_) {
		if (!claimTargetLocked(want_t, want_s)) {
			logLine(backend_->tag(), "no receiver supports channel '" + entry.id + "'");
			failed_ = true;
			have_channel_ = false;
			return false;
		}
	}

	std::vector<std::string> argv;
	std::string err;
	if (!backend_->buildCommand(entry, target_, argv, err)) {
		logLine(backend_->tag(), "cannot build command: " + err);
		failed_ = true;
		have_channel_ = false;
		return false;
	}
	SpawnSpec spec;
	spec.argv = std::move(argv);
	if (config_->common.diagnostics.rfind("file:", 0) == 0) {
		spec.diagnostics = "file:";
		spec.diagnostics_file = config_->common.diagnostics.substr(5);
	} else {
		spec.diagnostics = config_->common.diagnostics;
	}
	if (!pipeline_.start(spec, err)) {
		logLine(backend_->tag(), "spawn failed: " + err);
		failed_ = true;
		have_channel_ = false;
		if (pipeline_.cleanupFailed()) {
			cleanup_failed_ = true;
			opened_ = false;
			lease_.abandon();
			logLine(backend_->tag(), "reader startup cleanup failed; receiver lease kept");
		}
		return false;
	}
	if (!pipeline_.waitReady(config_->common.tune_timeout_ms)) {
		const std::string diag = pipeline_.lastDiagnostics();
		if (!pipeline_.stop(config_->common.stop_timeout_ms, config_->common.kill_timeout_ms)) {
			cleanup_failed_ = true;
			logLine(backend_->tag(), "cleanup failed after tune timeout; receiver lease kept");
		}
		logLine(backend_->tag(), "tune timeout for '" + entry.id + "'" +
		                          (diag.empty() ? std::string() : (": " + diag.substr(0, 200))));
		failed_ = true;
		have_channel_ = false;
		return false;
	}
	space_ = static_cast<DWORD>(space);
	channel_ = static_cast<DWORD>(channel);
	have_channel_ = true;
	failed_ = false;
	return true;
}

bool DriverCore::restartCurrentLocked()
{
	if (!have_channel_) {
		return false;
	}
	return startChannelLocked(space_, channel_);
}

const BOOL DriverCore::SetChannel(const DWORD dwSpace, const DWORD dwChannel) try
{
	std::lock_guard<std::mutex> lock(mu_);
	if (!opened_) {
		return FALSE;
	}
	return startChannelLocked(dwSpace, dwChannel) ? TRUE : FALSE;
}
catch (...) {
	recoverFailedChannelNoThrow();
	return FALSE;
}

const BOOL DriverCore::SetChannel(const BYTE bCh) try
{
	std::lock_guard<std::mutex> lock(mu_);
	if (!opened_ || config_->spaces.empty()) {
		return FALSE;
	}
	size_t index = bCh;
	if (index >= config_->spaces[0].channels.size()) {
		if (bCh >= 13 && bCh - 13 < config_->spaces[0].channels.size()) {
			index = static_cast<size_t>(bCh - 13);
		} else {
			return FALSE;
		}
	}
	return startChannelLocked(0, index) ? TRUE : FALSE;
}
catch (...) {
	recoverFailedChannelNoThrow();
	return FALSE;
}

const float DriverCore::GetSignalLevel(void)
{
	// The child CLI exposes no measurement under the current contract: CNR is
	// unknown and signalled as 0.0f (see README).
	return 0.0f;
}

const DWORD DriverCore::WaitTsStream(const DWORD dwTimeOut) try
{
	DWORD timeout = dwTimeOut;
	if (timeout == kWaitFailed || timeout > kMaxWaitMs) {
		timeout = kMaxWaitMs;
	}
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (!opened_ || cleanup_failed_ || !pipeline_.running()) {
			return kWaitFailed;
		}
	}
	// Wait without holding mu_ so a concurrent CloseTuner can stop the pipeline
	// and wake this wait.
	pipeline_.waitReady(timeout);
	std::lock_guard<std::mutex> lock(mu_);
	if (!opened_ || cleanup_failed_) {
		return kWaitFailed;
	}
	size_t buffers = 0;
	{
		std::lock_guard<std::mutex> pl(pipeline_.mutex());
		buffers = pipeline_.buffer().readyBufferCount();
	}
	if (buffers > 0) {
		return kWaitObject0;
	}
	if (pipeline_.stdoutClosed() || pipeline_.readError()) {
		failed_ = true;
		have_channel_ = false;
		return kWaitFailed;
	}
	return kWaitTimeout;
}
catch (...) { return kWaitFailed; }

const DWORD DriverCore::GetReadyCount(void) try
{
	std::lock_guard<std::mutex> lock(mu_);
	if (!opened_ || cleanup_failed_) {
		return 0;
	}
	std::lock_guard<std::mutex> pl(pipeline_.mutex());
	return static_cast<DWORD>(pipeline_.buffer().readyBufferCount());
}
catch (...) { return 0; }

const BOOL DriverCore::GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain) try
{
	std::lock_guard<std::mutex> lock(mu_);
	if (pdwSize == nullptr) {
		return FALSE;
	}
	*pdwSize = 0;
	if (pdwRemain != nullptr) {
		*pdwRemain = 0;
	}
	if (pDst == nullptr || !opened_ || cleanup_failed_) {
		return FALSE;
	}
	size_t out = 0;
	size_t remain = 0;
	bool ok = false;
	{
		std::lock_guard<std::mutex> pl(pipeline_.mutex());
		ok = pipeline_.buffer().copyOut(pDst, kMaxGetTsStreamBytes, out, remain);
	}
	*pdwSize = static_cast<DWORD>(out);
	if (pdwRemain != nullptr) {
		*pdwRemain = static_cast<DWORD>(remain);
	}
	return ok ? TRUE : FALSE;
}
catch (...) {
	if (pdwSize != nullptr) *pdwSize = 0;
	if (pdwRemain != nullptr) *pdwRemain = 0;
	return FALSE;
}

const BOOL DriverCore::GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain) try
{
	std::lock_guard<std::mutex> lock(mu_);
	if (ppDst == nullptr || pdwSize == nullptr) {
		return FALSE;
	}
	*ppDst = nullptr;
	*pdwSize = 0;
	if (pdwRemain != nullptr) {
		*pdwRemain = 0;
	}
	if (!opened_ || cleanup_failed_) {
		return FALSE;
	}
	const uint8_t *ptr = nullptr;
	size_t out = 0;
	size_t remain = 0;
	{
		std::lock_guard<std::mutex> pl(pipeline_.mutex());
		ptr = pipeline_.buffer().takePointer(out, remain);
	}
	*ppDst = const_cast<BYTE *>(ptr);
	*pdwSize = static_cast<DWORD>(out);
	if (pdwRemain != nullptr) {
		*pdwRemain = static_cast<DWORD>(remain);
	}
	return (ptr != nullptr && out != 0) ? TRUE : FALSE;
}
catch (...) {
	if (ppDst != nullptr) *ppDst = nullptr;
	if (pdwSize != nullptr) *pdwSize = 0;
	if (pdwRemain != nullptr) *pdwRemain = 0;
	return FALSE;
}

void DriverCore::PurgeTsStream(void) try
{
	std::lock_guard<std::mutex> lock(mu_);
	{
		std::lock_guard<std::mutex> pl(pipeline_.mutex());
		pipeline_.buffer().purge();
	}
	if (!opened_ || cleanup_failed_) {
		return;
	}
	if (have_channel_) {
		if (!restartCurrentLocked()) {
			logLine(backend_->tag(), "purge failed: receiver state cleared");
		}
	}
}
catch (...) { recoverFailedChannelNoThrow(); }

void DriverCore::Release(void) try
{
	CloseTuner();
	delete this;
}
catch (...) {}

const BON16CHAR *DriverCore::GetTunerName(void) try
{
	return reinterpret_cast<const BON16CHAR *>(tuner_name_.c_str());
}
catch (...) { return nullptr; }

const BOOL DriverCore::IsTunerOpening(void) try
{
	std::lock_guard<std::mutex> lock(mu_);
	return opened_ ? TRUE : FALSE;
}
catch (...) { return FALSE; }

const BON16CHAR *DriverCore::EnumTuningSpace(const DWORD dwSpace) try
{
	std::lock_guard<std::mutex> lock(mu_);
	if (dwSpace >= space_names_.size()) {
		return nullptr;
	}
	return reinterpret_cast<const BON16CHAR *>(space_names_[dwSpace].c_str());
}
catch (...) { return nullptr; }

const BON16CHAR *DriverCore::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel) try
{
	std::lock_guard<std::mutex> lock(mu_);
	if (dwSpace >= channel_names_.size() || dwChannel >= channel_names_[dwSpace].size()) {
		return nullptr;
	}
	return reinterpret_cast<const BON16CHAR *>(channel_names_[dwSpace][dwChannel].c_str());
}
catch (...) { return nullptr; }

bool DriverCore::currentChannelValidLocked()
{
	if (!opened_ || !have_channel_ || failed_ || cleanup_failed_) {
		return false;
	}
	if (pipeline_.stdoutClosed() || pipeline_.readError()) {
		failed_ = true;
		have_channel_ = false;
		return false;
	}
	return true;
}

const DWORD DriverCore::GetCurSpace(void) try
{
	std::lock_guard<std::mutex> lock(mu_);
	if (!currentChannelValidLocked()) {
		return BONDRIVER_SPACE_INVALID;
	}
	return space_;
}
catch (...) { return BONDRIVER_SPACE_INVALID; }

const DWORD DriverCore::GetCurChannel(void) try
{
	std::lock_guard<std::mutex> lock(mu_);
	if (!currentChannelValidLocked()) {
		return BONDRIVER_CHANNEL_INVALID;
	}
	return channel_;
}
catch (...) { return BONDRIVER_CHANNEL_INVALID; }

const LibraryState &libraryState(BackendKind kind)
{
	static LibraryState state;
	static std::once_flag once;
	std::call_once(once, [kind]() {
		const std::string module_path = currentModulePath();
		auto config = std::make_shared<Config>();
		std::string error;
		if (!loadConfig(kind, module_path, "", *config, error)) {
			state.valid = false;
			state.error = error;
			logLine("bondriver", "configuration error: " + error);
			return;
		}
		state.config = config;
		state.backend = createBackend(*config);
		state.valid = true;
	});
	return state;
}

IBonDriver2 *createDriverObject(BackendKind kind) try
{
#ifdef BONDRIVER_ENABLE_TEST_FAULTS
	if (!processEnvironmentValue("BONDRIVER_FAULT_FACTORY").empty()) throw std::bad_alloc();
#endif
	const LibraryState &state = libraryState(kind);
	if (!state.valid) {
		logLine("bondriver", "CreateBonDriver failed: " + state.error);
		return nullptr;
	}
	return new DriverCore(state.config, state.backend);
}
catch (...) { return nullptr; }

namespace {

struct StructHolder {
	std::mutex mu;
	STRUCT_IBONDRIVER2 st2;
	IBonDriver *core = nullptr;
	bool active = false;
};

StructHolder &structHolder()
{
	static StructHolder holder;
	return holder;
}

void structRelease(void *p) try
{
	static_cast<IBonDriver *>(p)->Release();
	StructHolder &holder = structHolder();
	std::lock_guard<std::mutex> lock(holder.mu);
	holder.core = nullptr;
	holder.active = false;
}
catch (...) {}

} // namespace

const STRUCT_IBONDRIVER *createDriverStruct(BackendKind kind) try
{
#ifdef BONDRIVER_ENABLE_TEST_FAULTS
	if (!processEnvironmentValue("BONDRIVER_FAULT_FACTORY").empty()) throw std::bad_alloc();
#endif
	const LibraryState &state = libraryState(kind);
	if (!state.valid) {
		logLine("bondriver", "CreateBonStruct failed: " + state.error);
		return nullptr;
	}
	StructHolder &holder = structHolder();
	std::lock_guard<std::mutex> lock(holder.mu);
	if (holder.active) {
		// Documented: CreateBonStruct is a singleton per library; repeated
		// calls return the same structure while one remains unreleased.
		return &holder.st2.st;
	}
	auto *core = new DriverCore(state.config, state.backend);
	holder.st2 = STRUCT_IBONDRIVER2{};
	structFill(holder.st2, core, &holder.st2 + 1);
	holder.st2.st.pF09 = structRelease;
	holder.core = core;
	holder.active = true;
	return &holder.st2.st;
}
catch (...) { return nullptr; }

} // namespace bondriver
