// Bounded MPEG-TS acquisition-buffer queue.
//
// The queue holds whole "acquisition buffers" (blocks) of 188-byte aligned TS
// data.  GetReadyCount/GetTsStream report and return acquisition-buffer counts,
// not bytes and not individual packets: one GetTsStream call returns exactly one
// acquisition buffer.  The public maximum block size is also the maximum copy
// size accepted by the copy form of GetTsStream.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace bondriver {

constexpr size_t kTsPacketSize = 188;
// Public maximum acquisition block size.  A block is always a multiple of 188
// and never larger than this value.
constexpr size_t kMaxTsBlockBytes = kTsPacketSize * 512;
// Maximum number of bytes the copy form of GetTsStream writes per call.
constexpr size_t kMaxGetTsStreamBytes = kMaxTsBlockBytes;

class TsBuffer {
public:
	explicit TsBuffer(size_t limit_bytes = 8u * 1024u * 1024u);

	void setLimit(size_t limit_bytes);
	size_t limit() const { return limit_; }

	// Appends raw child output.  A trailing partial packet is retained until a
	// later append completes it.
	void append(const uint8_t *data, size_t n);

	// True when at least one whole packet is available.
	bool hasData() const;

	// Number of ready acquisition buffers.
	size_t readyBufferCount() const;

	// Copy form: copies one acquisition buffer into dst (never more than
	// max_bytes and always 188 aligned).  out_bytes and remain_buffers are
	// outputs; *size is never read as an input capacity.
	bool copyOut(uint8_t *dst, size_t max_bytes, size_t &out_bytes, size_t &remain_buffers);

	// Pointer form: returns one acquisition buffer.  The pointer stays valid
	// until the next copyOut/takePointer/purge call.
	const uint8_t *takePointer(size_t &out_bytes, size_t &remain_buffers);

	// Discards all queued blocks and any trailing partial packet.
	void purge();

	uint64_t droppedPackets() const { return dropped_packets_; }
	uint64_t droppedBytes() const { return dropped_bytes_; }

private:
	std::vector<uint8_t> takeBlock();
	void enforceLimit();

	std::deque<std::vector<uint8_t>> blocks_;
	std::vector<uint8_t> current_;
	std::vector<uint8_t> handed_;
	size_t limit_;
	uint64_t dropped_packets_ = 0;
	uint64_t dropped_bytes_ = 0;
};

} // namespace bondriver
