// Platform-independent Pipeline helpers shared by the POSIX and Windows
// implementations.
#include "bondriver/Pipeline.h"

#include <chrono>
#include <thread>

namespace bondriver {

void Pipeline::sleepMillis(uint64_t ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void Pipeline::readerAppendTs(const uint8_t *data, size_t n)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		buffer_.append(data, n);
		const uint64_t dropped = buffer_.droppedPackets();
		if (dropped > last_logged_drops_) {
			// Announce overflow diagnostics at most once per 1000 dropped
			// packets so a flood cannot itself become a log flood.
			if (dropped - last_logged_drops_ >= 1000 || last_logged_drops_ == 0) {
				last_logged_drops_ = dropped;
				const std::string note = "[bondriver] TS buffer overflow: dropped " +
				                         std::to_string(dropped) + " packets total\n";
				diag_tail_ += note;
				if (diag_tail_.size() > 8192) {
					diag_tail_.erase(0, diag_tail_.size() - 8192);
				}
			}
		}
		cv_.notify_all();
	}
}

void Pipeline::readerSetDiagTail(const std::string &tail)
{
	std::lock_guard<std::mutex> lock(mutex_);
	diag_tail_ = tail;
	if (diag_tail_.size() > 8192) {
		diag_tail_.erase(0, diag_tail_.size() - 8192);
	}
}

std::string Pipeline::readerGetDiagTail()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return diag_tail_;
}

void Pipeline::readerMarkClosed(bool eof, bool read_error)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (eof || read_error) {
		stdout_closed_ = eof;
	}
	if (read_error) {
		read_error_ = true;
	}
	cv_.notify_all();
}

} // namespace bondriver
