#include "bondriver/Config.h"
#include "test_util.h"

#include <algorithm>
#include <limits>
#include <string>

using namespace bondriver;

namespace {

std::string g_root;

std::string path(const std::string &name)
{
	return g_root + "/" + name;
}

std::string writeAndLoadTable(const std::string &ini, const std::string &table, const std::string &module_basename)
{
	testutil::writeFile(path("case.ini"), ini);
	testutil::writeFile(path("t.tsv"), table);
	testutil::writeFile(path("s.tsv"),
	                    "space_id\tspace_name\tchannel_id\tchannel_name\tsystem\tfrequency_hz\tstream_id\tslot\n"
	                    "BS\tBS\tBS01_0\tBS01_0\tisdb-s\t1100000000\t1\t-\n");
	Config config;
	std::string error;
	const bool ok = loadConfig(path(module_basename + ".so"), path("case.ini"), config, error);
	return ok ? std::string() : error;
}

std::string writeAndLoad(const std::string &ini, const std::string &module_basename)
{
	return writeAndLoadTable(
	    ini,
	    "space_id\tspace_name\tchannel_id\tchannel_name\tsystem\tfrequency_hz\tstream_id\tslot\n"
	    "GR\tGR\tT13\tT13\tisdb-t\t473142857\t-\t-\n",
	    module_basename);
}

const char *kIni = R"INI([common]
tune_timeout_ms = 150
stop_timeout_ms = 250
kill_timeout_ms = 100
buffer_limit_bytes = 1880
diagnostics = discard
lock_dir = locks

[client.px4]
cli_path = bin/fake cli
runtime_dir = runtime

[group.BonDriver_Userland_T]
tuner_name = Path With Spaces Tuner
channel_table = t.tsv
receivers = px4:tok:0:T

[group.BonDriver_Userland_S]
tuner_name = Satellite Tuner
channel_table = s.tsv
receivers = px4:sat:0:S
)INI";

std::string iniWithBufferLimit(uint64_t value)
{
	std::string ini = kIni;
	const std::string marker = "buffer_limit_bytes = 1880";
	const size_t at = ini.find(marker);
	if (at != std::string::npos) {
		ini.replace(at, marker.size(), "buffer_limit_bytes = " + std::to_string(value));
	}
	return ini;
}

std::string iniWithReceivers(const std::string &receivers, const std::string &group = "BonDriver_Userland_T")
{
	std::string ini = kIni;
	const std::string marker = group == "BonDriver_Userland_T" ? "receivers = px4:tok:0:T"
	                                                            : "receivers = px4:sat:0:S";
	const size_t at = ini.find(marker);
	if (at != std::string::npos) {
		ini.replace(at, marker.size(), "receivers = " + receivers);
	}
	return ini;
}

bool hasSystem(const std::vector<Space> &spaces, System system)
{
	for (const Space &space : spaces) {
		for (const ChannelEntry &channel : space.channels) {
			if (channel.system == system) {
				return true;
			}
		}
	}
	return false;
}

} // namespace

int main()
{
	testutil::makeDir(testutil::tempRoot());
	// Directory with spaces exercises relative path resolution.
	g_root = testutil::tempRoot() + "/cfg dir";
	testutil::makeDir(g_root);
	testutil::makeDir(path("runtime"));

	{
		Config config;
		std::string error;
		testutil::writeFile(path("case.ini"), kIni);
		testutil::writeFile(path("t.tsv"),
		                    "space_id\tspace_name\tchannel_id\tchannel_name\tsystem\tfrequency_hz\tstream_id\tslot\n"
		                    "GR\tGR\tT13\tT13\tisdb-t\t473142857\t-\t-\n");
		testutil::writeFile(path("s.tsv"),
		                    "space_id\tspace_name\tchannel_id\tchannel_name\tsystem\tfrequency_hz\tstream_id\tslot\n"
		                    "BS\tBS\tBS01_0\tBS01_0\tisdb-s\t1100000000\t1\t-\n");
		const bool ok = loadConfig(path("BonDriver_Userland_T.so"), path("case.ini"), config, error);
		testutil::expect(ok, "valid config loads: " + error);
		if (ok) {
			testutil::expect(config.group.name == "BonDriver_Userland_T", "basename selects the T group");
			testutil::expect(config.group.tuner_name == "Path With Spaces Tuner", "tuner name preserved");
			testutil::expect(config.groups.size() == 2, "all groups parsed");
			testutil::expect(config.clients.size() == 1 && config.clients[0].name == "px4", "client parsed");
			testutil::expect(config.clients[0].cli_path == g_root + "/bin/fake cli", "cli path resolved with spaces");
			testutil::expect(config.clients[0].runtime_dir == g_root + "/runtime", "runtime dir resolved");
			testutil::expect(config.group.channel_table == g_root + "/t.tsv", "channel table resolved");
			testutil::expect(config.common.lock_dir == g_root + "/locks", "lock dir resolved");
			testutil::expect(config.common.buffer_limit_bytes == 1880, "buffer limit parsed");
			testutil::expect(config.common.diagnostics == "discard", "diagnostics parsed");
			testutil::expect(config.group.receivers.size() == 1, "receiver parsed");
			testutil::expect(config.spaces.size() == 1 && config.spaces[0].channels.size() == 1, "selected table parsed");
			testutil::expect(config.spaces[0].channels[0].id == "T13", "stable order");
		}
	}
	{
		Config config;
		std::string error;
		testutil::writeFile(path("case.ini"), kIni);
		testutil::writeFile(path("t.tsv"), "GR\tGR\tT13\tT13\tisdb-t\t473142857\t-\t-\n");
		testutil::writeFile(path("s.tsv"), "BS\tBS\tBS01_0\tBS01_0\tisdb-s\t1100000000\t1\t-\n");
		const bool ok = loadConfig(path("BonDriver_Userland_T_0.so"), path("case.ini"), config, error);
		testutil::expect(ok && config.group.name == "BonDriver_Userland_T",
		                 "trailing _<digits> is stripped for group selection: " + error);
	}

	{
		// The accepted config ceiling is 16 GiB, but a 32-bit process must also
		// reject values that cannot be represented by size_t before narrowing.
		const uint64_t accepted_max = std::min<uint64_t>(1ull << 34,
		                                                static_cast<uint64_t>((std::numeric_limits<size_t>::max)()));
		Config boundary;
		std::string error;
		testutil::writeFile(path("case.ini"), iniWithBufferLimit(accepted_max));
		testutil::writeFile(path("t.tsv"), "GR\tGR\tT13\tT13\tisdb-t\t473142857\t-\t-\n");
		testutil::writeFile(path("s.tsv"), "BS\tBS\tBS01_0\tBS01_0\tisdb-s\t1100000000\t1\t-\n");
		const bool loaded = loadConfig(path("BonDriver_Userland_T.so"), path("case.ini"), boundary, error);
		testutil::expect(loaded, "largest platform-representable buffer limit loads: " + error);
		if (loaded) {
			testutil::expect(boundary.common.buffer_limit_bytes == static_cast<size_t>(accepted_max),
			                 "buffer limit remains exact at platform boundary");
		}
		const std::string overflow_error = writeAndLoad(iniWithBufferLimit(accepted_max + 1), "BonDriver_Userland_T");
		testutil::expect(!overflow_error.empty(), "one byte above platform/config ceiling is rejected");
	}

	{
		const std::string err = writeAndLoad(
		    "[common]\ntune_timeout_ms = 150\n[common]\nstop_timeout_ms = 250\n"
		    "[client.px4]\ncli_path = c\nruntime_dir = runtime\n"
		    "[group.BonDriver_Userland_T]\ntuner_name = T\nchannel_table = t.tsv\nreceivers = px4:tok:0:T\n",
		    "BonDriver_Userland_T");
		testutil::expect(err.find("duplicate section") != std::string::npos,
		                 "reappearing section rejected even without key collision: " + err);
	}
	{
		const std::string err = writeAndLoad(std::string(kIni) + "[extra]\nkey = 1\n", "BonDriver_Userland_T");
		testutil::expect(err.find("unknown section") != std::string::npos, "unknown section rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(
		    "[common]\ntune_timeout_ms = 150\n"
		    "[client.px4]\ncli_path = c\nruntime_dir = runtime\nbogus = 1\n"
		    "[group.BonDriver_Userland_T]\ntuner_name = T\nchannel_table = t.tsv\nreceivers = px4:tok:0:T\n",
		    "BonDriver_Userland_T");
		testutil::expect(err.find("unknown option") != std::string::npos, "unknown key rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(
		    "[common]\ntune_timeout_ms = 150\n"
		    "[client.px4]\ncli_path = c\nruntime_dir = runtime\n"
		    "[group.BonDriver_Userland_T]\nchannel_table = t.tsv\nreceivers = px4:tok:0:T\n",
		    "BonDriver_Userland_T");
		testutil::expect(err.find("tuner_name") != std::string::npos, "missing required key rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(iniWithReceivers("px4:tok:0:T;px4:tok:0:T"), "BonDriver_Userland_T");
		testutil::expect(err.find("duplicate receiver") != std::string::npos,
		                 "duplicate receiver rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(iniWithReceivers("px4:tok:0:T;other:tok:0:T"), "BonDriver_Userland_T");
		testutil::expect(err.find("unknown client") != std::string::npos, "unknown client rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(iniWithReceivers("px4:-lead ing:0:T"), "BonDriver_Userland_T");
		testutil::expect(err.find("invalid instance") != std::string::npos, "instance with space/leading dash rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(iniWithReceivers("px4:bad/name:0:T"), "BonDriver_Userland_T");
		testutil::expect(err.find("invalid instance") != std::string::npos, "instance with slash rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(iniWithReceivers("px4:con:0:T"), "BonDriver_Userland_T");
		testutil::expect(err.find("invalid instance") != std::string::npos, "reserved instance rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(iniWithReceivers("px4:" + std::string(65, 'a') + ":0:T"), "BonDriver_Userland_T");
		testutil::expect(err.find("invalid instance") != std::string::npos, "65-byte instance rejected: " + err);
	}
	{
		const std::string err = writeAndLoadTable(
		    kIni,
		    "space_id\tspace_name\tchannel_id\tchannel_name\tsystem\tfrequency_hz\tstream_id\tslot\n"
		    "GR\tGR\tT13\tT13\tisdb-t\t473142857\t-\t-\n"
		    "GR\tGR\tT13\tT13\tisdb-t\t479142857\t-\t-\n",
		    "BonDriver_Userland_T");
		testutil::expect(err.find("duplicate channel id") != std::string::npos,
		                 "duplicate channel id rejected: " + err);
	}
	{
		const std::string err = writeAndLoadTable(
		    kIni,
		    "space_id\tspace_name\tchannel_id\tchannel_name\tsystem\tfrequency_hz\tstream_id\tslot\n"
		    "GR\tGR\tT13\tT13\tisdb-x\t473142857\t-\t-\n",
		    "BonDriver_Userland_T");
		testutil::expect(err.find("unknown system") != std::string::npos, "unknown system rejected: " + err);
	}
	{
		const std::string err = writeAndLoadTable(
		    kIni,
		    "space_id\tspace_name\tchannel_id\tchannel_name\tsystem\tfrequency_hz\tstream_id\tslot\n"
		    "GR\tGR\tT13\tT13\tisdb-t\t473142\t-\t-\n",
		    "BonDriver_Userland_T");
		testutil::expect(err.find("units must be Hz") != std::string::npos,
		                 "kHz unit in a T table rejected: " + err);
	}
	{
		const std::string err = writeAndLoadTable(
		    kIni,
		    "space_id\tspace_name\tchannel_id\tchannel_name\tsystem\tfrequency_hz\tstream_id\tslot\n"
		    "GR\tGR\tT13\tT13\tisdb-t\t473142857\t1\t-\n",
		    "BonDriver_Userland_T");
		testutil::expect(err.find("isdb-t must not set stream_id") != std::string::npos,
		                 "isdb-t table with stream_id rejected: " + err);
	}
	{
		// An S-only group must not expose a T table.
		std::string ini = kIni;
		const std::string s_marker = "[group.BonDriver_Userland_S]\ntuner_name = Satellite Tuner\nchannel_table = s.tsv";
		const size_t at = ini.find(s_marker);
		if (at != std::string::npos) {
			ini.replace(at, s_marker.size(),
			            "[group.BonDriver_Userland_S]\ntuner_name = S\nchannel_table = t.tsv");
		}
		const std::string err = writeAndLoad(ini, "BonDriver_Userland_S");
		testutil::expect(err.find("isdb-t") != std::string::npos, "S-only group rejects a T table: " + err);
	}
	{
		// A T-only group must not expose an S table.
		std::string ini = kIni;
		const std::string t_marker = "[group.BonDriver_Userland_T]\ntuner_name = Path With Spaces Tuner\nchannel_table = t.tsv";
		const size_t at = ini.find(t_marker);
		if (at != std::string::npos) {
			ini.replace(at, t_marker.size(),
			            "[group.BonDriver_Userland_T]\ntuner_name = T\nchannel_table = s.tsv");
		}
		const std::string err = writeAndLoad(ini, "BonDriver_Userland_T");
		testutil::expect(err.find("isdb-s") != std::string::npos, "T-only group rejects an S table: " + err);
	}
	{
		const std::string err = writeAndLoad(kIni, "BonDriver_Userland_X");
		testutil::expect(err.find("no [group.NAME] matches") != std::string::npos,
		                 "unknown module basename is rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(kIni, "BonDriver_Userland");
		testutil::expect(err.find("no [group.NAME] matches") != std::string::npos,
		                 "missing exact group is rejected: " + err);
	}
	{
		// Separate labels may point at the same executable/runtime.  The lease
		// key is label-free, so this is a valid configuration.
		const std::string err = writeAndLoad(
		    std::string(kIni) + "[client.other]\ncli_path = bin/fake cli\nruntime_dir = runtime\n",
		    "BonDriver_Userland_T");
		testutil::expect(err.empty(), "same cli_path/runtime under a second label is valid: " + err);
	}

	{
		// The shipped representative example: T12 / S8 / TS3 / all23.
		const std::string example = std::string(BONDRIVER_SOURCE_DIR) + "/config/BonDriver_Userland.ini.example";
		struct Case {
			const char *basename;
			const char *group;
			size_t count;
			bool has_t;
			bool has_s;
		};
		const Case cases[] = {
		    {"BonDriver_Userland_T", "BonDriver_Userland_T", 12, true, false},
		    {"BonDriver_Userland_S", "BonDriver_Userland_S", 8, false, true},
		    {"BonDriver_Userland_TS", "BonDriver_Userland_TS", 3, true, true},
		    {"BonDriver_Userland", "BonDriver_Userland", 23, true, true},
		};
		for (const Case &c : cases) {
			Config config;
			std::string error;
			const bool ok = loadConfig(path(std::string(c.basename) + ".so"), example, config, error);
			testutil::expect(ok, std::string("example parses for ") + c.group + ": " + error);
			if (!ok) {
				continue;
			}
			testutil::expect(config.group.name == c.group, std::string("example selects ") + c.group);
			testutil::expect(config.group.receivers.size() == c.count,
			                 std::string("receiver count for ") + c.group);
			testutil::expect(hasSystem(config.spaces, System::IsdbT) == c.has_t,
			                 std::string("T table coverage for ") + c.group);
			testutil::expect(hasSystem(config.spaces, System::IsdbS) == c.has_s,
			                 std::string("S table coverage for ") + c.group);
		}
	}

	return testutil::report("test_config");
}
