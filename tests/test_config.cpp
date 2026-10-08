#include "bondriver/Config.h"
#include "test_util.h"

#include <string>
#include <algorithm>
#include <limits>

using namespace bondriver;

namespace {

std::string g_root;

std::string path(const std::string &name)
{
	return g_root + "/" + name;
}

std::string writeAndLoad(const std::string &ini, const std::string &table, BackendKind kind)
{
	testutil::writeFile(path("case.ini"), ini);
	testutil::writeFile(path("channels.tsv"), table);
	Config config;
	std::string error;
	const bool ok = loadConfig(kind, path("case.ini"), path("case.ini"), config, error);
	if (!ok) {
		return error;
	}
	return std::string();
}

const char *kSianoIni = R"INI([common]
tuner_name = Path With Spaces Tuner
cli_path = bin/fake cli
channel_table = channels.tsv
tune_timeout_ms = 150
stop_timeout_ms = 250
kill_timeout_ms = 100
buffer_limit_bytes = 1880
diagnostics = discard

[siano]
device = 0
firmware = fw.bin
)INI";

std::string iniWithBufferLimit(uint64_t value)
{
	std::string ini = kSianoIni;
	const std::string marker = "buffer_limit_bytes = 1880";
	const size_t at = ini.find(marker);
	if (at != std::string::npos) {
		ini.replace(at, marker.size(), "buffer_limit_bytes = " + std::to_string(value));
	}
	return ini;
}

const char *kTable = "space_id\tspace_name\tchannel_id\tchannel_name\tsystem\tfrequency_hz\tstream_id\tslot\n"
                     "GR\t地上波\tT13\t東京\tisdb-t\t473142857\t-\t-\n"
                     "GR\t地上波\tT14\t大阪\tisdb-t\t479142857\t-\t-\n";

} // namespace

int main()
{
	g_root = testutil::tempRoot() + "-config";
	testutil::makeDir(testutil::tempRoot());
	// Directory with spaces exercises relative path resolution.
	g_root = testutil::tempRoot() + "/cfg dir";
	testutil::makeDir(testutil::tempRoot());
	testutil::makeDir(g_root);
	testutil::makeDir(path("runtime"));
	testutil::makeDir(path("runtime2"));

	{
		Config config;
		std::string error;
		testutil::writeFile(path("case.ini"), kSianoIni);
		testutil::writeFile(path("channels.tsv"), kTable);
		const bool ok = loadConfig(BackendKind::Siano, path("case.ini"), path("case.ini"), config, error);
		testutil::expect(ok, "valid config loads: " + error);
		if (ok) {
			testutil::expect(config.common.tuner_name == "Path With Spaces Tuner", "tuner name preserved");
			testutil::expect(config.common.cli_path == g_root + "/bin/fake cli", "cli path resolved with spaces");
			testutil::expect(config.common.channel_table == g_root + "/channels.tsv", "channel table resolved");
			testutil::expect(config.common.buffer_limit_bytes == 1880, "buffer limit parsed");
			testutil::expect(config.common.diagnostics == "discard", "diagnostics parsed");
			testutil::expect(config.siano.firmware == g_root + "/fw.bin", "firmware resolved");
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
		const bool loaded = loadConfig(BackendKind::Siano, path("case.ini"), path("case.ini"), boundary, error);
		testutil::expect(loaded, "largest platform-representable buffer limit loads: " + error);
		if (loaded) {
			testutil::expect(boundary.common.buffer_limit_bytes == static_cast<size_t>(accepted_max),
			                 "buffer limit remains exact at platform boundary");
		}
		const std::string overflow_error = writeAndLoad(iniWithBufferLimit(accepted_max + 1), kTable,
		                                                BackendKind::Siano);
		testutil::expect(!overflow_error.empty(), "one byte above platform/config ceiling is rejected");
	}

	testutil::expect(
	    !writeAndLoad(kSianoIni, std::string(kTable) + "GR\t地上波\tT13\tx\tisdb-t\t480000000\t-\t-\n",
	                  BackendKind::Siano)
	         .empty(),
	    "duplicate channel id rejected");
	testutil::expect(!writeAndLoad(kSianoIni, "GR\tg\tX\tx\tisdb-x\t473142857\t-\t-\n", BackendKind::Siano).empty(),
	                 "unknown system rejected");
	testutil::expect(!writeAndLoad(kSianoIni, "GR\tg\tX\tx\tisdb-t\t473142\t-\t-\n", BackendKind::Siano).empty(),
	                 "mixed units (kHz) rejected");
	testutil::expect(!writeAndLoad(kSianoIni, "BS\tb\tB1\tb\tisdb-s\t1100000000\t1\t1\n", BackendKind::Siano).empty(),
	                 "isdb-s rejected by Siano");
	testutil::expect(!writeAndLoad(kSianoIni, "GR\tg\tX\tx\tisdb-t\t473142857\t1\t-\n", BackendKind::Siano).empty(),
	                 "isdb-t with stream id rejected");
	testutil::expect(
	    !writeAndLoad("bad", "GR\tg\tX\tx\tisdb-t\t473142857\t-\t-\n", BackendKind::Siano)
	         .empty(),
	    "malformed ini rejected");

	{
		const std::string err = writeAndLoad(
		    "[common]\ntuner_name = T\ncli_path = c\nchannel_table = channels.tsv\n"
		    "[common]\ntuner_name = T2\n",
		    kTable, BackendKind::Siano);
		testutil::expect(err.find("duplicate") != std::string::npos, "duplicate section key rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(
		    "[common]\ntuner_name = T\ncli_path = c\nchannel_table = channels.tsv\nbogus = 1\n", kTable, BackendKind::Siano);
		testutil::expect(err.find("unknown option") != std::string::npos, "unknown key rejected: " + err);
	}
	{
		const std::string err = writeAndLoad("[common]\ncli_path = c\nchannel_table = channels.tsv\n", kTable,
		                                     BackendKind::Siano);
		testutil::expect(err.find("tuner_name") != std::string::npos, "missing required key rejected: " + err);
	}
	{
		// Duplicate PX4 receivers.
		const std::string err = writeAndLoad(
		    "[common]\ntuner_name = T\ncli_path = c\nchannel_table = channels.tsv\n"
		    "[px4]\nruntime_dir = " +
		        path("runtime") + "\nreceivers = tok:0:T;tok:0:T\n",
		    "GR\tg\tX\tx\tisdb-t\t473142857\t-\t-\n", BackendKind::Px4);
		testutil::expect(err.find("duplicate PX4 receiver") != std::string::npos, "duplicate px4 receiver rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(
		    "[common]\ntuner_name = T\ncli_path = c\nchannel_table = channels.tsv\n"
		    "[px4]\nruntime_dir = " +
		        path("runtime") + "\ninstance = tok\nreceiver = 0\nlnb_voltage = 15\n",
		    "GR\tg\tX\tx\tisdb-t\t473142857\t-\t-\n", BackendKind::Px4);
		testutil::expect(err.find("allow_lnb_15") != std::string::npos, "lnb 15 without permission rejected: " + err);
	}
	{
		const std::string err = writeAndLoad(
		    "[common]\ntuner_name = T\ncli_path = c\nchannel_table = channels.tsv\n"
		    "[px4]\nruntime_dir = " +
		        path("runtime") + "\nreceivers = tok:0:S\n",
		    "GR\tg\tX\tx\tisdb-t\t473142857\t-\t-\n", BackendKind::Px4);
		testutil::expect(err.find("isdb-t") != std::string::npos, "unsupported system for px4 targets rejected: " + err);
	}

	{
		// A runtime directory that cannot be resolved must fail, not silently
		// form a separate lexical identity.
		const std::string err = writeAndLoad(
		    "[common]\ntuner_name = T\ncli_path = c\nchannel_table = channels.tsv\n"
		    "[px4]\nruntime_dir = " +
		        path("does-not-exist") + "\nreceivers = tok:0:T\n",
		    "GR\tg\tX\tx\tisdb-t\t473142857\t-\t-\n", BackendKind::Px4);
		testutil::expect(err.find("not found") != std::string::npos, "unresolved runtime_dir rejected: " + err);
	}

	return testutil::report("test_config");
}
