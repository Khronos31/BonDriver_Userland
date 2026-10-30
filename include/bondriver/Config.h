// Configuration and external channel-table model.
//
// A library reads an INI file whose name matches its own module basename
// (BonDriver_Siano.ini / BonDriver_PX4.ini) from the directory containing the
// shared library.  Relative paths inside the INI resolve against that
// directory, never against the process cwd.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bondriver {

enum class System { IsdbT, IsdbS };

struct ChannelEntry {
	std::string id;
	std::string name;
	System system = System::IsdbT;
	int64_t frequency_hz = 0;
	int stream_id = -1; // isdb-s TSID, -1 when unset
	int slot = -1;      // isdb-s slot, -1 when unset
};

struct Space {
	std::string id;
	std::string name;
	std::vector<ChannelEntry> channels;
};

// One external host_ts client executable and its daemon runtime namespace.
// The label is a configuration name only; it is not a product identity and is
// deliberately absent from the cross-process lease key.
struct ClientConfig {
	std::string name;
	std::string cli_path;
	std::string runtime_dir;
};

// One receiver address inside a client's daemon.
struct ReceiverConfig {
	std::string client;
	std::string instance;
	int receiver = -1;
	bool supports_t = false;
	bool supports_s = false;
	bool hybrid = false;
};

// The tuner set exposed to the host.  Exactly one group is accepted for now.
struct GroupConfig {
	std::string name;
	std::string tuner_name;
	std::string channel_table; // absolute after resolution
	std::vector<ReceiverConfig> receivers;
};

struct ReceiverTarget {
	// Canonical identity used for the cross-process lease key.  Filled from the
	// canonical runtime_dir when the target is claimed.
	std::string key;
	std::string label;
	std::string cli_path;
	std::string runtime_dir; // canonical after a successful claim
	std::string instance;
	int receiver = -1;
	bool supports_t = false;
	bool supports_s = false;
	bool hybrid = false;
};

struct CommonConfig {
	std::string lock_dir;
	uint64_t tune_timeout_ms = 4000;
	uint64_t stop_timeout_ms = 5000;
	uint64_t kill_timeout_ms = 2000;
	size_t buffer_limit_bytes = 8u * 1024u * 1024u;
	std::string diagnostics = "stderr"; // "stderr" | "discard" | "file:PATH"
};

struct Config {
	std::string module_dir;
	std::string config_path;
	CommonConfig common;
	std::vector<ClientConfig> clients;
	GroupConfig group;
	std::vector<Space> spaces;

	// Returns the client entry for a label, or nullptr.
	const ClientConfig *findClient(const std::string &name) const;
	// Returns the space index and channel index for a stable channel id, or
	// false when unknown.  Indices are stable for the life of the config.
	bool findChannel(const std::string &id, size_t &space, size_t &channel) const;
};

// Resolves an existing directory to its filesystem-canonical absolute path so
// symlink/junction aliases map to one receiver identity.  Fails when the path
// cannot be resolved.  Used when a receiver is claimed, not during config load.
bool canonicalizeRuntimeDir(const std::string &path, std::string &out, std::string &error);

// Cross-process lease key.  It intentionally contains no client label, group
// name, tuner name or CLI path: the daemon endpoint is identified only by its
// canonical runtime directory, instance and receiver.
std::string receiverLeaseKey(const std::string &canonical_runtime_dir, const std::string &instance, int receiver);

// Resolves the config file path:
//   1. $BONDRIVER_CONFIG (test-only explicit override)
//   2. <module_dir>/<module_basename>.ini
std::string resolveConfigPath(const std::string &module_path, std::string &module_dir,
                              std::string &module_basename);

// Loads and validates the configuration.  On failure returns false and fills
// *error with a human readable reason.
bool loadConfig(const std::string &module_path, const std::string &override_path, Config &out,
                std::string &error);

// Channel table loading/validation, exposed for unit tests.
bool parseChannelTable(const std::string &text, std::vector<Space> &out, std::string &error);

} // namespace bondriver
