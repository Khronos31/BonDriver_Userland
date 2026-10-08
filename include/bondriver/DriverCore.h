// IBonDriver2 implementation over one owned child process and one receiver
// lease.  No extra virtual functions are introduced, so the inherited vtable
// order matches EDCB/MSVC expectations exactly.
#pragma once

#include "bondriver/Abi.h"
#include "bondriver/Backend.h"
#include "bondriver/Config.h"
#include "bondriver/FileLock.h"
#include "bondriver/Pipeline.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bondriver {

class DriverCore : public IBonDriver2
{
public:
	DriverCore(std::shared_ptr<const Config> config, std::shared_ptr<Backend> backend);
	~DriverCore();

	// IBonDriver
	const BOOL OpenTuner(void) override;
	void CloseTuner(void) override;
	const BOOL SetChannel(const BYTE bCh) override;
	const float GetSignalLevel(void) override;
	const DWORD WaitTsStream(const DWORD dwTimeOut) override;
	const DWORD GetReadyCount(void) override;
	const BOOL GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain) override;
	const BOOL GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain) override;
	void PurgeTsStream(void) override;
	void Release(void) override;

	// IBonDriver2
	const BON16CHAR *GetTunerName(void) override;
	const BOOL IsTunerOpening(void) override;
	const BON16CHAR *EnumTuningSpace(const DWORD dwSpace) override;
	const BON16CHAR *EnumChannelName(const DWORD dwSpace, const DWORD dwChannel) override;
	const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel) override;
	const DWORD GetCurSpace(void) override;
	const DWORD GetCurChannel(void) override;

private:
	bool openTunerLocked();
	void closeTunerLocked();
	bool claimTargetLocked(bool want_t, bool want_s);
	void releaseTargetLocked();
	bool startChannelLocked(size_t space, size_t channel);
	bool restartCurrentLocked();
	bool currentChannelValidLocked();

	std::shared_ptr<const Config> config_;
	std::shared_ptr<Backend> backend_;

	mutable std::mutex mu_;
	Pipeline pipeline_;
	FileLock lease_;
	ReceiverTarget target_;
	bool opened_ = false;
	bool leased_ = false;
	bool have_channel_ = false;
	bool failed_ = false;
	// Set when a child could not be reaped.  The receiver lease is then kept
	// (never advertised as reusable) and further opens are refused.
	bool cleanup_failed_ = false;
	DWORD space_ = 0;
	DWORD channel_ = 0;

	std::u16string tuner_name_;
	std::vector<std::u16string> space_names_;
	std::vector<std::vector<std::u16string>> channel_names_;
};

// Library-wide state: config and backend loaded once per shared library.
struct LibraryState {
	bool valid = false;
	std::string error;
	std::shared_ptr<const Config> config;
	std::shared_ptr<Backend> backend;
};

const LibraryState &libraryState(BackendKind kind);

// Factory helpers used by the per-backend export translation units.
IBonDriver2 *createDriverObject(BackendKind kind);
const STRUCT_IBONDRIVER *createDriverStruct(BackendKind kind);

} // namespace bondriver
