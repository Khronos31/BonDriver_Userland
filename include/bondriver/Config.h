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

enum class BackendKind { Siano, Px4 };
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

struct ReceiverTarget {
	// Canonical identity used for the cross-process lease key.  Two settings
	// that resolve to the same physical receiver share this string.
	std::string key;
	std::string label;
	bool supports_t = false;
	bool supports_s = false;
	bool hybrid = false;
	// Backend specific addressing.
	std::string selector; // Siano --device
	std::string instance; // PX4 --instance
	int receiver = -1;    // PX4 --receiver
};

struct CommonConfig {
	std::string tuner_name;
	std::string cli_path;
	std::string channel_table; // absolute after resolution
	std::string lock_dir;
	uint64_t tune_timeout_ms = 4000;
	uint64_t stop_timeout_ms = 5000;
	uint64_t kill_timeout_ms = 2000;
	size_t buffer_limit_bytes = 8u * 1024u * 1024u;
	std::string diagnostics = "stderr"; // "stderr" | "discard" | "file:PATH"
};

struct SianoConfig {
	std::string firmware;              // optional; empty omits --firmware
	bool pool_mode = false;
	std::string device;                // fixed selector
	std::vector<std::string> devices;  // pool selectors, ordered
};

struct Px4TargetConfig {
	std::string instance;
	int receiver = -1;
	bool supports_t = false;
	bool supports_s = false;
};

struct Px4Config {
	std::string runtime_dir;
	bool pool_mode = false;
	Px4TargetConfig fixed;
	std::vector<Px4TargetConfig> targets;
	int lnb_voltage = 0; // 0 or 15
	bool allow_lnb_15 = false;
};

struct Config {
	BackendKind kind = BackendKind::Siano;
	std::string module_dir;
	std::string config_path;
	CommonConfig common;
	SianoConfig siano;
	Px4Config px4;
	std::vector<Space> spaces;

	// Returns the space index and channel index for a stable channel id, or
	// false when unknown.  Indices are stable for the life of the config.
	bool findChannel(const std::string &id, size_t &space, size_t &channel) const;
};

// Resolves the config file path:
//   1. $BONDRIVER_<BACKEND>_CONFIG (test-only explicit override)
//   2. $BONDRIVER_CONFIG (test-only explicit override)
//   3. <module_dir>/<module_basename>.ini
std::string resolveConfigPath(const std::string &module_path, BackendKind kind,
                              std::string &module_dir, std::string &module_basename);

// Loads and validates the configuration.  On failure returns false and fills
// *error with a human readable reason.
bool loadConfig(BackendKind kind, const std::string &module_path, const std::string &override_path,
                Config &out, std::string &error);

// Channel table loading/validation, exposed for unit tests.
bool parseChannelTable(const std::string &text, BackendKind kind, std::vector<Space> &out,
                       std::string &error);

} // namespace bondriver
