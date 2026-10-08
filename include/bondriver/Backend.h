// Backend-specific command construction and receiver target list.
#pragma once

#include "bondriver/Config.h"

#include <memory>
#include <string>
#include <vector>

namespace bondriver {

class Backend {
public:
	virtual ~Backend() = default;

	// Ordered list of receivers that may be leased.  The core tries dedicated
	// (single-system) targets before hybrid ones.
	virtual const std::vector<ReceiverTarget> &targets() const = 0;

	// Builds the child argv for the given channel on a claimed target.  The
	// returned vector is passed directly to posix_spawn/CreateProcess.
	virtual bool buildCommand(const ChannelEntry &channel, const ReceiverTarget &target,
	                          std::vector<std::string> &argv, std::string &error) const = 0;

	// Prefix used when tagging diagnostics, e.g. "siano" or "px4".
	virtual const char *tag() const = 0;
};

std::shared_ptr<Backend> createBackend(const Config &config);

} // namespace bondriver
