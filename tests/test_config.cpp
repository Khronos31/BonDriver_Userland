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

std::string writeAndLoad(const std::string &ini, const std::string &table)
{
	testutil::writeFile(path("case.ini"), ini);
	testutil::writeFile(path("channels.tsv"), table);
	Config config;
	std::string error;
	const bool ok = loadConfig(path("case.ini"), path("case.ini"), config, error);
	if (!ok) {
		return error;
	}
	return std::string();
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

[group.test]
tuner_name = Path With Spaces Tuner
channel_table = channels.tsv
receivers = px4:tok:0:TS
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

std::string iniWithReceivers(const std::string &receivers)
{
	std::string ini = kIni;
	const std::string marker = "receivers = px4:tok:0:TS";
	const size_t at = ini.find(marker);
	if (at != std::string::npos) {
		ini.replace(at, marker.size(), "receivers = " + receivers);
	}
	return ini;
}

const char *kTable = "space_id\tspace_name\tchannel_id\tchannel_name\tsystem\tfrequency_hz\tstream_id\tslot\n"
                     "GR\t地上波\tT13\t東京\tisdb-t\t473142857\t-\t-\n"
                     "GR\t地上波\tT14\t大阪\tisdb-t\t479142857\t-\t-\n";

} // namespace

int main()
{
	testutil::makeDir(testutil::tempRoot());
	// Directory with spaces exercises relative path resolution.
	g_root = testutil::tempRoot() + "/cfg dir";
	testutil::makeDir(g_root);
	testutil::makeDir(path("runtime"));
	testutil::makeDir(path("runtime2"));

	{
		Config config;
		std::string error;
		testutil::writeFile(path("case.ini"), kIni);
		testutil::writeFile(path("channels.tsv"), kTable);
		const bool ok = loadConfig(path("case.ini"), path("case.ini"), config, error);
		testutil::expect(ok, "valid config loads: " + error);
		if (ok) {
			testutil::expect(config.group.tuner_name == "Path With Spaces Tuner", "tuner name preserved");
			testutil::expect(config.clients.size() == 1 && config.clients[0].name == "px4", "client parsed");
			testutil::expect(config.clients[0].cli_path == g_root + "/bin/fake cli", "cli path resolved with spaces");
			testutil::expect(config.clients[0].runtime_dir == g_root + "/runtime", "runtime dir resolved");
			testutil::expect(config.group.channel_table == g_root + "/channels.tsv", "channel table resolved");
			testutil::expect(config.common.lock_dir == g_root + "/locks", "lock dir resolved");
			testutil::expect(config.common.buffer_limit_bytes == 1880, "buffer limit parsed");
			testutil::expect(config.common.diagnostics == "discard", "diagnostics parsed");
			testutil::expect(config.group.receivers.size() == 1, "receiver parsed");
			testutil::expect(config.group.receivers[0].supports_t && config.group.receivers[0].supports_s,
			                 "hybrid receiver parsed");
			testutil::expect(config.spaces.size() == 1 && config.spaces[0].channels.size() == 2, "channels parsed");
			testutil::expect(config.spaces[0].channels[0].id == "T13", "stable order");
		}
	}

	{
		// The accepted config ceiling is 16 GiB, but a 32-bit process must also
		// reject values that cannot be represented by size_t before narrowing.
		const uint64_t accepted_max = std::min<uint64_t>(1ull << 34,
		                                                static_cast<uint64_t>((std::numeric_limits<size_t>::max)()));
		Config boundary;
		std::string error;
		testutil::writeFile(path("case.ini"), iniWithBufferLimit(accepted_max));
		testutil::writeFile(path("channels.tsv"), kTable);
		const bool loaded = loadConfig(path("case.ini"), path("case.ini"), boundary, error);
		testutil::expect(loaded, "largest platform-representable buffer limit loads: " + error);
		if (loaded) {
			testutil::expect(boundary.common.buffer_limit_bytes == static_cast<size_t>(accepted_max),
			                 "buffer limit remains exact at platform boundary");
		}
		const std::string overflow_error = writeAndLoad(iniWithBufferLimit(accepted_max + 1), kTable);
		testutil::expect(!overflow_error.empty(), "one byte above platform/config ceiling is rejected");
	}

	testutil::expect(
	    !writeAndLoad(kIni, std::string(kTable) + "GR\t地上波\tT13\tx\tisdb-t\t480000000\t-\t-\n").empty(),
	    "duplicate channel id rejected");
	testutil::expect(!writeAndLoad(kIni, "GR\tg\tX\tx\tisdb-x\t473142857\t-\t-\n").empty(),
	                 "unknown system rejected");
	testutil::expect(!writeAndLoad(kIni, "GR\tg\tX\tx\tisdb-t\t473142\t-\t-\n").empty(),
	                 "mixed units (kHz) rejected");
	testutil::expect(!writeAndLoad(kIni, "GR\tg\tX\tx\tisdb-t\t473142857\t1\t-\n").empty(),
	                 "isdb-t with stream id rejected");
	testutil::expect(
	    !writeAndLoad(iniWithReceivers("px4:tok:0:T"), "BS\tb\tB1\tb\tisdb-s\t1100000000\t1\t-\n").empty(),
	    "isdb-s table rejected when the group has no isdb-s receiver");
	testutil::expect(
	    writeAndLoad(iniWithReceivers("px4:tok:0:S"), "BS\tb\tB1\tb\tisdb-s\t1100000000\t1\t-\n").empty(),
	    "declared isdb-s receiver covers an isdb-s table (no product capability inference)");

	{
		const std::string err = writeAndLoad(
		    "[common]\ntune_timeout_ms = 150\n[common]\nstop_timeout_ms = 250\n"
		    "[client.px4]\ncli_path = c\nruntime_dir = runtime\n"
		    "[group.test]\ntuner_name = T\nchannel_table = channels.tsv\nreceivers = px4:tok:0:T\n",
		    kTable);
		testutil::expect(err.find("duplicate section") != std::string::npos,
		                 "reappearing section rejected even without key collision: " + err);
	}
	{
		const std::string err = writeAndLoad(
		    std::string(kIni) + "[extra]\nkey = 1\n", kTable);
		testutil::expect(err.find("unknown section") != std::string::npos, "unknown section rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(
		    "[common]\ntune_timeout_ms = 150\n"
		    "[client.px4]\ncli_path = c\nruntime_dir = runtime\nbogus = 1\n"
		    "[group.test]\ntuner_name = T\nchannel_table = channels.tsv\nreceivers = px4:tok:0:T\n",
		    kTable);
		testutil::expect(err.find("unknown option") != std::string::npos, "unknown key rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(
		    "[common]\ntune_timeout_ms = 150\n"
		    "[client.px4]\ncli_path = c\nruntime_dir = runtime\n"
		    "[group.test]\nchannel_table = channels.tsv\nreceivers = px4:tok:0:T\n",
		    kTable);
		testutil::expect(err.find("tuner_name") != std::string::npos, "missing required key rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(iniWithReceivers("px4:tok:0:T;px4:tok:0:T"), kTable);
		testutil::expect(err.find("duplicate receiver") != std::string::npos,
		                 "duplicate receiver rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(iniWithReceivers("px4:tok:0:T;other:tok:0:T"), kTable);
		testutil::expect(err.find("unknown client") != std::string::npos, "unknown client rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(iniWithReceivers("px4:-lead ing:0:T"), kTable);
		testutil::expect(err.find("invalid instance") != std::string::npos, "instance with space/leading dash rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(iniWithReceivers("px4:bad/name:0:T"), kTable);
		testutil::expect(err.find("invalid instance") != std::string::npos, "instance with slash rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(iniWithReceivers("px4:con:0:T"), kTable);
		testutil::expect(err.find("invalid instance") != std::string::npos, "reserved instance rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(iniWithReceivers("px4:" + std::string(65, 'a') + ":0:T"), kTable);
		testutil::expect(err.find("invalid instance") != std::string::npos, "65-byte instance rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(
		    std::string(kIni) +
		        "[group.second]\ntuner_name = T2\nchannel_table = channels.tsv\nreceivers = px4:tok:1:T\n",
		    kTable);
		testutil::expect(err.find("exactly one") != std::string::npos, "multiple groups are rejected: " + err);
	}
	{
		// Separate labels may point at the same executable/runtime.  The lease
		// key is label-free, so this is a valid configuration.
		const std::string err = writeAndLoad(
		    std::string(kIni) +
		        "[client.other]\ncli_path = bin/fake cli\nruntime_dir = runtime\n",
		    kTable);
		testutil::expect(err.empty(), "same cli_path/runtime under a second label is valid: " + err);
	}
	{
		// The shipped examples must parse with the documented syntax.  Replace
		// only the channel-table filename so the test stays self-contained.
		for (const char *name : {"BonDriver_PX4.ini.example", "BonDriver_Siano.ini.example"}) {
			std::string example = testutil::readFile(std::string(BONDRIVER_SOURCE_DIR) + "/config/" + name);
			const bool px4 = std::string(name).find("PX4") != std::string::npos;
			const std::string table =
			    std::string(BONDRIVER_SOURCE_DIR) + (px4 ? "/config/channels-all.example.tsv"
			                                             : "/config/channels-ground.example.tsv");
			for (const char *old_table : {"BonDriver_PX4.channels.tsv", "BonDriver_Siano.channels.tsv"}) {
				const size_t at = example.find(old_table);
				if (at != std::string::npos) {
					example.replace(at, std::string(old_table).size(), table);
				}
			}
			Config config;
			std::string error;
			testutil::writeFile(path("example.ini"), example);
			const bool ok = loadConfig(path("example.ini"), path("example.ini"), config, error);
			testutil::expect(ok, std::string("shipped example parses: ") + name + ": " + error);
			if (ok) {
				testutil::expect(!config.group.receivers.empty(), std::string("example has receivers: ") + name);
				testutil::expect(config.group.name == (px4 ? "PX4" : "Siano"),
				                 std::string("example group name: ") + name);
			}
		}
	}

	return testutil::report("test_config");
}
