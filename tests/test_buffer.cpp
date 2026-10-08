#include "bondriver/TsBuffer.h"
#include "test_util.h"

#include <cstdint>
#include <vector>

using namespace bondriver;

namespace {

std::vector<uint8_t> makeData(size_t bytes, uint8_t seed)
{
	std::vector<uint8_t> data(bytes);
	for (size_t i = 0; i < bytes; ++i) {
		data[i] = static_cast<uint8_t>(seed + i);
	}
	return data;
}

} // namespace

int main()
{
	{
		TsBuffer buffer(8u * 1024u * 1024u);
		auto partial = makeData(100, 1);
		buffer.append(partial.data(), partial.size());
		testutil::expect(!buffer.hasData(), "partial packet is not exposed");
		testutil::expect(buffer.readyBufferCount() == 0, "no acquisition buffer for a partial packet");
		auto rest = makeData(88, 2);
		buffer.append(rest.data(), rest.size());
		testutil::expect(buffer.hasData(), "split read reassembles to a whole packet");
		testutil::expect(buffer.readyBufferCount() == 1, "one acquisition buffer ready");
	}

	{
		// One GetTsStream call returns exactly one acquisition buffer, and the
		// buffer count matches the number of retrievable blocks.
		TsBuffer buffer(8u * 1024u * 1024u);
		auto data = makeData(kMaxTsBlockBytes * 3 + 100, 0x47);
		buffer.append(data.data(), data.size());
		testutil::expect(buffer.readyBufferCount() == 3, "three full acquisition buffers");
		std::vector<uint8_t> dst(kMaxGetTsStreamBytes, 0);
		size_t out = 0;
		size_t remain = 0;
		testutil::expect(buffer.copyOut(dst.data(), kMaxGetTsStreamBytes, out, remain), "copy one buffer");
		testutil::expect(out == kMaxTsBlockBytes, "returned buffer equals max block size");
		testutil::expect(remain == 2, "remain is the number of remaining buffers");
		const uint8_t *ptr = buffer.takePointer(out, remain);
		testutil::expect(ptr != nullptr && out == kMaxTsBlockBytes, "pointer one buffer");
		testutil::expect(remain == 1, "remain decremented by one buffer");
		std::vector<uint8_t> copy(ptr, ptr + out);
		auto more = makeData(kMaxTsBlockBytes, 0x99);
		buffer.append(more.data(), more.size());
		bool stable = true;
		for (size_t i = 0; i < out; ++i) {
			if (copy[i] != ptr[i]) {
				stable = false;
				break;
			}
		}
		testutil::expect(stable, "handed pointer stays valid across later appends");
		buffer.takePointer(out, remain);
		testutil::expect(remain == 1, "partial tail is not counted as a buffer");
	}

	{
		TsBuffer buffer(188 * 20);
		auto data = makeData(188 * 100, 0);
		buffer.append(data.data(), data.size());
		testutil::expect(buffer.readyBufferCount() <= 20, "bounded queue caps acquisition buffers");
		testutil::expect(buffer.droppedPackets() > 0, "overflow drops whole packets");
	}

	{
		TsBuffer buffer(8u * 1024u * 1024u);
		auto partial = makeData(100, 0);
		buffer.append(partial.data(), partial.size());
		buffer.purge();
		auto rest = makeData(88, 0);
		buffer.append(rest.data(), rest.size());
		testutil::expect(!buffer.hasData(), "purge discards trailing partial");
	}

	{
		TsBuffer buffer(8u * 1024u * 1024u);
		auto data = makeData(kMaxTsBlockBytes, 0x47);
		buffer.append(data.data(), data.size());
		std::vector<uint8_t> dst(kMaxGetTsStreamBytes + 32, 0xEE);
		size_t out = 0;
		size_t remain = 0;
		const bool ok = buffer.copyOut(dst.data(), kMaxGetTsStreamBytes, out, remain);
		testutil::expect(ok, "copy out succeeds");
		testutil::expect(out <= kMaxGetTsStreamBytes, "copy never exceeds documented maximum");
		testutil::expect(out % 188 == 0, "copy is packet aligned");
		bool canary = true;
		for (size_t i = out; i < dst.size(); ++i) {
			if (dst[i] != 0xEE) {
				canary = false;
				break;
			}
		}
		testutil::expect(canary, "copy never writes past reported size");
	}

	return testutil::report("test_buffer");
}
