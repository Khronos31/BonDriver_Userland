#include "bondriver/TsBuffer.h"

#include <algorithm>
#include <cstring>

namespace bondriver {

TsBuffer::TsBuffer(size_t limit_bytes) : limit_(limit_bytes) {}

void TsBuffer::setLimit(size_t limit_bytes)
{
	limit_ = limit_bytes;
}

bool TsBuffer::hasData() const
{
	return !blocks_.empty() || current_.size() >= kTsPacketSize;
}

size_t TsBuffer::readyBufferCount() const
{
	return blocks_.size() + (current_.size() >= kTsPacketSize ? 1u : 0u);
}

void TsBuffer::append(const uint8_t *data, size_t n)
{
	if (n == 0) {
		return;
	}
	size_t off = 0;
	if (current_.empty()) {
		// Fast path: carve whole blocks straight out of the incoming bytes.
		while (n - off >= kMaxTsBlockBytes) {
			blocks_.emplace_back(data + off, data + off + kMaxTsBlockBytes);
			off += kMaxTsBlockBytes;
		}
	}
	if (off < n) {
		current_.insert(current_.end(), data + off, data + n);
	}
	while (current_.size() >= kMaxTsBlockBytes) {
		blocks_.emplace_back(current_.begin(), current_.begin() + static_cast<std::ptrdiff_t>(kMaxTsBlockBytes));
		current_.erase(current_.begin(), current_.begin() + static_cast<std::ptrdiff_t>(kMaxTsBlockBytes));
	}
	enforceLimit();
}

std::vector<uint8_t> TsBuffer::takeBlock()
{
	if (!blocks_.empty()) {
		std::vector<uint8_t> block = std::move(blocks_.front());
		blocks_.pop_front();
		return block;
	}
	const size_t whole = (current_.size() / kTsPacketSize) * kTsPacketSize;
	if (whole == 0) {
		return {};
	}
	std::vector<uint8_t> block(current_.begin(), current_.begin() + static_cast<std::ptrdiff_t>(whole));
	current_.erase(current_.begin(), current_.begin() + static_cast<std::ptrdiff_t>(whole));
	return block;
}

bool TsBuffer::copyOut(uint8_t *dst, size_t max_bytes, size_t &out_bytes, size_t &remain_buffers)
{
	out_bytes = 0;
	if (dst == nullptr || max_bytes < kTsPacketSize) {
		remain_buffers = readyBufferCount();
		return false;
	}
	max_bytes -= max_bytes % kTsPacketSize;
	std::vector<uint8_t> block = takeBlock();
	if (!block.empty()) {
		const size_t n = std::min(block.size(), max_bytes);
		std::memcpy(dst, block.data(), n);
		out_bytes = n;
	}
	remain_buffers = readyBufferCount();
	return out_bytes != 0;
}

const uint8_t *TsBuffer::takePointer(size_t &out_bytes, size_t &remain_buffers)
{
	handed_ = takeBlock();
	out_bytes = handed_.size();
	remain_buffers = readyBufferCount();
	return handed_.empty() ? nullptr : handed_.data();
}

void TsBuffer::purge()
{
	blocks_.clear();
	current_.clear();
	handed_.clear();
}

void TsBuffer::enforceLimit()
{
	size_t total = current_.size();
	for (const std::vector<uint8_t> &block : blocks_) {
		total += block.size();
	}
	while (total > limit_ && !blocks_.empty()) {
		const size_t size = blocks_.front().size();
		total -= size;
		dropped_bytes_ += size;
		dropped_packets_ += size / kTsPacketSize;
		blocks_.pop_front();
	}
	if (total > limit_) {
		const size_t excess = total - limit_;
		const size_t whole = (current_.size() / kTsPacketSize) * kTsPacketSize;
		size_t drop = ((excess + kTsPacketSize - 1) / kTsPacketSize) * kTsPacketSize;
		if (drop > whole) {
			drop = whole;
		}
		if (drop != 0) {
			current_.erase(current_.begin(), current_.begin() + static_cast<std::ptrdiff_t>(drop));
			dropped_bytes_ += drop;
			dropped_packets_ += drop / kTsPacketSize;
			total -= drop;
		}
		if (total > limit_) {
			dropped_bytes_ += current_.size();
			current_.clear();
		}
	}
}

} // namespace bondriver
